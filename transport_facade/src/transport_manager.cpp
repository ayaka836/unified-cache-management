#include "transport/transport_manager.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace transport {

TransportManager::TransportManager() {
    route_map_[TransferRoute::LocalD2D] = "ffts";
    route_map_[TransferRoute::RemoteD2D] = "hccs";
    route_map_[TransferRoute::RemoteH2H] = "hcomm";
    route_map_[TransferRoute::RemoteD2H] = "hcomm";
}

void TransportManager::installTransport(TransportPtr transport,
                                        void* options) {
    transport->init(options);
    protocol_map_[transport->protocol()] = transport.get();
    transports_.push_back(std::move(transport));
}

void TransportManager::shutdown() {
    for (auto& transport : transports_) {
        transport->shutdown();
    }
    transports_.clear();
    protocol_map_.clear();
    remote_endpoints_.clear();
    connected_transports_.clear();
}

void TransportManager::registerMemory(const MemoryRegion& memory) {
    for (auto& transport : transports_) {
        if (!transport->supportsMemory(memory)) {
            continue;
        }
        MemoryExport desc;
        transport->registerMemory(memory, desc);
        local_export_.memories.push_back(desc);
    }
}

void TransportManager::unregisterMemory(void* addr) {
    for (auto& transport : transports_) {
        transport->unregisterMemory(addr);
    }
    const auto value = reinterpret_cast<uint64_t>(addr);
    local_export_.memories.erase(
        std::remove_if(local_export_.memories.begin(),
                       local_export_.memories.end(),
                       [value](const MemoryExport& desc) {
                           return reinterpret_cast<uint64_t>(
                                      desc.region.addr) == value;
                       }),
        local_export_.memories.end());
}

EndpointExport TransportManager::exportEndpoint() const {
    auto endpoint = local_export_;
    for (const auto& transport : transports_) {
        std::vector<std::byte> blob;
        if (transport->exportControlPlane(blob) == Status::Ok) {
            endpoint.control_blobs[transport->protocol()] = std::move(blob);
        }
    }
    return endpoint;
}

EndpointID TransportManager::importEndpoint(const EndpointExport& remote) {
    auto id = next_endpoint_id_++;
    RemoteEndpoint endpoint;
    endpoint.id = id;
    endpoint.exported = remote;
    remote_endpoints_[id] = std::move(endpoint);
    return id;
}

void TransportManager::closeEndpoint(EndpointID id) {
    remote_endpoints_.erase(id);
    connected_transports_.erase(id);
}

Status TransportManager::submitTransfer(const Transfer& request) {
    if (transports_.empty()) {
        return Status::NotSupported;
    }

    auto* transport = selectTransport(request.route);
    if (transport == nullptr) {
        return Status::NotSupported;
    }

    if (request.target_id == kLocalEndpointID) {
        return transport->submitTransfer(request, local_export_, local_export_);
    }

    auto* endpoint = findRemoteEndpoint(request.target_id);
    if (endpoint == nullptr ||
        ensureConnected(*transport, *endpoint) != Status::Ok) {
        return Status::NotSupported;
    }
    return transport->submitTransfer(request, local_export_,
                                     endpoint->exported);
}

Status TransportManager::send(const Message& request) {
    if (transports_.empty()) {
        return Status::NotSupported;
    }

    auto* transport = selectTransport(request.route);
    if (transport == nullptr) {
        return Status::NotSupported;
    }

    if (request.target_id == kLocalEndpointID) {
        return transport->submitSend(request, local_export_, local_export_);
    }

    auto* endpoint = findRemoteEndpoint(request.target_id);
    if (endpoint == nullptr ||
        ensureConnected(*transport, *endpoint) != Status::Ok) {
        return Status::NotSupported;
    }
    return transport->submitSend(request, local_export_, endpoint->exported);
}

Status TransportManager::receive(const Message& request) {
    if (transports_.empty()) {
        return Status::NotSupported;
    }

    auto* transport = selectTransport(request.route);
    if (transport == nullptr) {
        return Status::NotSupported;
    }

    return transport->submitReceive(request, local_export_, local_export_);
}

Transport* TransportManager::selectTransport(TransferRoute route) const {
    auto route_iter = route_map_.find(route);
    if (route_iter == route_map_.end()) {
        return nullptr;
    }

    auto protocol_iter = protocol_map_.find(route_iter->second);
    if (protocol_iter == protocol_map_.end()) {
        return nullptr;
    }
    return protocol_iter->second;
}

RemoteEndpoint* TransportManager::findRemoteEndpoint(EndpointID id) {
    if (id == kLocalEndpointID) {
        return nullptr;
    }
    auto iter = remote_endpoints_.find(id);
    if (iter == remote_endpoints_.end()) {
        return nullptr;
    }
    return &iter->second;
}

Status TransportManager::ensureConnected(Transport& transport,
                                         const RemoteEndpoint& endpoint) {
    const auto transport_name = std::string(transport.protocol());
    auto& connected = connected_transports_[endpoint.id];
    if (std::find(connected.begin(), connected.end(), transport_name) !=
        connected.end()) {
        return Status::Ok;
    }

    const auto status = transport.connect(endpoint);
    if (status == Status::Ok) {
        connected.push_back(transport_name);
    }
    return status;
}

}  // namespace transport
