/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_PROTOCOL_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_PROTOCOL_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include "type/types.h"

namespace UC::RemoteMemStore {

static constexpr uint32_t kXferMagic = 0x55435846;  // UCXF
static constexpr uint16_t kXferVersion = 1;
static constexpr size_t kMaxTensorCount = 64;
static constexpr size_t kMaxNodeNameBytes = 64;

enum class RemoteOp : uint8_t { PUT = 1, GET = 2 };
enum class XferStatus : uint8_t { OK = 0, NOT_FOUND = 1, ERROR = 2 };

struct BufferDesc {
    uint64_t addr{0};
    uint64_t bytes{0};
    uint32_t rkey{0};
};

struct FlagDesc {
    uint64_t addr{0};
    uint64_t bytes{0};
    uint32_t rkey{0};
};

struct ShardLayoutDesc {
    Detail::BlockId block{};
    size_t shard{0};
    size_t bytes{0};
    std::vector<size_t> tensorSizes{};
};

struct RemoteRequest {
    RemoteOp op{RemoteOp::PUT};
    std::string sourceNode{};
    std::string targetNode{};
    uint64_t sourceRank{0};
    uint64_t targetRank{0};
    ShardLayoutDesc shard{};
    BufferDesc poolBuffer{};
    std::vector<BufferDesc> buffers{};
    FlagDesc completionFlag{};
};

struct XferTensorEntry {
    uint64_t addr{0};
    uint64_t bytes{0};
    uint64_t packedOffset{0};
    uint32_t rkey{0};
    uint32_t reserved{0};
};

struct XferMessage {
    uint32_t magic{kXferMagic};
    uint16_t version{kXferVersion};
    uint16_t headerBytes{sizeof(XferMessage)};
    uint64_t requestId{0};
    RemoteOp op{RemoteOp::PUT};
    XferStatus status{XferStatus::OK};
    uint16_t tensorCount{0};
    uint32_t reserved{0};
    uint64_t sourceRank{0};
    uint64_t targetRank{0};
    Detail::BlockId block{};
    uint64_t shardIndex{0};
    uint64_t shardBytes{0};
    BufferDesc poolBuffer{};
    FlagDesc completionFlag{};
    std::array<char, kMaxNodeNameBytes> sourceNode{};
    std::array<char, kMaxNodeNameBytes> targetNode{};
    std::array<XferTensorEntry, kMaxTensorCount> tensors{};

    bool Valid() const noexcept
    {
        return magic == kXferMagic && version == kXferVersion && tensorCount <= kMaxTensorCount;
    }
    std::string SourceNode() const { return sourceNode.data(); }
    std::string TargetNode() const { return targetNode.data(); }
    uint64_t SourceRank() const noexcept { return sourceRank; }
    uint64_t TargetRank() const noexcept { return targetRank; }
};

static_assert(std::is_trivially_copyable_v<XferMessage>);

inline bool DecodeXferMessage(const void* data, size_t bytes, XferMessage& message)
{
    if (data == nullptr || bytes != sizeof(XferMessage)) { return false; }
    std::memcpy(&message, data, sizeof(message));
    return message.Valid();
}

inline void EncodeXferMessage(const XferMessage& message, void* data, size_t bytes)
{
    if (data == nullptr || bytes < sizeof(XferMessage)) { return; }
    std::memcpy(data, &message, sizeof(message));
}

inline void FillNodeName(std::array<char, kMaxNodeNameBytes>& dst, std::string_view src)
{
    dst.fill('\0');
    const auto n = std::min(src.size(), dst.size() - 1);
    std::memcpy(dst.data(), src.data(), n);
}

inline XferMessage MakeXferMessage(const RemoteRequest& request, uint64_t requestId)
{
    XferMessage message;
    message.requestId = requestId;
    message.op = request.op;
    message.sourceRank = request.sourceRank;
    message.targetRank = request.targetRank;
    message.tensorCount =
        static_cast<uint16_t>(std::min(request.buffers.size(), kMaxTensorCount));
    message.block = request.shard.block;
    message.shardIndex = request.shard.shard;
    message.shardBytes = request.shard.bytes;
    message.poolBuffer = request.poolBuffer;
    message.completionFlag = request.completionFlag;
    FillNodeName(message.sourceNode, request.sourceNode);
    FillNodeName(message.targetNode, request.targetNode);
    size_t offset = 0;
    for (size_t i = 0; i < message.tensorCount; ++i) {
        const auto& buffer = request.buffers[i];
        message.tensors[i] = XferTensorEntry{buffer.addr, buffer.bytes, offset, buffer.rkey, 0};
        offset += buffer.bytes;
    }
    return message;
}

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_PROTOCOL_H
