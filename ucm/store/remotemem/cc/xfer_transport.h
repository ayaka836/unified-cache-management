/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#ifndef UNIFIEDCACHE_REMOTEMEM_STORE_CC_XFER_TRANSPORT_H
#define UNIFIEDCACHE_REMOTEMEM_STORE_CC_XFER_TRANSPORT_H

#include <cstdint>
#include "global_config.h"
#include "protocol.h"
#include "status/status.h"
#include "trans/stream.h"

namespace UC::RemoteMemStore {

class XferTransport {
public:
    Status Setup(const Config& config);
    Status SendControl(const XferMessage& message) const;
    Status RemoteDeviceToHost(const XferMessage& message, void* remoteDevice, void* localHost,
                              size_t bytes, Trans::Stream* stream) const;
    Status HostToRemoteDevice(const XferMessage& message, void* localHost, void* remoteDevice,
                              size_t bytes, Trans::Stream* stream) const;
    Status HostToHost(const XferMessage& message, void* srcHost, void* dstHost, size_t bytes) const;
    Status Notify(const FlagDesc& flag, uint64_t value) const;

private:
    bool LocalOnly(const XferMessage& message) const;
    Status CheckDataPath(const XferMessage& message, size_t bytes) const;

private:
    std::string localNode_;
    uint64_t localRank_{0};
    std::string transport_{"rdma"};
};

}  // namespace UC::RemoteMemStore

#endif  // UNIFIEDCACHE_REMOTEMEM_STORE_CC_XFER_TRANSPORT_H
