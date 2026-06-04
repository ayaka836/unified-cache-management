#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/transport.hpp"

namespace transport {

struct HccsOptions {
    int device_id = -1;
    uint32_t local_pid = 0;
    int local_rank = -1;
    std::vector<std::string> notify_names;
    void* dispatcher = nullptr;
};

class HccsTransport final : public Transport {
   public:
    const char* protocol() const override { return "hccs"; }
    bool supportsMemory(const MemoryRegion& memory) const override;

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
        HccsEndpointAttrs endpoint;
    };

    Status importRemoteMemory(EndpointID target_id, const MemoryExport& desc,
                              MemoryExport& out);

    HccsOptions options_;
    std::unordered_map<EndpointID, PeerState> peers_;
    std::unordered_map<EndpointID, std::unordered_map<std::string, MemoryExport>>
        remote_import_cache_;
};

}  // namespace transport
