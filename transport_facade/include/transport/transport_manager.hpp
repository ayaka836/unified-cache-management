#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "transport/transport.hpp"

namespace transport {

// 传输后端调度器。
class TransportManager {
   public:
    using TransportPtr = std::unique_ptr<Transport>;

    TransportManager();

    // 安装传输后端。
    void installTransport(TransportPtr transport, void* options);
    // 关闭所有后端。
    void shutdown();

    // 注册本地内存。
    void registerMemory(const MemoryRegion& memory);
    // 注销本地内存。
    void unregisterMemory(void* addr);

    // 导出本端描述。
    EndpointExport exportEndpoint() const;
    // 导入远端描述。
    EndpointID importEndpoint(const EndpointExport& remote);
    // 关闭远端端点。
    void closeEndpoint(EndpointID id);

    // 提交单边传输。
    Status submitTransfer(const Transfer& request);
    // 发送消息。
    Status send(const Message& request);
    // 接收消息。
    Status receive(const Message& request);

    // 本端描述。
    const EndpointExport& localEndpoint() const { return local_export_; }

   private:
    // 按路由选后端。
    Transport* selectTransport(TransferRoute route) const;
    // 查找远端端点。
    RemoteEndpoint* findRemoteEndpoint(EndpointID id);
    // 确保后端已连接远端。
    Status ensureConnected(Transport& transport,
                           const RemoteEndpoint& endpoint);

    EndpointExport local_export_;
    EndpointID next_endpoint_id_ = kLocalEndpointID + 1;

    std::vector<TransportPtr> transports_;
    std::unordered_map<std::string, Transport*> protocol_map_;
    std::unordered_map<TransferRoute, std::string> route_map_;
    std::unordered_map<EndpointID, RemoteEndpoint> remote_endpoints_;
    std::unordered_map<EndpointID, std::vector<std::string>> connected_transports_;
};

}  // namespace transport
