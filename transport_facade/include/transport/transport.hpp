#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace transport {

using EndpointID = uint64_t;
constexpr EndpointID kLocalEndpointID = 0;

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

enum class TransferRoute {
    LocalD2D,
    RemoteD2D,
    RemoteH2H,
    RemoteD2H,
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

// FFTS 内存属性。
struct FftsMemoryAttrs {};

// HCCS 内存属性。
struct HccsMemoryAttrs {
    std::string ipc_name;
    uint32_t owner_pid = 0;
    int device_id = -1;
};

// HCOMM 内存属性。
struct HcommMemoryAttrs {
    uint64_t remote_addr = 0;
    uint64_t remote_size = 0;
    MemoryType memory_type = MemoryType::Host;
    int device_id = -1;
    uint64_t remote_handle = 0;
};

using MemoryAttrs =
    std::variant<FftsMemoryAttrs, HccsMemoryAttrs, HcommMemoryAttrs>;

// 可交换的内存描述。
struct MemoryExport {
    MemoryRegion region;
    std::string transport;
    MemoryAttrs attrs;
};

// 本地已注册内存。
struct LocalMemory {
    MemoryExport exported;
    void* native = nullptr;
};

// FFTS 端点属性。
struct FftsEndpointAttrs {
    int32_t device_id = 0;
};

// HCCS 端点属性。
struct HccsEndpointAttrs {
    int device_id = -1;
    uint32_t pid = 0;
    int rank = -1;
    std::vector<std::string> notify_names;
};

// HCOMM 端点属性。
struct HcommEndpointAttrs {
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

using EndpointAttrs =
    std::variant<FftsEndpointAttrs, HccsEndpointAttrs, HcommEndpointAttrs>;

// 单协议端点描述。
struct ProtocolEndpointExport {
    std::string transport;
    EndpointAttrs attrs;
};

// 可交换的完整端点描述。
struct EndpointExport {
    std::vector<MemoryExport> memories;
    std::vector<ProtocolEndpointExport> endpoints;
};

// 已导入的远端端点。
struct RemoteEndpoint {
    EndpointID id = kLocalEndpointID;
    EndpointExport exported;
};

// 单边传输请求。
struct Transfer {
    Operation op = Operation::Put;
    TransferRoute route;
    void* local = nullptr;
    EndpointID target_id = kLocalEndpointID;
    uint64_t target_address = 0;
    uint64_t length = 0;
    Stream stream;
    void* context = nullptr;
};

// 双边消息请求。
struct Message {
    TransferRoute route;
    void* local = nullptr;
    EndpointID target_id = kLocalEndpointID;
    uint64_t target_address = 0;
    uint64_t length = 0;
    uint64_t tag = 0;
    Stream stream;
    void* context = nullptr;
};

// 已解析的后端请求。
struct PreparedRequest {
    Operation op = Operation::Put;
    EndpointID target_id = kLocalEndpointID;
    LocalMemory* local = nullptr;
    MemoryExport remote;
    uint64_t local_offset = 0;
    uint64_t remote_offset = 0;
    uint64_t length = 0;
    uint64_t tag = 0;
    Stream stream;
    void* context = nullptr;
};

// 具体传输后端接口。
class Transport {
   public:
    virtual ~Transport() = default;

    // 协议名。
    virtual const char* protocol() const = 0;
    // 初始化后端。
    virtual Status init(void* options) = 0;
    // 导出本端属性。
    virtual Status exportEndpoint(ProtocolEndpointExport& out) const;
    // 连接远端。
    virtual Status connect(const RemoteEndpoint& remote);
    // 关闭后端。
    virtual Status shutdown() = 0;

    // 是否支持该内存。
    virtual bool supportsMemory(const MemoryRegion& memory) const;
    // 注册本地内存。
    virtual Status registerMemory(const MemoryRegion& memory,
                                  MemoryExport& out) = 0;
    // 注销本地内存。
    virtual Status unregisterMemory(void* addr) = 0;
    // 提交单边传输。
    virtual Status submitTransfer(const Transfer& request,
                                  const EndpointExport& local,
                                  const EndpointExport& remote);
    // 提交发送。
    virtual Status submitSend(const Message& request,
                              const EndpointExport& local,
                              const EndpointExport& remote);
    // 提交接收。
    virtual Status submitReceive(const Message& request,
                                 const EndpointExport& local,
                                 const EndpointExport& remote);

    // 后端提交入口。
    virtual Status submit(const PreparedRequest& request) = 0;
    // 后端发送入口。
    virtual Status send(const PreparedRequest& request);
    // 后端接收入口。
    virtual Status receive(const PreparedRequest& request);

   protected:
    // 查找本地内存。
    LocalMemory* findLocalMemory(void* addr);
    // 按地址查找远端内存。
    static const MemoryExport* findMemory(const EndpointExport& endpoint,
                                          uint64_t address,
                                          const std::string& transport);
    // 查找协议首个远端内存。
    static const MemoryExport* findTransportMemory(
        const EndpointExport& endpoint,
        const std::string& transport);

    // 查找协议端点属性。
    template <typename Attrs>
    static const Attrs* findEndpointAttrs(const EndpointExport& endpoint,
                                          const std::string& transport) {
        for (const auto& entry : endpoint.endpoints) {
            if (entry.transport == transport) {
                return std::get_if<Attrs>(&entry.attrs);
            }
        }
        return nullptr;
    }

    // 读取内存属性。
    template <typename Attrs>
    static const Attrs* getMemoryAttrs(const MemoryExport& memory) {
        return std::get_if<Attrs>(&memory.attrs);
    }

    std::unordered_map<void*, LocalMemory> local_memory_;
};

}  // namespace transport
