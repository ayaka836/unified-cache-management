/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_DEVICE_STAGING_POOL_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_DEVICE_STAGING_POOL_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include "global_config.h"
#include "status/status.h"

namespace UC::RemoteMemStore {

class DeviceStagingPool {
public:
    struct Segment {
        size_t index{0};
        void* addr{nullptr};
        size_t bytes{0};
    };

public:
    Status Setup(const Config& config);
    Expected<Segment> Allocate();
    void Release(const Segment& segment);
    bool Enabled() const noexcept { return enabled_; }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<void> data_{nullptr};
    size_t slotSize_{0};
    size_t nSlots_{0};
    bool enabled_{false};
    std::vector<size_t> freeSlots_{};
    std::vector<bool> used_{};
};

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_DEVICE_STAGING_POOL_H
