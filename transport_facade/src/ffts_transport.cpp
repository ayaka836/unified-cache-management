#include "transport/ffts_transport.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace transport {

namespace {

constexpr size_t kMaxReadyContexts = 128;
constexpr uint16_t kFftsTimeout = std::numeric_limits<uint16_t>::max();
constexpr uint8_t kCommunicationSubType = 0x5A;
constexpr uint8_t kLabelMarker = 0x5A;

struct FftsCommonContext {
    alignas(8) std::byte storage[128] = {};
};

struct FftsSdmaContext {
    uint32_t context_type = 0;
    uint32_t thread_dim = 1;
    uint32_t sdma_sqe_header = 0;
    uint32_t source_address_base_l = 0;
    uint32_t source_address_base_h = 0;
    uint32_t destination_address_base_l = 0;
    uint32_t destination_address_base_h = 0;
    uint32_t non_tail_data_length = 0;
    uint32_t tail_data_length = 0;
    uint8_t label = kLabelMarker;
};

struct FftsSqe {
    uint16_t total_context_num = 0;
    uint16_t ready_context_num = 0;
    uint16_t preload_context_num = 0;
    uint16_t timeout = kFftsTimeout;
    uint8_t sub_type = kCommunicationSubType;
};

struct FftsTaskInfo {
    FftsSqe* sqe = nullptr;
    void* desc_buf = nullptr;
    size_t desc_buf_len = 0;
};

struct FftsCopyDesc {
    void* dst = nullptr;
    const void* src = nullptr;
    size_t size = 0;
};

uint32_t low32(uint64_t value) {
    return static_cast<uint32_t>(value & 0xFFFFFFFFULL);
}

uint32_t high32(uint64_t value) {
    return static_cast<uint32_t>((value >> 32U) & 0xFFFFFFFFULL);
}

uint32_t buildSdmaMoveHeader() {
    constexpr uint32_t kDataTypeFp32 = 7U;
    constexpr uint32_t kSourceSubstreamValid = 1U << 9U;
    constexpr uint32_t kDestinationSubstreamValid = 1U << 10U;
    constexpr uint32_t kSourceNonSecure = 1U << 11U;
    constexpr uint32_t kDestinationNonSecure = 1U << 12U;
    return (kDataTypeFp32 << 4U) | kSourceSubstreamValid |
           kDestinationSubstreamValid | kSourceNonSecure |
           kDestinationNonSecure;
}

void fillSdmaContext(FftsCommonContext& storage, const FftsCopyDesc& copy) {
    storage = {};

    FftsSdmaContext ctx;
    ctx.sdma_sqe_header = buildSdmaMoveHeader();

    const auto src = reinterpret_cast<uint64_t>(copy.src);
    const auto dst = reinterpret_cast<uint64_t>(copy.dst);
    ctx.source_address_base_l = low32(src);
    ctx.source_address_base_h = high32(src);
    ctx.destination_address_base_l = low32(dst);
    ctx.destination_address_base_h = high32(dst);

    const auto bytes = static_cast<uint32_t>(copy.size);
    ctx.non_tail_data_length = bytes;
    ctx.tail_data_length = bytes;

    std::memcpy(storage.storage, &ctx,
                std::min(sizeof(storage.storage), sizeof(ctx)));
}

FftsSqe buildSqe(uint16_t context_count) {
    FftsSqe sqe;
    sqe.total_context_num = context_count;
    sqe.ready_context_num = context_count;
    sqe.preload_context_num = context_count;
    return sqe;
}

}  // namespace

class FftsEngine {
   public:
    using ContextBuffer = std::vector<FftsCommonContext>;

    Status setup(int32_t device_id) {
        if (ready_ && device_id_ == device_id) {
            return Status::Ok;
        }

        // aclrtSetDevice(device_id)
        // aclrtCreateStreamWithConfig(&stream_, 0,
        //     ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC)
        device_id_ = device_id;
        stream_ = nullptr;
        ready_ = true;
        return Status::Ok;
    }

    Status submit(const FftsCopyDesc* copies, size_t count) {
        size_t offset = 0;
        while (offset < count) {
            const auto chunk = std::min(kMaxReadyContexts, count - offset);
            const auto status = submitChunk(copies + offset, chunk);
            if (status != Status::Ok) {
                return status;
            }
            offset += chunk;
        }
        return Status::Ok;
    }

    Status synchronize() {
        // aclrtSynchronizeStream(stream_)
        clearCompletedGraphs();
        return Status::Ok;
    }

   private:
    Status submitChunk(const FftsCopyDesc* copies, size_t count) {
        std::vector<FftsCopyDesc> active_copies;
        active_copies.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& copy = copies[i];
            if (copy.size == 0 || copy.dst == copy.src) {
                continue;
            }
            active_copies.push_back(copy);
        }

        if (active_copies.empty()) {
            return Status::Ok;
        }

        auto contexts = std::make_shared<ContextBuffer>(active_copies.size());
        for (size_t i = 0; i < active_copies.size(); ++i) {
            fillSdmaContext((*contexts)[i], active_copies[i]);
        }

        auto sqe = buildSqe(static_cast<uint16_t>(contexts->size()));
        FftsTaskInfo task;
        task.sqe = &sqe;
        task.desc_buf = contexts->data();
        task.desc_buf_len = sizeof(FftsCommonContext) * contexts->size();

        // rtFftsPlusTaskLaunchWithFlag(&task, stream_, 0)
        (void)task;

        keepAlive(std::move(contexts));
        return Status::Ok;
    }

    void keepAlive(std::shared_ptr<ContextBuffer> contexts) {
        pending_contexts_.emplace_back(std::move(contexts));
    }

    void clearCompletedGraphs() { pending_contexts_.clear(); }

    int32_t device_id_{-1};
    void* stream_{nullptr};
    bool ready_{false};
    std::vector<std::shared_ptr<ContextBuffer>> pending_contexts_;
};

class FftsTransport::Impl {
   public:
    Status setup(int32_t device_id) { return engine_.setup(device_id); }
    Status submit(const FftsCopyDesc* copies, size_t count) {
        return engine_.submit(copies, count);
    }
    Status synchronize() { return engine_.synchronize(); }

   private:
    FftsEngine engine_;
};

FftsTransport::FftsTransport() : impl_(std::make_unique<Impl>()) {}

FftsTransport::~FftsTransport() = default;

FftsTransport::FftsTransport(FftsTransport&&) noexcept = default;

FftsTransport& FftsTransport::operator=(FftsTransport&&) noexcept = default;

bool FftsTransport::supportsMemory(const MemoryRegion& memory) const {
    return memory.type == MemoryType::Device;
}

Status FftsTransport::init(void* options) {
    auto* ffts_options = static_cast<FftsOptions*>(options);
    device_id_ = ffts_options == nullptr ? 0 : ffts_options->device_id;
    return impl_->setup(device_id_);
}

Status FftsTransport::exportEndpoint(ProtocolEndpointExport& out) const {
    out.transport = protocol();
    out.attrs = FftsEndpointAttrs{device_id_};
    return Status::Ok;
}

Status FftsTransport::shutdown() { return impl_->synchronize(); }

Status FftsTransport::registerMemory(const MemoryRegion& memory,
                                     MemoryExport& out) {
    out.region = memory;
    out.transport = protocol();
    out.attrs = FftsMemoryAttrs{};

    LocalMemory handle;
    handle.exported = out;
    local_memory_[memory.addr] = handle;
    return Status::Ok;
}

Status FftsTransport::unregisterMemory(void* addr) {
    local_memory_.erase(addr);
    return Status::Ok;
}

Status FftsTransport::submitTransfer(const Transfer& request,
                                     const EndpointExport& local,
                                     const EndpointExport& remote) {
    auto* local_handle = findLocalMemory(request.local);
    auto* target_handle =
        findLocalMemory(reinterpret_cast<void*>(request.target_address));
    if (local_handle == nullptr || target_handle == nullptr) {
        return Status::NotSupported;
    }

    const auto source_addr = reinterpret_cast<uint64_t>(request.local);
    const auto source_base =
        reinterpret_cast<uint64_t>(local_handle->exported.region.addr);
    const auto target_addr = request.target_address;
    const auto target_base =
        reinterpret_cast<uint64_t>(target_handle->exported.region.addr);

    PreparedRequest backend_request;
    backend_request.op = request.op;
    backend_request.target_id = request.target_id;
    backend_request.local = local_handle;
    backend_request.remote = target_handle->exported;
    backend_request.remote.transport = protocol();
    backend_request.local_offset = source_addr - source_base;
    backend_request.remote_offset = target_addr - target_base;
    backend_request.length = request.length;
    backend_request.stream = request.stream;
    backend_request.context = request.context;
    (void)local;
    return submit(backend_request);
}

Status FftsTransport::submit(const PreparedRequest& request) {
    if (request.local == nullptr) {
        return Status::NotSupported;
    }

    auto* local_base = static_cast<std::byte*>(request.local->exported.region.addr);
    auto* local = local_base + request.local_offset;
    auto* remote_base = static_cast<std::byte*>(request.remote.region.addr);
    auto* remote = remote_base + request.remote_offset;

    switch (request.op) {
        case Operation::Put: {
            FftsCopyDesc copy{remote, local, request.length};
            return impl_->submit(&copy, 1);
        }
        case Operation::Get: {
            FftsCopyDesc copy{local, remote, request.length};
            return impl_->submit(&copy, 1);
        }
    }

    return Status::Failed;
}

}  // namespace transport
