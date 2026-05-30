/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "host_dram_pool.h"
#include <cstddef>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include "logger/logger.h"
#include "trans/buffer.h"
#include "trans/device.h"

namespace UC::RemoteMemStore {

namespace {
constexpr size_t kPageSize = 4096;

size_t AlignUp(size_t value, size_t align) { return (value + align - 1) / align * align; }
}  // namespace

HostDramPool::~HostDramPool()
{
    if (registered_ && data_) { Trans::Buffer::UnregisterHostBuffer(data_.get()); }
}

std::shared_ptr<void> HostDramPool::AllocateAligned(size_t size)
{
    const auto aligned = AlignUp(size, kPageSize);
#if defined(_WIN32)
    void* ptr = _aligned_malloc(aligned, kPageSize);
    if (!ptr) { return nullptr; }
    return std::shared_ptr<void>(ptr, [](void* p) { _aligned_free(p); });
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, kPageSize, aligned) != 0) { return nullptr; }
    return std::shared_ptr<void>(ptr, std::free);
#endif
}

Status HostDramPool::Setup(const Config& config)
{
    if (config.shardSize == 0) { return Status::InvalidParam("invalid remote mem shard size"); }
    slotSize_ = config.shardSize;
    capacity_ = config.dramPoolCapacity / slotSize_ * slotSize_;
    nSlots_ = capacity_ / slotSize_;
    if (nSlots_ == 0) {
        return Status::InvalidParam("remote mem pool capacity({}) is smaller than shard size({})",
                                    config.dramPoolCapacity, slotSize_);
    }

    if (config.registerHostBuffer && config.deviceId >= 0) {
        Trans::Device device;
        auto s = device.Setup(config.deviceId);
        if (s.Failure()) { return s; }
        data_ = AllocateAligned(capacity_);
        if (!data_) { return Status::OutOfMemory(); }
        s = Trans::Buffer::RegisterHostBuffer(data_.get(), capacity_, &dataOnDevice_);
        if (s.Failure()) {
            UC_ERROR("Failed({}) to register remote mem DRAM pool({}).", s, capacity_);
            return s;
        }
        registered_ = true;
    } else if (config.deviceId >= 0) {
        Trans::Device device;
        auto s = device.Setup(config.deviceId);
        if (s.Failure()) { return s; }
        auto buffer = device.MakeBuffer();
        if (!buffer) { return Status::OutOfMemory(); }
        data_ = buffer->MakeHostBuffer(capacity_);
        if (!data_) { return Status::OutOfMemory(); }
        dataOnDevice_ = data_.get();
    } else {
        data_ = AllocateAligned(capacity_);
        if (!data_) { return Status::OutOfMemory(); }
        dataOnDevice_ = data_.get();
    }

    freeSlots_.reserve(nSlots_);
    used_.assign(nSlots_, false);
    for (size_t i = 0; i < nSlots_; ++i) { freeSlots_.push_back(nSlots_ - 1 - i); }
    return Status::OK();
}

Expected<size_t> HostDramPool::Allocate()
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (freeSlots_.empty()) { return Status::NoSpace(); }
    const auto slot = freeSlots_.back();
    freeSlots_.pop_back();
    used_[slot] = true;
    return slot;
}

void HostDramPool::Release(size_t slot)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (slot >= nSlots_ || !used_[slot]) { return; }
    used_[slot] = false;
    freeSlots_.push_back(slot);
}

void* HostDramPool::Data(size_t slot) const
{
    auto base = static_cast<std::byte*>(data_.get());
    return base + slot * slotSize_;
}

BufferDesc HostDramPool::Describe(size_t slot) const
{
    return BufferDesc{reinterpret_cast<uint64_t>(Data(slot)), slotSize_, 0};
}

}  // namespace UC::RemoteMemStore
