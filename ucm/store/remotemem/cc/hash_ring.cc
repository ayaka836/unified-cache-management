/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "hash_ring.h"
#include <algorithm>

namespace UC::RemoteMemStore {

Status HashRing::Setup(const Config& config)
{
    if (config.localNode.empty()) { return Status::InvalidParam("invalid remote mem local node"); }
    localNode_ = config.localNode;
    nodes_ = config.nodes;
    if (nodes_.empty()) { nodes_.push_back(localNode_); }
    if (std::find(nodes_.begin(), nodes_.end(), localNode_) == nodes_.end()) {
        nodes_.push_back(localNode_);
    }
    return Status::OK();
}

const std::string& HashRing::Locate(const Detail::BlockId& block) const
{
    static Detail::BlockIdHasher hasher;
    const auto idx = hasher(block) % nodes_.size();
    return nodes_[idx];
}

bool HashRing::IsLocal(const Detail::BlockId& block) const { return Locate(block) == localNode_; }

}  // namespace UC::RemoteMemStore
