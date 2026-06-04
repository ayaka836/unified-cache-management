#include "transport/transport.hpp"

namespace transport {

Status Transport::exportControlPlane(std::vector<std::byte>& out) const {
    (void)out;
    return Status::NotSupported;
}

Status Transport::connect(const RemoteEndpoint& remote) {
    (void)remote;
    return Status::Ok;
}

bool Transport::supportsMemory(const MemoryRegion& memory) const {
    (void)memory;
    return true;
}

Status Transport::submitTransfer(const Transfer& request,
                                 const EndpointExport& local,
                                 const EndpointExport& remote) {
    auto* local_handle = findLocalMemory(request.local);
    if (local_handle == nullptr || remote.memories.empty()) {
        return Status::NotSupported;
    }

    const std::string protocol_name(protocol());
    const auto* remote_memory =
        findMemory(remote, request.target_address, protocol_name);
    if (remote_memory == nullptr) {
        return Status::NotSupported;
    }

    PreparedRequest legacy;
    legacy.op = request.op;
    legacy.local = local_handle;
    legacy.remote = *remote_memory;
    legacy.local_offset =
        reinterpret_cast<uint64_t>(request.local) -
        reinterpret_cast<uint64_t>(local_handle->exported.region.addr);
    legacy.remote_offset =
        request.target_address -
        reinterpret_cast<uint64_t>(remote_memory->region.addr);
    legacy.length = request.length;
    legacy.stream = request.stream;
    legacy.context = request.context;
    (void)local;
    return submit(legacy);
}

Status Transport::submitSend(const Message& request,
                             const EndpointExport& local,
                             const EndpointExport& remote) {
    auto* local_handle = findLocalMemory(request.local);
    const std::string protocol_name(protocol());
    if (local_handle == nullptr || remote.memories.empty()) {
        return Status::NotSupported;
    }
    const auto* remote_memory =
        findMemory(remote, request.target_address, protocol_name);
    if (remote_memory == nullptr) {
        return Status::NotSupported;
    }

    PreparedRequest backend_request;
    backend_request.local = local_handle;
    backend_request.remote = *remote_memory;
    backend_request.local_offset =
        reinterpret_cast<uint64_t>(request.local) -
        reinterpret_cast<uint64_t>(local_handle->exported.region.addr);
    backend_request.remote_offset =
        request.target_address -
        reinterpret_cast<uint64_t>(remote_memory->region.addr);
    backend_request.length = request.length;
    backend_request.tag = request.tag;
    backend_request.stream = request.stream;
    backend_request.context = request.context;
    (void)local;
    return send(backend_request);
}

Status Transport::submitReceive(const Message& request,
                                const EndpointExport& local,
                                const EndpointExport& remote) {
    auto* local_handle = findLocalMemory(request.local);
    const std::string protocol_name(protocol());
    const auto* remote_memory = findTransportMemory(remote, protocol_name);
    if (local_handle == nullptr || remote_memory == nullptr) {
        return Status::NotSupported;
    }

    PreparedRequest backend_request;
    backend_request.local = local_handle;
    backend_request.remote = *remote_memory;
    backend_request.local_offset =
        reinterpret_cast<uint64_t>(request.local) -
        reinterpret_cast<uint64_t>(local_handle->exported.region.addr);
    backend_request.remote_offset = 0;
    backend_request.length = request.length;
    backend_request.tag = request.tag;
    backend_request.stream = request.stream;
    backend_request.context = request.context;
    (void)local;
    return receive(backend_request);
}

Status Transport::send(const PreparedRequest& request) {
    (void)request;
    return Status::NotSupported;
}

Status Transport::receive(const PreparedRequest& request) {
    (void)request;
    return Status::NotSupported;
}

LocalMemory* Transport::findLocalMemory(void* addr) {
    const auto value = reinterpret_cast<uint64_t>(addr);
    for (auto& entry : local_memory_) {
        const auto base =
            reinterpret_cast<uint64_t>(entry.second.exported.region.addr);
        if (value >= base &&
            value < base + entry.second.exported.region.length) {
            return &entry.second;
        }
    }
    return nullptr;
}

const MemoryExport* Transport::findMemory(const EndpointExport& endpoint,
                                          uint64_t address,
                                          const std::string& transport) {
    for (const auto& memory : endpoint.memories) {
        const auto base = reinterpret_cast<uint64_t>(memory.region.addr);
        if (memory.transport == transport && address >= base &&
            address < base + memory.region.length) {
            return &memory;
        }
    }
    return nullptr;
}

const MemoryExport* Transport::findTransportMemory(
    const EndpointExport& endpoint,
    const std::string& transport) {
    for (const auto& memory : endpoint.memories) {
        if (memory.transport == transport) {
            return &memory;
        }
    }
    return nullptr;
}

const std::vector<std::byte>* Transport::findControlBlob(
    const EndpointExport& endpoint,
    const std::string& transport) {
    auto iter = endpoint.control_blobs.find(transport);
    return iter == endpoint.control_blobs.end() ? nullptr : &iter->second;
}

}  // namespace transport
