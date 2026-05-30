/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_GLOBAL_CONFIG_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_GLOBAL_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ucmstore_v1.h"

namespace UC::RemoteMemStore {

enum class AddressMode : uint8_t { DEVICE, HOST };

struct Config {
    StoreV1* storeBackend{};
    std::string localNode{"local"};
    std::vector<std::string> nodes{};
    size_t localRank{0};
    size_t poolRank{0};
    int32_t deviceId{-1};
    std::vector<size_t> tensorSizes{};
    size_t shardSize{0};
    size_t blockSize{0};
    size_t dramPoolCapacity{1ULL << 30};
    size_t flagBufferCapacity{1ULL << 20};
    size_t flagBytes{8};
    size_t timeoutMs{30000};
    size_t deviceStagingSlots{4};
    bool backendFallback{true};
    bool backendWriteThrough{false};
    bool registerHostBuffer{true};
    bool deviceStagingEnable{true};
    std::string xferTransport{"rdma"};
    AddressMode addressMode{AddressMode::DEVICE};
};

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_GLOBAL_CONFIG_H
