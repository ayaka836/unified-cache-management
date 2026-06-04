#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "transport/transport.hpp"

namespace transport {

struct FftsMemoryAttrs final : MemoryAttrs {};

struct FftsEndpointAttrs final : EndpointAttrs {
    int32_t device_id = 0;
};

struct FftsOptions {
    int32_t device_id = 0;
};

class FftsTransport final : public Transport {
   public:
    FftsTransport();
    ~FftsTransport() override;

    FftsTransport(FftsTransport&&) noexcept;
    FftsTransport& operator=(FftsTransport&&) noexcept;

    FftsTransport(const FftsTransport&) = delete;
    FftsTransport& operator=(const FftsTransport&) = delete;

    Status init(void* options) override;
    Status shutdown() override;
    EndpointExport exportEndpoint() const override;
    Status submitTransfer(const Transfer& request, TaskID& out) override;
    Status query(TaskID id, TaskResult& out) override;
    Status wait(TaskID id, TaskResult& out, uint64_t timeout_us) override;
    void release(TaskID id) override;

   private:
    struct TaskRecord {
        TaskResult result;
        void* native = nullptr;
    };

    class Impl;
    std::unique_ptr<Impl> impl_;
    int32_t device_id_ = 0;
    TaskID next_task_id_ = kInvalidTaskID + 1;
    std::unordered_map<TaskID, TaskRecord> tasks_;
};

}  // namespace transport
