/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_FLAG_BUFFER_RING_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_FLAG_BUFFER_RING_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "global_config.h"
#include "protocol.h"
#include "status/status.h"

namespace UC::RemoteMemStore {

class FlagBufferRing {
public:
    struct Segment {
        size_t index{0};
        void* addr{nullptr};
        size_t bytes{0};
        uint32_t rkey{0};
    };

public:
    ~FlagBufferRing();
    Status Setup(const Config& config);
    Expected<Segment> Allocate();
    void Release(const Segment& segment);
    void Reset(const Segment& segment);
    void MarkComplete(const Segment& segment);
    bool Complete(const Segment& segment) const;
    FlagDesc Describe(const Segment& segment) const;
    void* DataOnDevice() const noexcept { return dataOnDevice_; }

private:
    static std::shared_ptr<void> AllocateAligned(size_t size);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<void> data_{nullptr};
    void* dataOnDevice_{nullptr};
    size_t segmentBytes_{0};
    size_t capacity_{0};
    size_t nSegments_{0};
    bool registered_{false};
    std::vector<size_t> freeSegments_{};
    std::vector<bool> used_{};
};

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_FLAG_BUFFER_RING_H
