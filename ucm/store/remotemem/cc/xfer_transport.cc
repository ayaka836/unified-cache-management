/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include "xfer_transport.h"
#include <atomic>
#include <cstring>
#include "logger/logger.h"

namespace UC::RemoteMemStore {

Status XferTransport::Setup(const Config& config)
{
    localNode_ = config.localNode;
    localRank_ = config.localRank;
    transport_ = config.xferTransport;
    if (transport_ != "rdma" && transport_ != "loopback") {
        return Status::InvalidParam("invalid remote mem xfer transport({})", transport_);
    }
    return Status::OK();
}

bool XferTransport::LocalOnly(const XferMessage& message) const
{
    return message.SourceNode() == localNode_ && message.TargetNode() == localNode_ &&
           message.SourceRank() == localRank_ && message.TargetRank() == localRank_;
}

Status XferTransport::CheckDataPath(const XferMessage& message, size_t bytes) const
{
    if (!message.Valid()) { return Status::DeserializeFailed(); }
    if (message.poolBuffer.addr == 0 || message.poolBuffer.bytes < message.shardBytes) {
        return Status::InvalidParam("invalid remote mem pool buffer");
    }
    if (!LocalOnly(message)) {
        UC_ERROR("RemoteMem xfer transport({}) has no data link for {} -> {}.", transport_,
                 message.SourceNode(), message.TargetNode());
        return Status::Unsupported();
    }
    if (bytes != message.shardBytes) { return Status::InvalidParam("invalid xfer bytes"); }
    return Status::OK();
}

Status XferTransport::SendControl(const XferMessage& message) const
{
    if (!message.Valid()) { return Status::DeserializeFailed(); }
    if (LocalOnly(message)) { return Status::OK(); }
    UC_ERROR("RemoteMem xfer transport({}) has no link for {} -> {}.", transport_,
             message.SourceNode(), message.TargetNode());
    return Status::Unsupported();
}

Status XferTransport::RemoteDeviceToHost(const XferMessage& message, void* remoteDevice,
                                         void* localHost, size_t bytes,
                                         Trans::Stream* stream) const
{
    auto s = CheckDataPath(message, bytes);
    if (s.Failure()) { return s; }
    if (reinterpret_cast<uint64_t>(localHost) != message.poolBuffer.addr) {
        return Status::InvalidParam("remote mem pool buffer mismatch");
    }
    if (stream == nullptr) { return Status::InvalidParam("invalid device stream"); }
    return stream->DeviceToHost(remoteDevice, localHost, bytes);
}

Status XferTransport::HostToRemoteDevice(const XferMessage& message, void* localHost,
                                         void* remoteDevice, size_t bytes,
                                         Trans::Stream* stream) const
{
    auto s = CheckDataPath(message, bytes);
    if (s.Failure()) { return s; }
    if (reinterpret_cast<uint64_t>(localHost) != message.poolBuffer.addr) {
        return Status::InvalidParam("remote mem pool buffer mismatch");
    }
    if (stream == nullptr) { return Status::InvalidParam("invalid device stream"); }
    return stream->HostToDevice(localHost, remoteDevice, bytes);
}

Status XferTransport::HostToHost(const XferMessage& message, void* srcHost, void* dstHost,
                                 size_t bytes) const
{
    auto s = CheckDataPath(message, bytes);
    if (s.Failure()) { return s; }
    if (reinterpret_cast<uint64_t>(dstHost) != message.poolBuffer.addr &&
        reinterpret_cast<uint64_t>(srcHost) != message.poolBuffer.addr) {
        return Status::InvalidParam("remote mem pool buffer mismatch");
    }
    std::memcpy(dstHost, srcHost, bytes);
    return Status::OK();
}

Status XferTransport::Notify(const FlagDesc& flag, uint64_t value) const
{
    if (flag.addr == 0 || flag.bytes < sizeof(value)) { return Status::InvalidParam(); }
    auto* ptr = reinterpret_cast<uint64_t*>(flag.addr);
    *ptr = value;
    std::atomic_thread_fence(std::memory_order_release);
    return Status::OK();
}

}  // namespace UC::RemoteMemStore
