/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_HOST_DRAM_POOL_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_HOST_DRAM_POOL_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include "global_config.h"
#include "protocol.h"
#include "status/status.h"

namespace UC::RemoteMemStore {

class HostDramPool {
public:
    ~HostDramPool();
    Status Setup(const Config& config);
    Expected<size_t> Allocate();
    void Release(size_t slot);
    void* Data(size_t slot) const;
    BufferDesc Describe(size_t slot) const;
    size_t SlotSize() const noexcept { return slotSize_; }
    size_t Capacity() const noexcept { return capacity_; }
    void* DataOnDevice() const noexcept { return dataOnDevice_; }

private:
    static std::shared_ptr<void> AllocateAligned(size_t size);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<void> data_{nullptr};
    void* dataOnDevice_{nullptr};
    size_t slotSize_{0};
    size_t capacity_{0};
    size_t nSlots_{0};
    bool registered_{false};
    std::vector<size_t> freeSlots_{};
    std::vector<bool> used_{};
};

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_HOST_DRAM_POOL_H
