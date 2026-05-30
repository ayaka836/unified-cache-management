/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_HASH_RING_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_HASH_RING_H

#include <string>
#include <vector>
#include "global_config.h"
#include "status/status.h"
#include "type/types.h"

namespace UC::RemoteMemStore {

class HashRing {
public:
    Status Setup(const Config& config);
    const std::string& Locate(const Detail::BlockId& block) const;
    bool IsLocal(const Detail::BlockId& block) const;
    const std::string& LocalNode() const noexcept { return localNode_; }
    const std::vector<std::string>& Nodes() const noexcept { return nodes_; }

private:
    std::string localNode_;
    std::vector<std::string> nodes_;
};

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_HASH_RING_H
