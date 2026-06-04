#include "transport/hcomm_transport.hpp"

#include <string>

namespace transport {

Status HcommTransport::init(void* options) {
    auto* hcomm_options = static_cast<HcommOptions*>(options);
    options_ = hcomm_options == nullptr ? HcommOptions{} : *hcomm_options;

    // EndpointDesc endpoint = build from options_.local or options_.endpoint_desc
    // HcommEndpointCreate(&endpoint, &endpoint_)

    threads_.resize(options_.thread_count);
    // HcommThreadAlloc(options_.local.engine, options_.thread_count,
    //                  notify_num_per_thread, threads_.data())

    return Status::Ok;
}

Status HcommTransport::exportEndpoint(ProtocolEndpointExport& out) const {
    HcommEndpointAttrs attrs = options_.local;
    attrs.channel_count = options_.channel_count;
    attrs.notify_count = options_.notify_count;
    attrs.exchange_all_mems = options_.exchange_all_mems;

    out.transport = protocol();
    out.attrs = attrs;
    return Status::Ok;
}

Status HcommTransport::connect(const RemoteEndpoint& remote) {
    const auto* attrs =
        findEndpointAttrs<HcommEndpointAttrs>(remote.exported, protocol());
    if (attrs == nullptr) {
        return Status::NotSupported;
    }

    auto& peer = peers_[remote.id];
    peer.endpoint = *attrs;
    peer.channels.resize(options_.channel_count);

    // Build peer EndpointDesc from attrs.
    // HcommChannelDesc descs[options_.channel_count]
    // fill descs with peer EndpointDesc, notify_count, exchange_all_mems
    // HcommChannelCreate(endpoint_, options_.local.engine, descs,
    //                    options_.channel_count, peer.channels.data())
    return Status::Ok;
}

Status HcommTransport::shutdown() {
    for (const auto& endpoint_entry : remote_import_cache_) {
        for (const auto& entry : endpoint_entry.second) {
            // HcommMemUnimport(endpoint_, entry.second.attrs)
            (void)entry;
        }
    }
    remote_import_cache_.clear();
    peers_.clear();
    // HcommChannelDestroy(peer.channels.data(), peer.channels.size())
    // HcommThreadFree(threads_.data(), threads_.size())
    // HcommEndpointDestroy(endpoint_)
    endpoint_ = nullptr; threads_.clear();
    return Status::Ok;
}

Status HcommTransport::registerMemory(const MemoryRegion& memory,
                                      MemoryExport& out) {
    void* mem_handle = nullptr;

    // CommMem mem {
    //     .type = memory.type == Device ? COMM_MEM_TYPE_DEVICE
    //                                   : COMM_MEM_TYPE_HOST,
    //     .addr = memory.addr,
    //     .size = memory.length,
    // };
    // HcommMemReg(endpoint_, mem_tag, &mem, &mem_handle)
    // HcommMemExport(endpoint_, mem_handle, &desc, &desc_len)

    out.region = memory;
    out.transport = protocol();
    out.attrs = HcommMemoryAttrs{
        reinterpret_cast<uint64_t>(memory.addr),
        memory.length,
        memory.type,
        memory.device_id,
        reinterpret_cast<uint64_t>(mem_handle),
    };

    LocalMemory handle;
    handle.exported = out;
    handle.native = mem_handle;
    local_memory_[memory.addr] = handle;
    return Status::Ok;
}

Status HcommTransport::unregisterMemory(void* addr) {
    auto iter = local_memory_.find(addr);
    if (iter == local_memory_.end()) {
        return Status::NotSupported;
    }
    auto& memory = iter->second;
    // HcommMemUnreg(endpoint_, memory.native)
    (void)memory;
    local_memory_.erase(iter);
    return Status::Ok;
}

Status HcommTransport::importRemoteMemory(EndpointID target_id,
                                          const MemoryExport& desc,
                                          MemoryExport& out) {
    const auto desc_addr = reinterpret_cast<uint64_t>(desc.region.addr);
    const auto* attrs = getMemoryAttrs<HcommMemoryAttrs>(desc);
    if (attrs == nullptr) {
        return Status::NotSupported;
    }

    auto& endpoint_cache = remote_import_cache_[target_id];
    auto iter = endpoint_cache.find(desc_addr);
    if (iter != endpoint_cache.end()) {
        out = iter->second;
        return Status::Ok;
    }

    MemoryExport imported_memory = desc;

    // CommMem remote_mem {};
    // HcommMemImport(endpoint_, attrs, &remote_mem)
    //
    // HCOMM import returns only a CommMem view. There is no additional remote
    // native handle. For real runtime integration:
    // imported_memory.region.addr = remote_mem.addr;
    // imported_memory.region.length = remote_mem.size;
    // imported_memory.region.type = remote_mem.type == COMM_MEM_TYPE_DEVICE
    //                            ? MemoryType::Device
    //                            : MemoryType::Host;
    if (attrs->remote_addr != 0) {
        imported_memory.region.addr =
            reinterpret_cast<void*>(attrs->remote_addr);
    }
    if (attrs->remote_size != 0) {
        imported_memory.region.length = attrs->remote_size;
    }
    imported_memory.region.type = attrs->memory_type;
    imported_memory.region.device_id = attrs->device_id;

    out = imported_memory;
    endpoint_cache[desc_addr] = out;
    return Status::Ok;
}

Status HcommTransport::submitTransfer(const Transfer& request,
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

Status HcommTransport::submit(const PreparedRequest& request) {
    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    const auto local_base =
        reinterpret_cast<uint64_t>(request.local->exported.region.addr);
    const auto remote_base =
        reinterpret_cast<uint64_t>(request.remote.region.addr);

    if (request.length != 0 && (local_base == 0 || remote_base == 0)) {
        return Status::NotSupported;
    }
    if (request.local_offset > request.local->exported.region.length ||
        request.length >
            request.local->exported.region.length - request.local_offset) {
        return Status::NotSupported;
    }
    if (request.remote_offset > request.remote.region.length ||
        request.length >
            request.remote.region.length - request.remote_offset) {
        return Status::NotSupported;
    }

    auto* local =
        reinterpret_cast<std::byte*>(local_base + request.local_offset);
    auto* remote =
        reinterpret_cast<std::byte*>(remote_base + request.remote_offset);
    auto thread = threads_.empty() ? 0 : threads_.front();
    auto peer_iter = peers_.find(request.target_id);
    auto channel = peer_iter == peers_.end() ||
                           peer_iter->second.channels.empty()
                       ? 0
                       : peer_iter->second.channels.front();

    switch (request.op) {
        case Operation::Put:
            // HcommWriteOnThread(thread, channel, remote, local,
            //                    request.length)
            break;
        case Operation::Get:
            // HcommReadOnThread(thread, channel, local, remote,
            //                   request.length)
            break;
    }

    (void)local;
    (void)remote;
    (void)thread;
    (void)channel;
    return Status::Ok;
}

Status HcommTransport::send(const PreparedRequest& request) {
    MemoryExport remote;
    auto status = importRemoteMemory(request.target_id, request.remote, remote);
    if (status != Status::Ok) {
        return status;
    }

    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    const auto local_base =
        reinterpret_cast<uint64_t>(request.local->exported.region.addr);
    const auto remote_base = reinterpret_cast<uint64_t>(remote.region.addr);

    if (request.length != 0 && (local_base == 0 || remote_base == 0)) {
        return Status::NotSupported;
    }
    if (request.local_offset > request.local->exported.region.length ||
        request.length >
            request.local->exported.region.length - request.local_offset) {
        return Status::NotSupported;
    }
    if (request.remote_offset > remote.region.length ||
        request.length > remote.region.length - request.remote_offset) {
        return Status::NotSupported;
    }

    auto* local =
        reinterpret_cast<std::byte*>(local_base + request.local_offset);
    auto* remote_addr =
        reinterpret_cast<std::byte*>(remote_base + request.remote_offset);
    auto thread = threads_.empty() ? 0 : threads_.front();
    auto peer_iter = peers_.find(request.target_id);
    auto channel = peer_iter == peers_.end() ||
                           peer_iter->second.channels.empty()
                       ? 0
                       : peer_iter->second.channels.front();

    // Use request.tag/context as the caller-defined two-sided match
    // context. The HCOMM backend can map it to notify/wait metadata.
    // HcommWriteWithNotifyOnThread(thread, channel, remote_addr, local,
    //                              request.length, request.tag)
    (void)local;
    (void)remote_addr;
    (void)thread;
    (void)channel;
    (void)request;
    return Status::Ok;
}

Status HcommTransport::receive(const PreparedRequest& request) {
    MemoryExport remote;
    auto status = importRemoteMemory(request.target_id, request.remote, remote);
    if (status != Status::Ok) {
        return status;
    }

    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    const auto local_base =
        reinterpret_cast<uint64_t>(request.local->exported.region.addr);
    const auto remote_base = reinterpret_cast<uint64_t>(remote.region.addr);

    if (request.length != 0 && (local_base == 0 || remote_base == 0)) {
        return Status::NotSupported;
    }
    if (request.local_offset > request.local->exported.region.length ||
        request.length >
            request.local->exported.region.length - request.local_offset) {
        return Status::NotSupported;
    }
    if (request.remote_offset > remote.region.length ||
        request.length > remote.region.length - request.remote_offset) {
        return Status::NotSupported;
    }

    auto* local =
        reinterpret_cast<std::byte*>(local_base + request.local_offset);
    auto* remote_addr =
        reinterpret_cast<std::byte*>(remote_base + request.remote_offset);
    auto thread = threads_.empty() ? 0 : threads_.front();
    auto peer_iter = peers_.find(request.target_id);
    auto channel = peer_iter == peers_.end() ||
                           peer_iter->second.channels.empty()
                       ? 0
                       : peer_iter->second.channels.front();

    // Use request.tag/context as the caller-defined two-sided match
    // context. The HCOMM backend can map it to notify/wait metadata.
    // HcommReadOnThread(thread, channel, local, remote_addr, request.length)
    (void)local;
    (void)remote_addr;
    (void)thread;
    (void)channel;
    (void)request;
    return Status::Ok;
}

}  // namespace transport
