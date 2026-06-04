#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
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

struct Stream {
    void* native = nullptr;
};

struct MemoryRegion {
    void* addr = nullptr;
    uint64_t length = 0;
    MemoryType type = MemoryType::Host;
    int device_id = -1;
};

struct MemoryExport {
    MemoryRegion region;
    std::string transport;
    std::vector<std::byte> import_blob;
};

struct LocalMemory {
    MemoryExport exported;
    void* native = nullptr;
};

struct EndpointExport {
    std::vector<MemoryExport> memories;
    std::unordered_map<std::string, std::vector<std::byte>> control_blobs;
};

struct RemoteEndpoint {
    EndpointID id = kLocalEndpointID;
    EndpointExport exported;
};

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

struct PreparedRequest {
    Operation op = Operation::Put;
    LocalMemory* local = nullptr;
    MemoryExport remote;
    uint64_t local_offset = 0;
    uint64_t remote_offset = 0;
    uint64_t length = 0;
    uint64_t tag = 0;
    Stream stream;
    void* context = nullptr;
};

class Transport {
   public:
    virtual ~Transport() = default;

    virtual const char* protocol() const = 0;
    virtual Status init(void* options) = 0;
    virtual Status exportControlPlane(std::vector<std::byte>& out) const;
    virtual Status connect(const RemoteEndpoint& remote);
    virtual Status shutdown() = 0;

    virtual bool supportsMemory(const MemoryRegion& memory) const;
    virtual Status registerMemory(const MemoryRegion& memory,
                                  MemoryExport& out) = 0;
    virtual Status unregisterMemory(void* addr) = 0;
    // submit* returns whether the backend accepted the request. It is not a
    // transfer completion signal and does not distinguish sync vs async work.
    virtual Status submitTransfer(const Transfer& request,
                                  const EndpointExport& local,
                                  const EndpointExport& remote);
    virtual Status submitSend(const Message& request,
                              const EndpointExport& local,
                              const EndpointExport& remote);
    virtual Status submitReceive(const Message& request,
                                 const EndpointExport& local,
                                 const EndpointExport& remote);

    // Return Ok once the backend has accepted the request.
    virtual Status submit(const PreparedRequest& request) = 0;
    virtual Status send(const PreparedRequest& request);
    virtual Status receive(const PreparedRequest& request);

   protected:
    LocalMemory* findLocalMemory(void* addr);
    static const MemoryExport* findMemory(const EndpointExport& endpoint,
                                          uint64_t address,
                                          const std::string& transport);
    static const MemoryExport* findTransportMemory(
        const EndpointExport& endpoint,
        const std::string& transport);
    static const std::vector<std::byte>* findControlBlob(
        const EndpointExport& endpoint,
        const std::string& transport);

    std::unordered_map<void*, LocalMemory> local_memory_;
};

}  // namespace transport
