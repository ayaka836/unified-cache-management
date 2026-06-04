#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace transport {

using EndpointID = uint64_t;
using MemoryHandle = uint64_t;
using TaskID = uint64_t;
constexpr EndpointID kLocalEndpointID = 0;
constexpr EndpointID kInvalidEndpointID = 0;
constexpr MemoryHandle kInvalidMemoryHandle = 0;
constexpr TaskID kInvalidTaskID = 0;

enum class Status {
    Ok,
    NotSupported,
    Failed,
};

enum class MemoryType {
    Host,
    Device,
};

enum class Operation {
    Put,
    Get,
};

enum class TaskState {
    Pending,
    Done,
    Failed,
};

// 后端原生流。
struct Stream {
    void* native = nullptr;
};

// 一段本地内存。
struct MemoryRegion {
    void* addr = nullptr;
    uint64_t length = 0;
    MemoryType type = MemoryType::Host;
    int device_id = -1;
};

// 内存属性基类。
struct MemoryAttrs {
    virtual ~MemoryAttrs() = default;
};

// 可交换的内存描述。
struct MemoryExport {
    MemoryHandle handle = kInvalidMemoryHandle;
    MemoryRegion region;
    std::shared_ptr<const MemoryAttrs> attrs;
};

// 本地已注册内存。
struct LocalMemory {
    MemoryExport exported;
    void* native = nullptr;
};

// 端点属性基类。
struct EndpointAttrs {
    virtual ~EndpointAttrs() = default;
};

// 可交换的端点描述。
struct EndpointExport {
    std::shared_ptr<const EndpointAttrs> attrs;
    std::vector<MemoryExport> memories;
};

// 单边传输请求。
struct Transfer {
    Operation op = Operation::Put;
    MemoryHandle local_handle = kInvalidMemoryHandle;
    void* local = nullptr;
    EndpointID target_id = kLocalEndpointID;
    MemoryHandle remote_handle = kInvalidMemoryHandle;
    uint64_t target_address = 0;
    uint64_t length = 0;
    Stream stream;
    void* context = nullptr;
};

// 双边消息请求。
struct Message {
    MemoryHandle local_handle = kInvalidMemoryHandle;
    void* local = nullptr;
    EndpointID target_id = kLocalEndpointID;
    uint64_t length = 0;
    Stream stream;
};

// 异步任务结果。
struct TaskResult {
    TaskState state = TaskState::Pending;
    Status status = Status::Ok;
    uint64_t transferred = 0;
};

// 传输后端最小接口。
class Transport {
   public:
    virtual ~Transport() = default;

    // 初始化后端。
    virtual Status init(void* options) = 0;
    // 关闭后端。
    virtual Status shutdown() = 0;
    // 注册本地内存。
    virtual Status registerMemory(const MemoryRegion& memory,
                                  MemoryHandle& out);
    // 注销本地内存。
    virtual Status unregisterMemory(MemoryHandle handle);
    // 导出本端描述。
    virtual EndpointExport exportEndpoint() const;
    // 导入远端描述。
    virtual EndpointID importEndpoint(const EndpointExport& remote);
    // 关闭远端端点。
    virtual void closeEndpoint(EndpointID id);
    // 提交单边传输。
    virtual Status submitTransfer(const Transfer& request, TaskID& out);
    // 提交发送。
    virtual Status send(const Message& request, TaskID& out);
    // 提交接收。
    virtual Status receive(const Message& request, TaskID& out);
    // 查询任务。
    virtual Status query(TaskID id, TaskResult& out);
    // 等待任务。
    virtual Status wait(TaskID id, TaskResult& out, uint64_t timeout_us);
    // 释放任务。
    virtual void release(TaskID id);
};

}  // namespace transport
