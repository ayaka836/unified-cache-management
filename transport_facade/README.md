# Transport Facade 设计说明

`transport_facade` 现在没有 `TransportManager`。上层直接创建并调用具体 transport，例如 `FftsTransport`、`HccsTransport`、`HcommTransport`。

## 调用流程

```text
transport.init(options)
  -> transport.registerMemory(memory)
  -> local = transport.exportEndpoint()
  -> remote_id = transport.importEndpoint(remote)
  -> transport.submitTransfer(request)
  -> transport.send(message)
  -> transport.receive(message)
```

`importEndpoint()` 只保存远端元数据。首次远端提交时，`Transport` 基类会调用具体后端的 `connect()` 懒建链。

## 基础类型

### `EndpointID`

当前 transport 内的远端端点句柄。

- `kLocalEndpointID`：本地端点，固定为 `0`。

### `Status`

接口返回状态。

- `Ok`：成功。
- `NotSupported`：不支持。
- `Failed`：失败。

### `MemoryType`

内存类型。

- `Host`：主机内存。
- `Device`：设备内存。

### `Operation`

单边传输方向。

- `Put`：本地写远端。
- `Get`：本地读远端。

## 内存结构

### `Stream`

后端原生流。

- `native`：原生 stream 指针。

### `MemoryRegion`

内存范围。

- `addr`：起始地址。
- `length`：长度。
- `type`：内存类型。
- `device_id`：设备 ID。

### `FftsMemoryAttrs`

FFTS 内存属性。当前为空。

### `HccsMemoryAttrs`

HCCS 内存属性。

- `ipc_name`：IPC 内存名。
- `owner_pid`：导出进程 ID。
- `device_id`：设备 ID。

### `HcommMemoryAttrs`

HCOMM 内存属性。

- `remote_addr`：远端地址。
- `remote_size`：远端大小。
- `memory_type`：内存类型。
- `device_id`：设备 ID。
- `remote_handle`：远端句柄。

### `MemoryAttrs`

协议内存属性。

```cpp
using MemoryAttrs =
    std::variant<FftsMemoryAttrs, HccsMemoryAttrs, HcommMemoryAttrs>;
```

### `MemoryExport`

可交换的内存描述。

- `region`：公共内存范围。
- `attrs`：协议内存属性。

### `LocalMemory`

本地已注册内存。

- `exported`：导出的内存描述。
- `native`：后端本地句柄。

## 端点结构

### `FftsEndpointAttrs`

FFTS 端点属性。

- `device_id`：设备 ID。

### `HccsEndpointAttrs`

HCCS 端点属性。

- `device_id`：设备 ID。
- `pid`：进程 ID。
- `rank`：rank。
- `notify_names`：notify 名称。

### `HcommEndpointAttrs`

HCOMM 端点属性。

- `protocol`：HCOMM 协议类型。
- `engine`：HCOMM engine。
- `addr_type`：地址类型。
- `addr`：端点地址。
- `loc_type`：位置类型。
- `device_id`：设备 ID。
- `channel_count`：channel 数。
- `notify_count`：notify 数。
- `exchange_all_mems`：是否交换全部内存。

### `EndpointAttrs`

协议端点属性。

```cpp
using EndpointAttrs =
    std::variant<FftsEndpointAttrs, HccsEndpointAttrs, HcommEndpointAttrs>;
```

### `EndpointExport`

可交换的端点描述。

- `attrs`：协议端点属性。
- `memories`：已注册内存描述。

## 请求结构

### `Transfer`

单边传输请求。

- `op`：`Put` 或 `Get`。
- `local`：本地地址。
- `target_id`：目标端点 ID。
- `target_address`：目标地址。
- `length`：长度。
- `stream`：后端 stream。
- `context`：调用方上下文。

### `Message`

双边消息请求。

- `local`：本地地址。
- `target_id`：目标端点 ID。
- `target_address`：目标地址。
- `length`：长度。
- `tag`：匹配标签。
- `stream`：后端 stream。
- `context`：调用方上下文。

### `PreparedRequest`

后端已解析请求。

- `op`：操作方向。
- `target_id`：目标端点 ID。
- `local`：本地已注册内存。
- `remote`：远端内存描述。
- `local_offset`：本地偏移。
- `remote_offset`：远端偏移。
- `length`：长度。
- `tag`：消息标签。
- `stream`：后端 stream。
- `context`：调用方上下文。

## `Transport` 公共接口

```cpp
virtual Status init(void* options) = 0;
virtual Status shutdown() = 0;
```

初始化和关闭具体后端。

```cpp
Status registerMemory(const MemoryRegion& memory);
Status unregisterMemory(void* addr);
```

注册和注销本地内存。基类维护 `local_export_`。

```cpp
EndpointExport exportEndpoint() const;
EndpointID importEndpoint(const EndpointExport& remote);
void closeEndpoint(EndpointID id);
const EndpointExport& localEndpoint() const;
```

导出本端、导入远端、关闭远端和查看本端描述。

```cpp
Status submitTransfer(const Transfer& request);
Status send(const Message& request);
Status receive(const Message& request);
```

提交单边传输和双边消息。远端请求会先懒连接。

## 后端实现钩子

具体 transport 只需要实现 protected 钩子。

```cpp
virtual bool supportsMemory(const MemoryRegion& memory) const;
```

判断是否支持内存。默认支持。

```cpp
virtual Status doExportEndpoint(EndpointAttrs& out) const;
```

导出协议端点属性。

```cpp
virtual Status doRegisterMemory(const MemoryRegion& memory,
                                MemoryExport& out) = 0;
virtual Status doUnregisterMemory(void* addr) = 0;
```

注册和注销具体后端内存。

```cpp
virtual Status connect(EndpointID id, const EndpointExport& remote);
```

连接远端端点。默认成功。

```cpp
virtual Status doSubmitTransfer(const Transfer& request,
                                const EndpointExport& local,
                                const EndpointExport& remote);
virtual Status doSubmitSend(const Message& request,
                            const EndpointExport& local,
                            const EndpointExport& remote);
virtual Status doSubmitReceive(const Message& request,
                               const EndpointExport& local,
                               const EndpointExport& remote);
```

解析用户请求。默认实现会生成 `PreparedRequest`。

```cpp
virtual Status submitPrepared(const PreparedRequest& request) = 0;
virtual Status submitSendPrepared(const PreparedRequest& request);
virtual Status submitReceivePrepared(const PreparedRequest& request);
```

最终后端提交入口。

## 内部状态

`Transport` 基类维护：

- `local_memory_`：本地已注册内存表。
- `local_export_`：本端导出描述。
- `next_endpoint_id_`：下一个远端端点 ID。
- `remote_endpoints_`：远端端点描述表。
- `connected_endpoints_`：已连接远端端点。

## 示例

```cpp
HccsTransport hccs;
hccs.init(&options);

hccs.registerMemory(local_memory);
auto local = hccs.exportEndpoint();

auto remote_id = hccs.importEndpoint(remote_export);

Transfer request;
request.op = Operation::Put;
request.local = local_addr;
request.target_id = remote_id;
request.target_address = remote_addr;
request.length = length;

hccs.submitTransfer(request);
```

