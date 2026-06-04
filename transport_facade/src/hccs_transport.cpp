#include "transport/hccs_transport.hpp"

#include <algorithm>
#include <memory>
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

Status HccsTransport::init(void* options) {
    auto* hccs_options = static_cast<HccsOptions*>(options);
    options_ = hccs_options == nullptr ? HccsOptions{} : *hccs_options;

    // aclrtSetDevice(options_.device_id)
    // initialize IPC/P2P runtime
    // attach options_.dispatcher if FFTS is used

    return Status::Ok;
}

EndpointExport HccsTransport::exportEndpoint() const {
    auto endpoint = local_export_;
    auto attrs = std::make_shared<HccsEndpointAttrs>();
    attrs->device_id = options_.device_id;
    attrs->pid = options_.local_pid;
    attrs->rank = options_.local_rank;
    attrs->notify_names = options_.notify_names;
    endpoint.attrs = attrs;
    return endpoint;
}

EndpointID HccsTransport::importEndpoint(const EndpointExport& remote) {
    const auto* attrs =
        dynamic_cast<const HccsEndpointAttrs*>(remote.attrs.get());
    if (attrs == nullptr) {
        return kInvalidEndpointID;
    }

    const auto id = next_endpoint_id_++;
    peers_[id] = PeerState{*attrs};
    remote_endpoints_[id] = remote;

    // Prepare peer-side P2P/IPC state from remote memories.
    // Typical calls:
    //   rtSetIpcMemPid(ipc_name, &remote_pid, 1)
    //   create/import HCCS notifies used by two-sided protocols
    return id;
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
    tasks_.clear();
    remote_endpoints_.clear();
    peers_.clear();
    local_memory_.clear();
    local_export_.memories.clear();
    // destroy owned runtime context
    options_ = {};
    return Status::Ok;
}

Status HccsTransport::registerMemory(const MemoryRegion& memory,
                                     MemoryHandle& out) {
    if (memory.type != MemoryType::Device) {
        return Status::NotSupported;
    }

    LocalMemory local;
    char ipc_name_buffer[65] = {};

    // rtIpcSetMemoryName(memory.addr, memory.length, ipc_name_buffer,
    //                    sizeof(ipc_name_buffer))
    // Peer pid authorization is driven by HccsEndpointAttrs in connect().

    const std::string ipc_name(ipc_name_buffer);

    local.exported.region = memory;
    auto attrs = std::make_shared<HccsMemoryAttrs>();
    attrs->ipc_name = ipc_name;
    attrs->owner_pid = options_.local_pid;
    attrs->device_id = options_.device_id;
    local.exported.attrs = attrs;
    local.native = nullptr;

    const auto handle = next_memory_handle_++;
    local.exported.handle = handle;
    local_memory_[handle] = local;
    local_export_.memories.push_back(local.exported);
    out = handle;
    return Status::Ok;
}

Status HccsTransport::unregisterMemory(MemoryHandle handle) {
    auto iter = local_memory_.find(handle);
    if (iter == local_memory_.end()) {
        return Status::NotSupported;
    }

    // close local IPC/P2PMem handle
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

void HccsTransport::closeEndpoint(EndpointID id) {
    remote_endpoints_.erase(id);
    peers_.erase(id);
    remote_import_cache_.erase(id);
}

TaskID HccsTransport::allocateTask(const TaskResult& result, void* native) {
    const auto id = next_task_id_++;
    tasks_[id] = TaskRecord{result, native};
    return id;
}

Status HccsTransport::importRemoteMemory(EndpointID target_id,
                                         const MemoryExport& desc,
                                         MemoryExport& out) {
    const auto* attrs =
        dynamic_cast<const HccsMemoryAttrs*>(desc.attrs.get());
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

Status HccsTransport::submitTransfer(const Transfer& request, TaskID& out) {
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

    const MemoryExport* remote_export = nullptr;
    for (const auto& memory : remote->memories) {
        if (memory.handle == request.remote_handle) {
            remote_export = &memory;
            break;
        }
    }
    if (remote_export == nullptr) {
        return Status::NotSupported;
    }

    MemoryExport remote_memory;
    auto status =
        importRemoteMemory(request.target_id, *remote_export, remote_memory);
    if (status != Status::Ok) {
        return status;
    }

    const auto local_base_value =
        reinterpret_cast<uint64_t>(local.exported.region.addr);
    const auto local_addr_value = reinterpret_cast<uint64_t>(request.local);
    const auto remote_base_value =
        reinterpret_cast<uint64_t>(remote_export->region.addr);
    if (local_addr_value < local_base_value ||
        request.target_address < remote_base_value) {
        return Status::NotSupported;
    }
    const auto local_offset = local_addr_value - local_base_value;
    const auto remote_offset = request.target_address - remote_base_value;
    if (local_offset > local.exported.region.length ||
        request.length > local.exported.region.length - local_offset ||
        remote_offset > remote_export->region.length ||
        request.length > remote_export->region.length - remote_offset) {
        return Status::NotSupported;
    }

    auto* local_base =
        static_cast<std::byte*>(local.exported.region.addr);
    auto* local_addr = local_base + local_offset;
    auto* remote_base = static_cast<std::byte*>(remote_memory.region.addr);
    auto* remote_addr = remote_base + remote_offset;
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

    (void)local_addr;
    (void)remote_addr;
    (void)op;

    out = allocateTask(TaskResult{TaskState::Pending, Status::Ok,
                                  request.length}, op);
    return Status::Ok;
}

Status HccsTransport::send(const Message& request, TaskID& out) {
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

    const auto local_base_value =
        reinterpret_cast<uint64_t>(local.exported.region.addr);
    const auto local_addr_value = reinterpret_cast<uint64_t>(request.local);
    if (local_addr_value < local_base_value) {
        return Status::NotSupported;
    }
    const auto local_offset = local_addr_value - local_base_value;
    if (local_offset > local.exported.region.length ||
        request.length > local.exported.region.length - local_offset) {
        return Status::NotSupported;
    }
    auto* local_base =
        static_cast<std::byte*>(local.exported.region.addr);
    auto* local_addr = local_base + local_offset;

    // wait receiver-ready notify from peer state
    // send local user memory
    // record send-done notify
    (void)local_addr;
    (void)request;

    out = allocateTask(TaskResult{TaskState::Pending, Status::Ok,
                                  request.length});
    return Status::Ok;
}

Status HccsTransport::receive(const Message& request, TaskID& out) {
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

    const auto local_base_value =
        reinterpret_cast<uint64_t>(local.exported.region.addr);
    const auto local_addr_value = reinterpret_cast<uint64_t>(request.local);
    if (local_addr_value < local_base_value) {
        return Status::NotSupported;
    }
    const auto local_offset = local_addr_value - local_base_value;
    if (local_offset > local.exported.region.length ||
        request.length > local.exported.region.length - local_offset) {
        return Status::NotSupported;
    }
    auto* local_base =
        static_cast<std::byte*>(local.exported.region.addr);
    auto* local_addr = local_base + local_offset;

    // record receiver-ready notify
    // wait send-done notify
    // receive into local user memory
    (void)local_addr;
    (void)request;

    out = allocateTask(TaskResult{TaskState::Pending, Status::Ok,
                                  request.length});
    return Status::Ok;
}

Status HccsTransport::query(TaskID id, TaskResult& out) {
    auto iter = tasks_.find(id);
    if (iter == tasks_.end()) {
        out = {};
        return Status::NotSupported;
    }

    // Real implementation should query HCCS notify/copy completion here.
    out = iter->second.result;
    return Status::Ok;
}

Status HccsTransport::wait(TaskID id, TaskResult& out, uint64_t timeout_us) {
    auto iter = tasks_.find(id);
    if (iter == tasks_.end()) {
        out = {};
        return Status::NotSupported;
    }

    // Real implementation should wait on HCCS notify/copy completion.
    (void)timeout_us;
    iter->second.result.state = TaskState::Done;
    out = iter->second.result;
    return Status::Ok;
}

void HccsTransport::release(TaskID id) {
    tasks_.erase(id);
}

}  // namespace transport
