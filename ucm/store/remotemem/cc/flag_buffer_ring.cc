/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "flag_buffer_ring.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include "logger/logger.h"
#include "trans/buffer.h"
#include "trans/device.h"

namespace UC::RemoteMemStore {

namespace {
constexpr size_t kPageSize = 4096;
constexpr uint64_t kIncomplete = 0;
constexpr uint64_t kComplete = 1;

size_t AlignUp(size_t value, size_t align) { return (value + align - 1) / align * align; }
}  // namespace

FlagBufferRing::~FlagBufferRing()
{
    if (registered_ && data_) { Trans::Buffer::UnregisterHostBuffer(data_.get()); }
}

std::shared_ptr<void> FlagBufferRing::AllocateAligned(size_t size)
{
    const auto aligned = AlignUp(size, kPageSize);
#if defined(_WIN32)
    void* ptr = _aligned_malloc(aligned, kPageSize);
    if (!ptr) { return nullptr; }
    std::memset(ptr, 0, aligned);
    return std::shared_ptr<void>(ptr, [](void* p) { _aligned_free(p); });
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, kPageSize, aligned) != 0) { return nullptr; }
    std::memset(ptr, 0, aligned);
    return std::shared_ptr<void>(ptr, std::free);
#endif
}

Status FlagBufferRing::Setup(const Config& config)
{
    segmentBytes_ = std::max<size_t>(sizeof(uint64_t), config.flagBytes);
    capacity_ = config.flagBufferCapacity / segmentBytes_ * segmentBytes_;
    nSegments_ = capacity_ / segmentBytes_;
    if (nSegments_ == 0) {
        return Status::InvalidParam("flag buffer capacity({}) is smaller than flag bytes({})",
                                    config.flagBufferCapacity, segmentBytes_);
    }

    data_ = AllocateAligned(capacity_);
    if (!data_) { return Status::OutOfMemory(); }
    dataOnDevice_ = data_.get();
    if (config.registerHostBuffer && config.deviceId >= 0) {
        Trans::Device device;
        auto s = device.Setup(config.deviceId);
        if (s.Failure()) { return s; }
        s = Trans::Buffer::RegisterHostBuffer(data_.get(), capacity_, &dataOnDevice_);
        if (s.Failure()) {
            UC_ERROR("Failed({}) to register remote mem flag ring({}).", s, capacity_);
            return s;
        }
        registered_ = true;
    }

    freeSegments_.reserve(nSegments_);
    used_.assign(nSegments_, false);
    for (size_t i = 0; i < nSegments_; ++i) { freeSegments_.push_back(nSegments_ - 1 - i); }
    return Status::OK();
}

Expected<FlagBufferRing::Segment> FlagBufferRing::Allocate()
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (freeSegments_.empty()) { return Status::NoSpace(); }
    const auto idx = freeSegments_.back();
    freeSegments_.pop_back();
    used_[idx] = true;
    Segment segment;
    segment.index = idx;
    segment.addr = static_cast<std::byte*>(data_.get()) + idx * segmentBytes_;
    segment.bytes = segmentBytes_;
    Reset(segment);
    return segment;
}

void FlagBufferRing::Release(const Segment& segment)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (segment.index >= nSegments_ || !used_[segment.index]) { return; }
    used_[segment.index] = false;
    freeSegments_.push_back(segment.index);
}

void FlagBufferRing::Reset(const Segment& segment)
{
    std::memset(segment.addr, 0, segment.bytes);
    auto* flag = reinterpret_cast<uint64_t*>(segment.addr);
    *flag = kIncomplete;
    std::atomic_thread_fence(std::memory_order_release);
}

void FlagBufferRing::MarkComplete(const Segment& segment)
{
    auto* flag = reinterpret_cast<uint64_t*>(segment.addr);
    *flag = kComplete;
    std::atomic_thread_fence(std::memory_order_release);
}

bool FlagBufferRing::Complete(const Segment& segment) const
{
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto* flag = reinterpret_cast<const uint64_t*>(segment.addr);
    return *flag == kComplete;
}

FlagDesc FlagBufferRing::Describe(const Segment& segment) const
{
    return FlagDesc{reinterpret_cast<uint64_t>(segment.addr), segment.bytes, segment.rkey};
}

}  // namespace UC::RemoteMemStore
