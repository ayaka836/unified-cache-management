#pragma once

#include <cstdint>
#include <memory>

#include "transport/transport.hpp"

namespace transport {

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

    const char* protocol() const override { return "ffts"; }
    bool supportsMemory(const MemoryRegion& memory) const override;

    Status init(void* options) override;
    Status exportEndpoint(ProtocolEndpointExport& out) const override;
    Status shutdown() override;
    Status registerMemory(const MemoryRegion& memory,
                          MemoryExport& out) override;
    Status unregisterMemory(void* addr) override;
    Status submitTransfer(const Transfer& request,
                          const EndpointExport& local,
                          const EndpointExport& remote) override;
    Status submit(const PreparedRequest& request) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    int32_t device_id_ = 0;
};

}  // namespace transport
