/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "device_staging_pool.h"
#include "trans/device.h"

namespace UC::RemoteMemStore {

Status DeviceStagingPool::Setup(const Config& config)
{
    enabled_ = config.addressMode == AddressMode::DEVICE && config.deviceStagingEnable;
    if (!enabled_) { return Status::OK(); }
    if (config.deviceId < 0) { return Status::InvalidParam("invalid staging device id"); }
    if (config.shardSize == 0 || config.deviceStagingSlots == 0) {
        return Status::InvalidParam("invalid staging config({},{})", config.shardSize,
                                    config.deviceStagingSlots);
    }

    Trans::Device device;
    auto s = device.Setup(config.deviceId);
    if (s.Failure()) { return s; }
    auto buffer = device.MakeBuffer();
    if (!buffer) { return Status::OutOfMemory(); }

    slotSize_ = config.shardSize;
    nSlots_ = config.deviceStagingSlots;
    data_ = buffer->MakeDeviceBuffer(slotSize_ * nSlots_);
    if (!data_) { return Status::OutOfMemory(); }

    freeSlots_.reserve(nSlots_);
    used_.assign(nSlots_, false);
    for (size_t i = 0; i < nSlots_; ++i) { freeSlots_.push_back(nSlots_ - 1 - i); }
    return Status::OK();
}

Expected<DeviceStagingPool::Segment> DeviceStagingPool::Allocate()
{
    if (!enabled_) { return Status::Unsupported(); }
    std::lock_guard<std::mutex> lock{mutex_};
    if (freeSlots_.empty()) { return Status::NoSpace(); }
    const auto slot = freeSlots_.back();
    freeSlots_.pop_back();
    used_[slot] = true;
    return Segment{slot, static_cast<std::byte*>(data_.get()) + slot * slotSize_, slotSize_};
}

void DeviceStagingPool::Release(const Segment& segment)
{
    if (!enabled_) { return; }
    std::lock_guard<std::mutex> lock{mutex_};
    if (segment.index >= nSlots_ || !used_[segment.index]) { return; }
    used_[segment.index] = false;
    freeSlots_.push_back(segment.index);
}

}  // namespace UC::RemoteMemStore
