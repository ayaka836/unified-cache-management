#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/transport.hpp"

namespace transport {

using HcommEndpoint = HcommEndpointAttrs;

struct HcommOptions {
    HcommEndpoint local;

    uint32_t channel_count = 1;
    uint32_t thread_count = 1;
    uint32_t notify_count = 0;
    bool exchange_all_mems = false;

    void* endpoint_desc = nullptr;  // optional EndpointDesc*
};

class HcommTransport final : public Transport {
   public:
    const char* protocol() const override { return "hcomm"; }

    Status init(void* options) override;
    Status exportEndpoint(ProtocolEndpointExport& out) const override;
    Status connect(const RemoteEndpoint& remote) override;
    Status shutdown() override;
    Status registerMemory(const MemoryRegion& memory,
                          MemoryExport& out) override;
    Status unregisterMemory(void* addr) override;
    Status submitTransfer(const Transfer& request,
                          const EndpointExport& local,
                          const EndpointExport& remote) override;
    Status submit(const PreparedRequest& request) override;
    Status send(const PreparedRequest& request) override;
    Status receive(const PreparedRequest& request) override;

   private:
    struct PeerState {
        HcommEndpointAttrs endpoint;
        std::vector<uint64_t> channels;
    };

    Status importRemoteMemory(EndpointID target_id, const MemoryExport& desc,
                              MemoryExport& out);

    HcommOptions options_;
    void* endpoint_ = nullptr;
    std::vector<uint64_t> threads_;
    std::unordered_map<EndpointID, PeerState> peers_;
    std::unordered_map<EndpointID, std::unordered_map<uint64_t, MemoryExport>>
        remote_import_cache_;
};

}  // namespace transport
