#include "transport/hcomm_transport.hpp"

#include <algorithm>
#include <memory>
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

EndpointExport HcommTransport::exportEndpoint() const {
    auto endpoint = local_export_;
    HcommEndpointAttrs attrs = options_.local;
    attrs.channel_count = options_.channel_count;
    attrs.notify_count = options_.notify_count;
    attrs.exchange_all_mems = options_.exchange_all_mems;

    endpoint.attrs = std::make_shared<HcommEndpointAttrs>(attrs);
    return endpoint;
}

EndpointID HcommTransport::importEndpoint(const EndpointExport& remote) {
    const auto* attrs =
        dynamic_cast<const HcommEndpointAttrs*>(remote.attrs.get());
    if (attrs == nullptr) {
        return kInvalidEndpointID;
    }

    const auto id = next_endpoint_id_++;
    auto& peer = peers_[id];
    peer.endpoint = *attrs;
    const auto remote_channel_count =
        attrs->channel_count == 0 ? 1 : attrs->channel_count;
    const auto channel_count =
        std::min(options_.channel_count, remote_channel_count);
    peer.channels.resize(channel_count);

    // Build peer EndpointDesc from attrs.
    // HcommChannelDesc descs[channel_count]
    // fill descs with peer EndpointDesc, attrs->notify_count,
    // attrs->exchange_all_mems
    // HcommChannelCreate(endpoint_, options_.local.engine, descs,
    //                    channel_count, peer.channels.data())
    remote_endpoints_[id] = remote;
    return id;
}

Status HcommTransport::shutdown() {
    for (const auto& endpoint_entry : remote_import_cache_) {
        for (const auto& entry : endpoint_entry.second) {
            // HcommMemUnimport(endpoint_, entry.second.attrs)
            (void)entry;
        }
    }
    remote_import_cache_.clear();
    tasks_.clear();
    remote_endpoints_.clear();
    peers_.clear();
    local_memory_.clear();
    local_export_.memories.clear();
    // HcommChannelDestroy(peer.channels.data(), peer.channels.size())
    // HcommThreadFree(threads_.data(), threads_.size())
    // HcommEndpointDestroy(endpoint_)
    endpoint_ = nullptr; threads_.clear();
    return Status::Ok;
}

Status HcommTransport::registerMemory(const MemoryRegion& memory,
                                      MemoryHandle& out) {
    LocalMemory local;
    void* mem_handle = nullptr;

    // CommMem mem {
    //     .type = memory.type == Device ? COMM_MEM_TYPE_DEVICE
    //                                   : COMM_MEM_TYPE_HOST,
    //     .addr = memory.addr,
    //     .size = memory.length,
    // };
    // HcommMemReg(endpoint_, mem_tag, &mem, &mem_handle)
    // HcommMemExport(endpoint_, mem_handle, &desc, &desc_len)

    local.exported.region = memory;
    auto attrs = std::make_shared<HcommMemoryAttrs>();
    attrs->remote_addr = reinterpret_cast<uint64_t>(memory.addr);
    attrs->remote_size = memory.length;
    attrs->memory_type = memory.type;
    attrs->device_id = memory.device_id;
    attrs->remote_handle = reinterpret_cast<uint64_t>(mem_handle);
    local.exported.attrs = attrs;

    local.native = mem_handle;

    const auto handle = next_memory_handle_++;
    local.exported.handle = handle;
    local_memory_[handle] = local;
    local_export_.memories.push_back(local.exported);
    out = handle;
    return Status::Ok;
}

Status HcommTransport::unregisterMemory(MemoryHandle handle) {
    auto iter = local_memory_.find(handle);
    if (iter == local_memory_.end()) {
        return Status::NotSupported;
    }

    // HcommMemUnreg(endpoint_, memory.native)
    (void)iter->second;
    local_memory_.erase(iter);
    local_export_.memories.erase(
        std::remove_if(local_export_.memories.begin(),
                       local_export_.memories.end(),
                       [handle](const MemoryExport& desc) {
                           return desc.handle == handle;
                       }),
        local_export_.memories.end());
    return Status::Ok;
}

void HcommTransport::closeEndpoint(EndpointID id) {
    remote_endpoints_.erase(id);
    peers_.erase(id);
    remote_import_cache_.erase(id);
}

TaskID HcommTransport::allocateTask(const TaskResult& result, void* native) {
    const auto id = next_task_id_++;
    tasks_[id] = TaskRecord{result, native};
    return id;
}

Status HcommTransport::importRemoteMemory(EndpointID target_id,
                                          const MemoryExport& desc,
                                          MemoryExport& out) {
    const auto desc_key = desc.handle;
    const auto* attrs =
        dynamic_cast<const HcommMemoryAttrs*>(desc.attrs.get());
    if (attrs == nullptr) {
        return Status::NotSupported;
    }

    auto& endpoint_cache = remote_import_cache_[target_id];
    auto iter = endpoint_cache.find(desc_key);
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
    endpoint_cache[desc_key] = out;
    return Status::Ok;
}

Status HcommTransport::submitTransfer(const Transfer& request, TaskID& out) {
    out = kInvalidTaskID;
    const auto* remote = &local_export_;
    if (request.target_id != kLocalEndpointID) {
        auto remote_iter = remote_endpoints_.find(request.target_id);
        if (remote_iter == remote_endpoints_.end()) {
            return Status::NotSupported;
        }
        remote = &remote_iter->second;
    }

    auto local_iter = local_memory_.find(request.local_handle);
    if (local_iter == local_memory_.end()) {
        return Status::NotSupported;
    }
    auto& local = local_iter->second;

    const MemoryExport* remote_memory = nullptr;
    for (const auto& memory : remote->memories) {
        if (memory.handle == request.remote_handle) {
            remote_memory = &memory;
            break;
        }
    }
    if (remote_memory == nullptr) {
        return Status::NotSupported;
    }

    MemoryExport imported_remote;
    auto status =
        importRemoteMemory(request.target_id, *remote_memory, imported_remote);
    if (status != Status::Ok) {
        return status;
    }

    const auto local_base =
        reinterpret_cast<uint64_t>(local.exported.region.addr);
    const auto local_addr_value = reinterpret_cast<uint64_t>(request.local);
    const auto exported_remote_base =
        reinterpret_cast<uint64_t>(remote_memory->region.addr);
    const auto remote_base =
        reinterpret_cast<uint64_t>(imported_remote.region.addr);

    if (request.length != 0 && (local_base == 0 || remote_base == 0)) {
        return Status::NotSupported;
    }
    if (local_addr_value < local_base ||
        request.target_address < exported_remote_base) {
        return Status::NotSupported;
    }
    const auto local_offset = local_addr_value - local_base;
    const auto remote_offset = request.target_address - exported_remote_base;
    if (local_offset > local.exported.region.length ||
        request.length > local.exported.region.length - local_offset) {
        return Status::NotSupported;
    }
    if (remote_offset > imported_remote.region.length ||
        request.length >
            imported_remote.region.length - remote_offset) {
        return Status::NotSupported;
    }

    auto* local_addr = reinterpret_cast<std::byte*>(local_base + local_offset);
    auto* remote_addr = reinterpret_cast<std::byte*>(remote_base + remote_offset);
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

    (void)local_addr;
    (void)remote_addr;
    (void)thread;
    (void)channel;
    out = allocateTask(TaskResult{TaskState::Pending, Status::Ok,
                                  request.length});
    return Status::Ok;
}

Status HcommTransport::send(const Message& request, TaskID& out) {
    out = kInvalidTaskID;
    if (request.target_id != kLocalEndpointID &&
        remote_endpoints_.find(request.target_id) == remote_endpoints_.end()) {
        return Status::NotSupported;
    }

    auto local_iter = local_memory_.find(request.local_handle);
    if (local_iter == local_memory_.end()) {
        return Status::NotSupported;
    }
    auto& local = local_iter->second;

    const auto local_base =
        reinterpret_cast<uint64_t>(local.exported.region.addr);
    const auto local_addr_value = reinterpret_cast<uint64_t>(request.local);

    if (request.length != 0 && local_base == 0) {
        return Status::NotSupported;
    }
    if (local_addr_value < local_base) {
        return Status::NotSupported;
    }
    const auto local_offset = local_addr_value - local_base;
    if (local_offset > local.exported.region.length ||
        request.length > local.exported.region.length - local_offset) {
        return Status::NotSupported;
    }

    auto* local_addr = reinterpret_cast<std::byte*>(local_base + local_offset);
    auto thread = threads_.empty() ? 0 : threads_.front();
    auto peer_iter = peers_.find(request.target_id);
    auto channel = peer_iter == peers_.end() ||
                           peer_iter->second.channels.empty()
                       ? 0
                       : peer_iter->second.channels.front();

    // HcommSendOnThread(thread, channel, local_addr, request.length)
    (void)local_addr;
    (void)thread;
    (void)channel;
    (void)request;
    out = allocateTask(TaskResult{TaskState::Pending, Status::Ok,
                                  request.length});
    return Status::Ok;
}

Status HcommTransport::receive(const Message& request, TaskID& out) {
    out = kInvalidTaskID;
    if (request.target_id != kLocalEndpointID &&
        remote_endpoints_.find(request.target_id) == remote_endpoints_.end()) {
        return Status::NotSupported;
    }

    auto local_iter = local_memory_.find(request.local_handle);
    if (local_iter == local_memory_.end()) {
        return Status::NotSupported;
    }
    auto& local = local_iter->second;

    const auto local_base =
        reinterpret_cast<uint64_t>(local.exported.region.addr);
    const auto local_addr_value = reinterpret_cast<uint64_t>(request.local);

    if (request.length != 0 && local_base == 0) {
        return Status::NotSupported;
    }
    if (local_addr_value < local_base) {
        return Status::NotSupported;
    }
    const auto local_offset = local_addr_value - local_base;
    if (local_offset > local.exported.region.length ||
        request.length > local.exported.region.length - local_offset) {
        return Status::NotSupported;
    }

    auto* local_addr = reinterpret_cast<std::byte*>(local_base + local_offset);
    auto thread = threads_.empty() ? 0 : threads_.front();
    auto peer_iter = peers_.find(request.target_id);
    auto channel = peer_iter == peers_.end() ||
                           peer_iter->second.channels.empty()
                       ? 0
                       : peer_iter->second.channels.front();

    // HcommReceiveOnThread(thread, channel, local_addr, request.length)
    (void)local_addr;
    (void)thread;
    (void)channel;
    (void)request;
    out = allocateTask(TaskResult{TaskState::Pending, Status::Ok,
                                  request.length});
    return Status::Ok;
}

Status HcommTransport::query(TaskID id, TaskResult& out) {
    auto iter = tasks_.find(id);
    if (iter == tasks_.end()) {
        out = {};
        return Status::NotSupported;
    }

    // Real implementation should query HCOMM request/native completion here.
    out = iter->second.result;
    return Status::Ok;
}

Status HcommTransport::wait(TaskID id, TaskResult& out, uint64_t timeout_us) {
    auto iter = tasks_.find(id);
    if (iter == tasks_.end()) {
        out = {};
        return Status::NotSupported;
    }

    // Real implementation should block on HCOMM completion up to timeout_us.
    (void)timeout_us;
    iter->second.result.state = TaskState::Done;
    out = iter->second.result;
    return Status::Ok;
}

void HcommTransport::release(TaskID id) {
    tasks_.erase(id);
}

}  // namespace transport
