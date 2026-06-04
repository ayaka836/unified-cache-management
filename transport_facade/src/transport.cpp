#include "transport/transport.hpp"

namespace transport {

Status Transport::registerMemory(const MemoryRegion& memory,
                                 MemoryHandle& out) {
    (void)memory;
    out = kInvalidMemoryHandle;
    return Status::NotSupported;
}

Status Transport::unregisterMemory(MemoryHandle handle) {
    (void)handle;
    return Status::NotSupported;
}

EndpointExport Transport::exportEndpoint() const {
    return {};
}

EndpointID Transport::importEndpoint(const EndpointExport& remote) {
    (void)remote;
    return kInvalidEndpointID;
}

void Transport::closeEndpoint(EndpointID id) {
    (void)id;
}

Status Transport::submitTransfer(const Transfer& request, TaskID& out) {
    (void)request;
    out = kInvalidTaskID;
    return Status::NotSupported;
}

Status Transport::send(const Message& request, TaskID& out) {
    (void)request;
    out = kInvalidTaskID;
    return Status::NotSupported;
}

Status Transport::receive(const Message& request, TaskID& out) {
    (void)request;
    out = kInvalidTaskID;
    return Status::NotSupported;
}

Status Transport::query(TaskID id, TaskResult& out) {
    (void)id;
    out = {};
    return Status::NotSupported;
}

Status Transport::wait(TaskID id, TaskResult& out, uint64_t timeout_us) {
    (void)id;
    (void)timeout_us;
    out = {};
    return Status::NotSupported;
}

void Transport::release(TaskID id) {
    (void)id;
}

}  // namespace transport
