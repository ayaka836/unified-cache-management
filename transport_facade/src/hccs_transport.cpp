#include "transport/hccs_transport.hpp"

#include <string>

namespace transport {

namespace {

struct NotifyState {
    void* native = nullptr;
    uint32_t index = 0;
    uint64_t id = 0;
    uint64_t addr = 0;
    std::string name;
};

struct HccsRequestContext {
    NotifyState* wait = nullptr;
    NotifyState* signal = nullptr;
};

}  // namespace

bool HccsTransport::supportsMemory(const MemoryRegion& memory) const {
    return memory.type == MemoryType::Device;
}

Status HccsTransport::init(void* options) {
    auto* hccs_options = static_cast<HccsOptions*>(options);
    options_ = hccs_options == nullptr ? HccsOptions{} : *hccs_options;

    // aclrtSetDevice(options_.device_id)
    // initialize IPC/P2P runtime
    // attach options_.dispatcher if FFTS is used

    return Status::Ok;
}

Status HccsTransport::exportEndpoint(ProtocolEndpointExport& out) const {
    out.transport = protocol();
    out.attrs = HccsEndpointAttrs{
        options_.device_id,
        options_.local_pid,
        options_.local_rank,
        options_.notify_names,
    };
    return Status::Ok;
}

Status HccsTransport::connect(const RemoteEndpoint& remote) {
    const auto* attrs =
        findEndpointAttrs<HccsEndpointAttrs>(remote.exported, protocol());
    if (attrs == nullptr) {
        return Status::NotSupported;
    }

    peers_[remote.id] = PeerState{*attrs};

    // Prepare peer-side P2P/IPC state from remote memories.
    // Typical calls:
    //   rtSetIpcMemPid(ipc_name, &remote_pid, 1)
    //   create/import HCCS notifies used by two-sided protocols
    return Status::Ok;
}

Status HccsTransport::shutdown() {
    // release notifies
    // close imported IPC memory
    for (const auto& endpoint_entry : remote_import_cache_) {
        for (const auto& entry : endpoint_entry.second) {
            // rtIpcCloseMemory(entry.second.region.addr)
            (void)entry;
        }
    }
    remote_import_cache_.clear();
    peers_.clear();
    // destroy owned runtime context
    options_ = {};
    return Status::Ok;
}

Status HccsTransport::registerMemory(const MemoryRegion& memory,
                                     MemoryExport& out) {
    char ipc_name_buffer[65] = {};

    // rtIpcSetMemoryName(memory.addr, memory.length, ipc_name_buffer,
    //                    sizeof(ipc_name_buffer))
    // Peer pid authorization is driven by HccsEndpointAttrs in connect().

    const std::string ipc_name(ipc_name_buffer);

    out.region = memory;
    out.transport = protocol();
    out.attrs = HccsMemoryAttrs{
        ipc_name,
        options_.local_pid,
        options_.device_id,
    };

    LocalMemory handle;
    handle.exported = out;
    handle.native = nullptr;
    local_memory_[memory.addr] = handle;
    return Status::Ok;
}

Status HccsTransport::unregisterMemory(void* addr) {
    auto& memory = local_memory_[addr];
    // close local IPC/P2PMem handle
    (void)memory;
    local_memory_.erase(addr);
    return Status::Ok;
}

Status HccsTransport::importRemoteMemory(EndpointID target_id,
                                         const MemoryExport& desc,
                                         MemoryExport& out) {
    const auto* attrs = getMemoryAttrs<HccsMemoryAttrs>(desc);
    if (attrs == nullptr) {
        return Status::NotSupported;
    }

    const auto key = attrs->ipc_name.empty()
                         ? std::to_string(
                               reinterpret_cast<uint64_t>(desc.region.addr))
                         : attrs->ipc_name;
    auto& endpoint_cache = remote_import_cache_[target_id];
    auto iter = endpoint_cache.find(key);
    if (iter != endpoint_cache.end()) {
        out = iter->second;
        return Status::Ok;
    }

    void* mapped_addr = nullptr;

    // rtIpcOpenMemory(&mapped_addr, attrs->ipc_name.c_str())
    mapped_addr = desc.region.addr;

    out = desc;
    out.region.addr = mapped_addr;
    endpoint_cache[key] = out;
    return Status::Ok;
}

Status HccsTransport::submitTransfer(const Transfer& request,
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

    MemoryExport imported;
    const auto status =
        importRemoteMemory(request.target_id, *remote_memory, imported);
    if (status != Status::Ok) {
        return status;
    }

    PreparedRequest legacy;
    legacy.op = request.op;
    legacy.target_id = request.target_id;
    legacy.local = local_handle;
    legacy.remote = imported;
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

Status HccsTransport::submit(const PreparedRequest& request) {
    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    auto* local_base =
        static_cast<std::byte*>(request.local->exported.region.addr);
    auto* local = local_base + request.local_offset;
    auto* remote_base = static_cast<std::byte*>(request.remote.region.addr);
    auto* remote = remote_base + request.remote_offset;
    auto* op = static_cast<HccsRequestContext*>(request.context);

    switch (request.op) {
        case Operation::Put:
            // wait(op->wait)
            // device copy: local -> remote
            // signal(op->signal)
            break;
        case Operation::Get:
            // wait(op->wait)
            // device copy: remote -> local
            // signal(op->signal)
            break;
    }

    (void)local;
    (void)remote;
    (void)op;

    return Status::Ok;
}

Status HccsTransport::send(const PreparedRequest& request) {
    MemoryExport remote_memory;
    auto status =
        importRemoteMemory(request.target_id, request.remote, remote_memory);
    if (status != Status::Ok) {
        return status;
    }

    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    auto* local_base =
        static_cast<std::byte*>(request.local->exported.region.addr);
    auto* local = local_base + request.local_offset;
    auto* remote_base = static_cast<std::byte*>(remote_memory.region.addr);
    auto* remote = remote_base + request.remote_offset;
    auto* op = static_cast<HccsRequestContext*>(request.context);

    // wait receiver-ready notify
    // copy local user memory -> remote staging memory
    // record send-done notify
    (void)local;
    (void)remote;
    (void)op;

    return Status::Ok;
}

Status HccsTransport::receive(const PreparedRequest& request) {
    MemoryExport remote_memory;
    auto status =
        importRemoteMemory(request.target_id, request.remote, remote_memory);
    if (status != Status::Ok) {
        return status;
    }

    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    auto* local_base =
        static_cast<std::byte*>(request.local->exported.region.addr);
    auto* local = local_base + request.local_offset;
    auto* remote_base = static_cast<std::byte*>(remote_memory.region.addr);
    auto* remote = remote_base + request.remote_offset;
    auto* op = static_cast<HccsRequestContext*>(request.context);

    // record receiver-ready notify
    // wait send-done notify
    // copy staging memory -> local user memory when needed
    (void)local;
    (void)remote;
    (void)op;

    return Status::Ok;
}

}  // namespace transport
