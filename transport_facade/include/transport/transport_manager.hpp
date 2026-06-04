#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/transport.hpp"

namespace transport {

class TransportManager {
   public:
    using TransportPtr = std::unique_ptr<Transport>;

    TransportManager();

    void installTransport(TransportPtr transport, void* options);
    void shutdown();

    void registerMemory(const MemoryRegion& memory);
    void unregisterMemory(void* addr);

    EndpointExport exportEndpoint() const;
    EndpointID importEndpoint(const EndpointExport& remote);
    void closeEndpoint(EndpointID id);

    Status submitTransfer(const Transfer& request);
    Status send(const Message& request);
    Status receive(const Message& request);

    const EndpointExport& localEndpoint() const { return local_export_; }

   private:
    Transport* selectTransport(TransferRoute route) const;
    RemoteEndpoint* findRemoteEndpoint(EndpointID id);
    Status ensureConnected(Transport& transport,
                           const RemoteEndpoint& endpoint);

    EndpointExport local_export_;
    EndpointID next_endpoint_id_ = kLocalEndpointID + 1;

    std::vector<TransportPtr> transports_;
    std::unordered_map<std::string, Transport*> protocol_map_;
    std::unordered_map<TransferRoute, std::string> route_map_;
    std::unordered_map<EndpointID, RemoteEndpoint> remote_endpoints_;
    std::unordered_map<EndpointID, std::vector<std::string>> connected_transports_;
};

}  // namespace transport
