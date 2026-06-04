#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/transport.hpp"

namespace transport {

struct HccsMemoryAttrs final : MemoryAttrs {
    std::string ipc_name;
    uint32_t owner_pid = 0;
    int device_id = -1;
};

struct HccsEndpointAttrs final : EndpointAttrs {
    int device_id = -1;
    uint32_t pid = 0;
    int rank = -1;
    std::vector<std::string> notify_names;
};

struct HccsOptions {
    int device_id = -1;
    uint32_t local_pid = 0;
    int local_rank = -1;
    std::vector<std::string> notify_names;
    void* dispatcher = nullptr;
};

class HccsTransport final : public Transport {
   public:
    Status init(void* options) override;
    Status shutdown() override;
    Status registerMemory(const MemoryRegion& memory,
                          MemoryHandle& out) override;
    Status unregisterMemory(MemoryHandle handle) override;
    EndpointExport exportEndpoint() const override;
    EndpointID importEndpoint(const EndpointExport& remote) override;
    void closeEndpoint(EndpointID id) override;
    Status submitTransfer(const Transfer& request, TaskID& out) override;
    Status send(const Message& request, TaskID& out) override;
    Status receive(const Message& request, TaskID& out) override;
    Status query(TaskID id, TaskResult& out) override;
    Status wait(TaskID id, TaskResult& out, uint64_t timeout_us) override;
    void release(TaskID id) override;

   private:
    struct PeerState {
        HccsEndpointAttrs endpoint;
    };

    struct TaskRecord {
        TaskResult result;
        void* native = nullptr;
    };

    Status importRemoteMemory(EndpointID target_id, const MemoryExport& desc,
                              MemoryExport& out);
    TaskID allocateTask(const TaskResult& result, void* native = nullptr);

    HccsOptions options_;
    std::unordered_map<MemoryHandle, LocalMemory> local_memory_;
    EndpointExport local_export_;
    EndpointID next_endpoint_id_ = kLocalEndpointID + 1;
    MemoryHandle next_memory_handle_ = kInvalidMemoryHandle + 1;
    std::unordered_map<EndpointID, EndpointExport> remote_endpoints_;
    std::unordered_map<EndpointID, PeerState> peers_;
    std::unordered_map<EndpointID, std::unordered_map<std::string, MemoryExport>>
        remote_import_cache_;
    TaskID next_task_id_ = kInvalidTaskID + 1;
    std::unordered_map<TaskID, TaskRecord> tasks_;
};

}  // namespace transport
