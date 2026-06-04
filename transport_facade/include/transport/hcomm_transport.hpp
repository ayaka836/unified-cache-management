#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/transport.hpp"

namespace transport {

struct HcommMemoryAttrs final : MemoryAttrs {
    uint64_t remote_addr = 0;
    uint64_t remote_size = 0;
    MemoryType memory_type = MemoryType::Host;
    int device_id = -1;
    uint64_t remote_handle = 0;
};

struct HcommEndpointAttrs final : EndpointAttrs {
    int protocol = -1;
    int engine = -1;
    int addr_type = -1;
    std::string addr;
    int loc_type = -1;
    int device_id = -1;
    uint32_t channel_count = 1;
    uint32_t notify_count = 0;
    bool exchange_all_mems = false;
};

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
        HcommEndpointAttrs endpoint;
        std::vector<uint64_t> channels;
    };

    struct TaskRecord {
        TaskResult result;
        void* native = nullptr;
    };

    Status importRemoteMemory(EndpointID target_id, const MemoryExport& desc,
                              MemoryExport& out);
    TaskID allocateTask(const TaskResult& result, void* native = nullptr);

    HcommOptions options_;
    void* endpoint_ = nullptr;
    std::vector<uint64_t> threads_;
    std::unordered_map<MemoryHandle, LocalMemory> local_memory_;
    EndpointExport local_export_;
    EndpointID next_endpoint_id_ = kLocalEndpointID + 1;
    MemoryHandle next_memory_handle_ = kInvalidMemoryHandle + 1;
    std::unordered_map<EndpointID, EndpointExport> remote_endpoints_;
    std::unordered_map<EndpointID, PeerState> peers_;
    std::unordered_map<EndpointID, std::unordered_map<uint64_t, MemoryExport>>
        remote_import_cache_;
    TaskID next_task_id_ = kInvalidTaskID + 1;
    std::unordered_map<TaskID, TaskRecord> tasks_;
};

}  // namespace transport
