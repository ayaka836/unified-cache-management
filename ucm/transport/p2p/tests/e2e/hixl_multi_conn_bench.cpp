#include <acl/acl.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <hixl/hixl.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "test_common.h"

using namespace transport;

namespace {

constexpr int kDefaultConnectTimeoutMs = 30000;
constexpr int kDefaultWaitAttempts = 600;
constexpr int kDefaultWaitIntervalMs = 100;

const char* EnvText(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

uint64_t EnvU64(const char* name, uint64_t fallback)
{
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') { return fallback; }
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 0);
    return end != nullptr && *end == '\0' ? value : fallback;
}

int EnvInt(const char* name, int fallback)
{
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') { return fallback; }
    char* end = nullptr;
    const auto value = std::strtol(text, &end, 0);
    return end != nullptr && *end == '\0' ? static_cast<int>(value) : fallback;
}

std::string EnvString(const char* name, const std::string& fallback = {})
{
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

enum class SubmitMode {
    Serial,
    Parallel,
};

enum class OpKind {
    Read,
    Write,
};

struct Config {
    std::string role;
    std::string local_host =
        EnvText("HIXL_BENCH_LOCAL_HOST", EnvText("HIXL_TEST_LOCAL_HOST", "127.0.0.1"));
    std::string peer_host =
        EnvText("HIXL_BENCH_PEER_HOST", EnvText("HIXL_TEST_PEER_HOST", "127.0.0.1"));
    uint16_t server_control_port =
        test::envPort("HIXL_BENCH_CONTROL_PORT_A", test::envPort("TRANSPORT_CONTROL_PORT_A", 4601));
    uint16_t client_control_port =
        test::envPort("HIXL_BENCH_CONTROL_PORT_B", test::envPort("TRANSPORT_CONTROL_PORT_B", 4602));
    uint16_t server_hixl_port =
        test::envPort("HIXL_BENCH_HIXL_PORT_A", test::envPort("HIXL_TEST_PORT_A", 5501));
    uint16_t client_hixl_port =
        test::envPort("HIXL_BENCH_HIXL_PORT_B", test::envPort("HIXL_TEST_PORT_B", 5601));
    int server_device_id = EnvInt("HIXL_BENCH_DEVICE_A", EnvInt("HIXL_TEST_DEVICE_A", 0));
    int client_device_id = EnvInt("HIXL_BENCH_DEVICE_B", EnvInt("HIXL_TEST_DEVICE_B", 1));
    std::vector<int> server_devices;
    size_t connections = static_cast<size_t>(EnvU64("HIXL_BENCH_CONNECTIONS", 1));
    size_t desc_size = static_cast<size_t>(EnvU64("HIXL_BENCH_DESC_SIZE", 4096));
    size_t desc_count = static_cast<size_t>(EnvU64("HIXL_BENCH_DESC_COUNT", 1));
    size_t inflight_depth = static_cast<size_t>(EnvU64("HIXL_BENCH_INFLIGHT_DEPTH", 1));
    size_t iterations = static_cast<size_t>(EnvU64("HIXL_BENCH_ITERATIONS", 1000));
    int connect_timeout_ms = EnvInt("HIXL_BENCH_CONNECT_TIMEOUT_MS", kDefaultConnectTimeoutMs);
    int wait_attempts = EnvInt("HIXL_BENCH_WAIT_ATTEMPTS", kDefaultWaitAttempts);
    int wait_interval_ms = EnvInt("HIXL_BENCH_WAIT_RETRY_MS", kDefaultWaitIntervalMs);
    int poll_interval_us = EnvInt("HIXL_BENCH_POLL_INTERVAL_US", 0);
    SubmitMode submit_mode = EnvString("HIXL_BENCH_SUBMIT_MODE", "serial") == "parallel"
                                 ? SubmitMode::Parallel
                                 : SubmitMode::Serial;
    OpKind op = EnvString("HIXL_BENCH_OP", "write") == "read" ? OpKind::Read : OpKind::Write;
    std::string global_resource_config = EnvString("HIXL_BENCH_GLOBAL_RESOURCE_CONFIG");
};

struct AclRuntime {
    AclRuntime()
    {
        const auto status = aclInit(nullptr);
        if (status != ACL_ERROR_NONE) {
            std::cerr << "aclInit failed: " << static_cast<int>(status) << "\n";
            return;
        }
        ok = true;
    }

    ~AclRuntime()
    {
        if (ok) { (void)aclFinalize(); }
    }

    AclRuntime(const AclRuntime&) = delete;
    AclRuntime& operator=(const AclRuntime&) = delete;

    bool ok = false;
};

struct DeviceContext {
    int device_id = -1;
    aclrtContext context = nullptr;
    bool active = false;

    bool Set(int device)
    {
        device_id = device;
        const auto set_status = aclrtSetDevice(device_id);
        if (set_status != ACL_ERROR_NONE) {
            std::cerr << "aclrtSetDevice(" << device_id
                      << ") failed: " << static_cast<int>(set_status) << "\n";
            return false;
        }
        const auto get_status = aclrtGetCurrentContext(&context);
        if (get_status != ACL_ERROR_NONE || context == nullptr) {
            std::cerr << "aclrtGetCurrentContext failed for device " << device_id << ": "
                      << static_cast<int>(get_status) << "\n";
            return false;
        }
        active = true;
        return true;
    }

    void Reset()
    {
        if (active) {
            (void)aclrtResetDevice(device_id);
            active = false;
        }
    }
};

struct DeviceBuffer {
    ~DeviceBuffer()
    {
        if (addr != nullptr) { (void)aclrtFree(addr); }
    }

    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    void* addr = nullptr;
    size_t size = 0;
};

struct HostBuffer {
    ~HostBuffer()
    {
        if (addr != nullptr) { (void)aclrtFreeHost(addr); }
    }

    HostBuffer() = default;
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    void* addr = nullptr;
    size_t size = 0;
};

struct ServerEngine {
    DeviceContext device;
    DeviceBuffer buffer;
    hixl::Hixl engine;
    std::string local_engine;
    hixl::MemHandle mem_handle = nullptr;
};

struct ClientPeer {
    std::string remote_engine;
    uintptr_t remote_base = 0;
    size_t remote_size = 0;
};

struct Slot {
    std::vector<hixl::TransferOpDesc> descs;
    hixl::TransferReq req = nullptr;
    bool active = false;
};

struct PeerWork {
    ClientPeer* peer = nullptr;
    std::vector<Slot> slots;
    size_t submitted = 0;
    size_t completed = 0;
    size_t failed = 0;
    uint64_t submit_ns = 0;
    uint64_t status_ns = 0;
    size_t status_queries = 0;
};

uint64_t NowNs();

bool ParseU16(const std::string& text, uint16_t& out)
{
    char* end = nullptr;
    const auto value = std::strtoul(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0' || value == 0 || value > UINT16_MAX) { return false; }
    out = static_cast<uint16_t>(value);
    return true;
}

bool ParseSize(const std::string& text, size_t& out)
{
    char* end = nullptr;
    const auto value = std::strtoull(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0') { return false; }
    out = static_cast<size_t>(value);
    return true;
}

bool ParseInt(const std::string& text, int& out)
{
    char* end = nullptr;
    const auto value = std::strtol(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0') { return false; }
    out = static_cast<int>(value);
    return true;
}

bool ParseDeviceList(const std::string& text, std::vector<int>& devices)
{
    devices.clear();
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        int device = -1;
        if (!ParseInt(item, device)) { return false; }
        devices.push_back(device);
    }
    return !devices.empty();
}

bool ApplyOption(Config& config, const std::string& key, const std::string& value)
{
    if (key == "role") {
        config.role = value;
    } else if (key == "local-host") {
        config.local_host = value;
    } else if (key == "peer-host") {
        config.peer_host = value;
    } else if (key == "server-control-port") {
        if (!ParseU16(value, config.server_control_port)) { return false; }
    } else if (key == "client-control-port") {
        if (!ParseU16(value, config.client_control_port)) { return false; }
    } else if (key == "server-hixl-port") {
        if (!ParseU16(value, config.server_hixl_port)) { return false; }
    } else if (key == "client-hixl-port") {
        if (!ParseU16(value, config.client_hixl_port)) { return false; }
    } else if (key == "server-device") {
        if (!ParseInt(value, config.server_device_id)) { return false; }
    } else if (key == "client-device") {
        if (!ParseInt(value, config.client_device_id)) { return false; }
    } else if (key == "server-devices") {
        if (!ParseDeviceList(value, config.server_devices)) { return false; }
        config.connections = config.server_devices.size();
    } else if (key == "connections") {
        if (!ParseSize(value, config.connections)) { return false; }
    } else if (key == "desc-size") {
        if (!ParseSize(value, config.desc_size)) { return false; }
    } else if (key == "desc-count") {
        if (!ParseSize(value, config.desc_count)) { return false; }
    } else if (key == "inflight-depth") {
        if (!ParseSize(value, config.inflight_depth)) { return false; }
    } else if (key == "iterations") {
        if (!ParseSize(value, config.iterations)) { return false; }
    } else if (key == "submit-mode") {
        if (value == "serial") {
            config.submit_mode = SubmitMode::Serial;
        } else if (value == "parallel") {
            config.submit_mode = SubmitMode::Parallel;
        } else {
            return false;
        }
    } else if (key == "op") {
        if (value == "read") {
            config.op = OpKind::Read;
        } else if (value == "write") {
            config.op = OpKind::Write;
        } else {
            return false;
        }
    } else if (key == "connect-timeout-ms") {
        if (!ParseInt(value, config.connect_timeout_ms)) { return false; }
    } else if (key == "poll-interval-us") {
        if (!ParseInt(value, config.poll_interval_us)) { return false; }
    } else if (key == "global-resource-config") {
        config.global_resource_config = value;
    } else {
        return false;
    }
    return true;
}

bool ParseArgs(int argc, char** argv, Config& config)
{
    const auto env_devices = EnvString("HIXL_BENCH_SERVER_DEVICES");
    if (!env_devices.empty() && !ParseDeviceList(env_devices, config.server_devices)) {
        std::cerr << "invalid HIXL_BENCH_SERVER_DEVICES\n";
        return false;
    }
    if (!config.server_devices.empty()) { config.connections = config.server_devices.size(); }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { return false; }
        if (arg == "server" || arg == "client") {
            config.role = arg;
            continue;
        }
        if (arg.rfind("--", 0) != 0) {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
        arg = arg.substr(2);
        std::string value;
        const auto equal = arg.find('=');
        if (equal == std::string::npos) {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --" << arg << "\n";
                return false;
            }
            value = argv[++i];
        } else {
            value = arg.substr(equal + 1);
            arg = arg.substr(0, equal);
        }
        if (!ApplyOption(config, arg, value)) {
            std::cerr << "invalid option: --" << arg << "=" << value << "\n";
            return false;
        }
    }
    return true;
}

void PrintUsage(const char* program)
{
    std::cerr << "usage: " << program << " server|client [options]\n"
              << "options:\n"
              << "  --server-devices 2,4,6      peer devices; one peer engine per device\n"
              << "  --connections N             peer count if --server-devices is omitted\n"
              << "  --desc-size BYTES           bytes per TransferOpDesc, default 4096\n"
              << "  --desc-count N              descs per submitted task, default 1\n"
              << "  --inflight-depth N          max outstanding async tasks per peer, default 1\n"
              << "  --iterations N              tasks per peer, default 1000\n"
              << "  --submit-mode serial|parallel\n"
              << "  --op read|write\n"
              << "  --local-host HOST --peer-host HOST\n"
              << "  --server-device ID --client-device ID\n"
              << "  --server-control-port PORT --client-control-port PORT\n"
              << "  --server-hixl-port PORT --client-hixl-port PORT\n"
              << "  --global-resource-config JSON\n";
}

bool CheckedMul(size_t a, size_t b, size_t& out)
{
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) { return false; }
    out = a * b;
    return true;
}

bool ValidateConfig(Config& config)
{
    if (config.role != "server" && config.role != "client") {
        std::cerr << "role must be server or client\n";
        return false;
    }
    if (config.connections == 0 || config.desc_size == 0 || config.desc_count == 0 ||
        config.inflight_depth == 0 || config.iterations == 0) {
        std::cerr << "connections, desc-size, desc-count, inflight-depth and iterations must be "
                     "positive\n";
        return false;
    }
    if (config.server_devices.empty()) {
        config.server_devices.reserve(config.connections);
        for (size_t i = 0; i < config.connections; ++i) {
            config.server_devices.push_back(config.server_device_id + static_cast<int>(i));
        }
    }
    if (config.server_devices.size() != config.connections) {
        std::cerr << "server-devices count must equal connections\n";
        return false;
    }
    size_t task_bytes = 0;
    size_t buffer_size = 0;
    if (!CheckedMul(config.desc_size, config.desc_count, task_bytes) ||
        !CheckedMul(task_bytes, config.inflight_depth, buffer_size)) {
        std::cerr << "buffer size overflows size_t\n";
        return false;
    }
    size_t local_size = 0;
    if (!CheckedMul(buffer_size, config.connections, local_size)) {
        std::cerr << "local buffer size overflows size_t\n";
        return false;
    }
    if (static_cast<uint32_t>(config.server_hixl_port) + config.connections - 1 > UINT16_MAX) {
        std::cerr << "server hixl base port plus connections exceeds uint16 port range\n";
        return false;
    }
    if (static_cast<uint32_t>(config.server_control_port) + config.connections - 1 > UINT16_MAX) {
        std::cerr << "server control base port plus connections exceeds uint16 port range\n";
        return false;
    }
    const auto server_begin = static_cast<uint32_t>(config.server_hixl_port);
    const auto server_end = server_begin + static_cast<uint32_t>(config.connections) - 1;
    const auto client_port = static_cast<uint32_t>(config.client_hixl_port);
    if (config.local_host == config.peer_host && client_port >= server_begin &&
        client_port <= server_end) {
        std::cerr << "client hixl port " << config.client_hixl_port
                  << " overlaps server hixl engine port range [" << server_begin << ", "
                  << server_end << "]. Set --client-hixl-port to a free port.\n";
        return false;
    }
    return true;
}

std::string SubmitModeName(SubmitMode mode)
{ return mode == SubmitMode::Serial ? "serial" : "parallel"; }

std::string OpName(OpKind op) { return op == OpKind::Read ? "read" : "write"; }

uint16_t LocalControlPort(const Config& config)
{ return config.role == "server" ? config.server_control_port : config.client_control_port; }

Endpoint ServerControlEndpoint(const Config& config, size_t index)
{
    return test::makeEndpoint(
        config.peer_host,
        static_cast<uint16_t>(static_cast<uint32_t>(config.server_control_port) + index));
}

std::string EngineName(const std::string& host, uint16_t base_port, size_t index = 0)
{ return host + ":" + std::to_string(static_cast<uint32_t>(base_port) + index); }

std::map<hixl::AscendString, hixl::AscendString> HixlOptions(const Config& config)
{
    std::map<hixl::AscendString, hixl::AscendString> options;
    if (!config.global_resource_config.empty()) {
        options.emplace("GlobalResourceConfig", config.global_resource_config.c_str());
    }
    return options;
}

bool SendTextWithRetry(TcpMessageChannel& control, const Endpoint& peer, const std::string& text,
                       const Config& config, const char* step)
{
    return test::sendTextWithRetry(control, peer, text, config.wait_attempts,
                                   config.wait_interval_ms, step);
}

void FillPattern(unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) { data[i] = static_cast<unsigned char>(i & 0xff); }
}

bool InitClientEngine(const Config& config, hixl::Hixl& engine, const hixl::MemDesc& mem_desc,
                      hixl::MemHandle& mem_handle)
{
    const auto options = HixlOptions(config);
    const auto local_engine = EngineName(config.local_host, config.client_hixl_port);
    auto status = engine.Initialize(local_engine.c_str(), options);
    if (status != hixl::SUCCESS) {
        std::cerr << "Initialize(" << local_engine << ") failed: " << static_cast<int>(status)
                  << "\n";
        return false;
    }
    status = engine.RegisterMem(mem_desc, hixl::MEM_HOST, mem_handle);
    if (status != hixl::SUCCESS) {
        std::cerr << "client RegisterMem failed: " << static_cast<int>(status) << "\n";
        return false;
    }
    return true;
}

bool InitServerEngines(const Config& config, size_t per_peer_buffer_size,
                       std::deque<ServerEngine>& engines)
{
    const auto options = HixlOptions(config);
    engines.resize(config.connections);
    for (size_t i = 0; i < engines.size(); ++i) {
        auto& item = engines[i];
        if (!item.device.Set(config.server_devices[i])) { return false; }
        item.buffer.size = per_peer_buffer_size;
        if (aclrtMalloc(&item.buffer.addr, item.buffer.size, ACL_MEM_MALLOC_HUGE_ONLY) !=
                ACL_ERROR_NONE ||
            aclrtMemset(item.buffer.addr, item.buffer.size, 0, item.buffer.size) !=
                ACL_ERROR_NONE) {
            std::cerr << "server allocate/clear device buffer failed for peer " << i << "\n";
            return false;
        }
        if (config.op == OpKind::Read) {
            HostBuffer pattern;
            pattern.size = item.buffer.size;
            if (aclrtMallocHost(&pattern.addr, pattern.size) != ACL_ERROR_NONE) {
                std::cerr << "server allocate host pattern failed for peer " << i << "\n";
                return false;
            }
            FillPattern(static_cast<unsigned char*>(pattern.addr), pattern.size);
            if (aclrtMemcpy(item.buffer.addr, item.buffer.size, pattern.addr, pattern.size,
                            ACL_MEMCPY_HOST_TO_DEVICE) != ACL_ERROR_NONE) {
                std::cerr << "server initialize device pattern failed for peer " << i << "\n";
                return false;
            }
        }

        item.local_engine = EngineName(config.local_host, config.server_hixl_port, i);
        auto status = item.engine.Initialize(item.local_engine.c_str(), options);
        if (status != hixl::SUCCESS) {
            std::cerr << "Initialize(" << item.local_engine
                      << ") failed: " << static_cast<int>(status) << "\n";
            return false;
        }

        hixl::MemDesc mem_desc{reinterpret_cast<uintptr_t>(item.buffer.addr), item.buffer.size};
        status = item.engine.RegisterMem(mem_desc, hixl::MEM_DEVICE, item.mem_handle);
        if (status != hixl::SUCCESS) {
            std::cerr << "server RegisterMem failed for peer " << i << ": "
                      << static_cast<int>(status) << "\n";
            return false;
        }
    }
    return true;
}

void FinalizeServerEngines(std::deque<ServerEngine>& engines)
{
    for (auto& item : engines) {
        if (item.device.context != nullptr) { (void)aclrtSetCurrentContext(item.device.context); }
        if (item.mem_handle != nullptr) {
            (void)item.engine.DeregisterMem(item.mem_handle);
            item.mem_handle = nullptr;
        }
        item.engine.Finalize();
        if (item.buffer.addr != nullptr) {
            (void)aclrtFree(item.buffer.addr);
            item.buffer.addr = nullptr;
        }
        item.device.Reset();
    }
}

std::string BuildAddressMessage(const std::deque<ServerEngine>& engines)
{
    std::ostringstream out;
    out << "ADDRS " << engines.size();
    for (const auto& item : engines) {
        out << " " << item.local_engine << " " << std::hex
            << reinterpret_cast<uintptr_t>(item.buffer.addr) << std::dec << " " << item.buffer.size;
    }
    return out.str();
}

bool ParseAddressMessage(const std::string& text, std::vector<ClientPeer>& peers)
{
    std::istringstream in(text);
    std::string tag;
    size_t count = 0;
    if (!(in >> tag >> count) || tag != "ADDRS" || count == 0) { return false; }
    peers.clear();
    peers.resize(count);
    for (auto& peer : peers) {
        if (!(in >> peer.remote_engine >> std::hex >> peer.remote_base >> std::dec >>
              peer.remote_size)) {
            return false;
        }
        if (peer.remote_engine.empty() || peer.remote_base == 0 || peer.remote_size == 0) {
            return false;
        }
    }
    return true;
}

bool ReceivePeerAddress(TcpMessageChannel& control, std::vector<ClientPeer>& peers)
{
    std::string message;
    std::vector<ClientPeer> received;
    if (!test::receiveText(control, message, "client receive ADDRS") ||
        !ParseAddressMessage(message, received) || received.size() != 1) {
        return false;
    }
    peers.push_back(received.front());
    return true;
}

bool ConnectAll(hixl::Hixl& engine, const std::vector<ClientPeer>& peers, int timeout_ms)
{
    for (const auto& peer : peers) {
        const auto status = engine.Connect(peer.remote_engine.c_str(), timeout_ms);
        if (status != hixl::SUCCESS) {
            std::cerr << "Connect(" << peer.remote_engine
                      << ") failed: " << static_cast<int>(status) << "\n";
            return false;
        }
    }
    return true;
}

void DisconnectAll(hixl::Hixl& engine, const std::vector<ClientPeer>& peers, int timeout_ms)
{
    for (const auto& peer : peers) {
        (void)engine.Disconnect(peer.remote_engine.c_str(), timeout_ms);
    }
}

bool BuildWork(const Config& config, std::vector<ClientPeer>& peers, uintptr_t local_base,
               size_t per_peer_buffer_size, std::vector<PeerWork>& work)
{
    const size_t task_bytes = config.desc_size * config.desc_count;
    if (peers.size() != config.connections) { return false; }
    work.clear();
    work.resize(peers.size());
    for (size_t peer_index = 0; peer_index < peers.size(); ++peer_index) {
        auto& item = work[peer_index];
        item.peer = &peers[peer_index];
        item.slots.resize(config.inflight_depth);
        if (peers[peer_index].remote_size < per_peer_buffer_size) {
            std::cerr << "remote buffer " << peer_index
                      << " too small: " << peers[peer_index].remote_size << " < "
                      << per_peer_buffer_size << "\n";
            return false;
        }
        for (size_t slot_index = 0; slot_index < item.slots.size(); ++slot_index) {
            auto& slot = item.slots[slot_index];
            slot.descs.reserve(config.desc_count);
            const auto local_slot_base =
                peer_index * per_peer_buffer_size + slot_index * task_bytes;
            const auto remote_slot_base = slot_index * task_bytes;
            for (size_t desc_index = 0; desc_index < config.desc_count; ++desc_index) {
                const auto desc_offset = desc_index * config.desc_size;
                slot.descs.push_back(hixl::TransferOpDesc{
                    static_cast<uintptr_t>(local_base + local_slot_base + desc_offset),
                    static_cast<uintptr_t>(item.peer->remote_base + remote_slot_base + desc_offset),
                    static_cast<size_t>(config.desc_size),
                });
            }
        }
    }
    return true;
}

bool SubmitSlot(hixl::Hixl& engine, PeerWork& work, Slot& slot, OpKind op)
{
    hixl::TransferArgs args;
    const auto hixl_op = op == OpKind::Read ? hixl::READ : hixl::WRITE;
    const auto begin_ns = NowNs();
    const auto status =
        engine.TransferAsync(work.peer->remote_engine.c_str(), hixl_op, slot.descs, args, slot.req);
    work.submit_ns += NowNs() - begin_ns;
    if (status != hixl::SUCCESS || slot.req == nullptr) {
        std::cerr << "TransferAsync(" << work.peer->remote_engine << ", descs=" << slot.descs.size()
                  << ") failed: " << static_cast<int>(status) << " req=" << slot.req << "\n";
        ++work.failed;
        return false;
    }
    slot.active = true;
    ++work.submitted;
    return true;
}

bool PollSlot(hixl::Hixl& engine, PeerWork& work, Slot& slot)
{
    hixl::TransferStatus transfer_status = hixl::TransferStatus::WAITING;
    const auto begin_ns = NowNs();
    const auto status = engine.GetTransferStatus(slot.req, transfer_status);
    work.status_ns += NowNs() - begin_ns;
    ++work.status_queries;
    if (status != hixl::SUCCESS) {
        std::cerr << "GetTransferStatus(" << work.peer->remote_engine
                  << ") failed: " << static_cast<int>(status) << "\n";
        slot.active = false;
        slot.req = nullptr;
        ++work.failed;
        return false;
    }
    if (transfer_status == hixl::TransferStatus::WAITING) { return true; }
    slot.active = false;
    slot.req = nullptr;
    if (transfer_status == hixl::TransferStatus::COMPLETED) {
        ++work.completed;
        return true;
    }
    std::cerr << "transfer on " << work.peer->remote_engine
              << " failed with status=" << static_cast<int>(transfer_status) << "\n";
    ++work.failed;
    return false;
}

bool DrivePeer(hixl::Hixl& engine, PeerWork& work, const Config& config)
{
    while (work.completed < config.iterations && work.failed == 0) {
        for (auto& slot : work.slots) {
            if (slot.active || work.submitted >= config.iterations) { continue; }
            if (!SubmitSlot(engine, work, slot, config.op)) { return false; }
        }
        bool any_active = false;
        for (auto& slot : work.slots) {
            if (!slot.active) { continue; }
            any_active = true;
            if (!PollSlot(engine, work, slot)) { return false; }
        }
        if (any_active && config.poll_interval_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(config.poll_interval_us));
        }
    }
    return work.failed == 0;
}

bool RunSerial(hixl::Hixl& engine, std::vector<PeerWork>& work, const Config& config)
{
    const auto total_iterations = config.iterations * work.size();
    size_t total_completed = 0;
    while (total_completed < total_iterations) {
        total_completed = 0;
        for (auto& item : work) {
            if (item.failed != 0) { return false; }
            for (auto& slot : item.slots) {
                if (slot.active || item.submitted >= config.iterations) { continue; }
                if (!SubmitSlot(engine, item, slot, config.op)) { return false; }
            }
            bool any_active = false;
            for (auto& slot : item.slots) {
                if (!slot.active) { continue; }
                any_active = true;
                if (!PollSlot(engine, item, slot)) { return false; }
            }
            total_completed += item.completed;
            if (any_active && config.poll_interval_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(config.poll_interval_us));
            }
        }
    }
    return true;
}

bool RunParallel(hixl::Hixl& engine, std::vector<PeerWork>& work, const Config& config,
                 aclrtContext context)
{
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < work.size(); ++i) {
        threads.emplace_back([&, i]() {
            if (context != nullptr) {
                const auto status = aclrtSetCurrentContext(context);
                if (status != ACL_ERROR_NONE) {
                    std::cerr << "worker " << i
                              << " aclrtSetCurrentContext failed: " << static_cast<int>(status)
                              << "\n";
                    ok.store(false);
                    return;
                }
            }
            if (!DrivePeer(engine, work[i], config)) { ok.store(false); }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    return ok.load();
}

uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void PrintConfig(const Config& config, size_t local_buffer_size, size_t per_peer_buffer_size)
{
    std::cout << "role=" << config.role << " local=" << config.local_host
              << " peer=" << config.peer_host << " connections=" << config.connections
              << " desc_size=" << config.desc_size << " desc_count=" << config.desc_count
              << " inflight_depth=" << config.inflight_depth << " iterations=" << config.iterations
              << " submit_mode=" << SubmitModeName(config.submit_mode)
              << " op=" << OpName(config.op) << " local_buffer_size=" << local_buffer_size
              << " per_peer_buffer_size=" << per_peer_buffer_size << "\n";
}

void PrintResult(const Config& config, uint64_t elapsed_ns, const std::vector<PeerWork>& work)
{
    const double seconds = static_cast<double>(elapsed_ns) / 1e9;
    const auto tasks = config.connections * config.iterations;
    const auto bytes_per_task = config.desc_size * config.desc_count;
    const auto bytes = static_cast<double>(tasks) * static_cast<double>(bytes_per_task);
    const double gib = bytes / 1024.0 / 1024.0 / 1024.0;
    const double gbps = seconds > 0 ? (bytes * 8.0 / seconds / 1e9) : 0.0;
    const double iops = seconds > 0 ? static_cast<double>(tasks) / seconds : 0.0;
    uint64_t submit_ns = 0;
    uint64_t status_ns = 0;
    size_t status_queries = 0;
    for (const auto& item : work) {
        submit_ns += item.submit_ns;
        status_ns += item.status_ns;
        status_queries += item.status_queries;
    }
    const double submit_seconds = static_cast<double>(submit_ns) / 1e9;
    const double status_seconds = static_cast<double>(status_ns) / 1e9;
    const double submit_avg_ns = tasks > 0 ? static_cast<double>(submit_ns) / tasks : 0.0;
    const double status_avg_ns =
        status_queries > 0 ? static_cast<double>(status_ns) / status_queries : 0.0;
    std::cout << "RESULT"
              << " submit_mode=" << SubmitModeName(config.submit_mode)
              << " op=" << OpName(config.op) << " connections=" << config.connections
              << " desc_size=" << config.desc_size << " desc_count=" << config.desc_count
              << " inflight_depth=" << config.inflight_depth << " iterations=" << config.iterations
              << " tasks=" << tasks << " bytes=" << static_cast<uint64_t>(bytes)
              << " seconds=" << seconds << " submit_seconds=" << submit_seconds
              << " submit_avg_ns=" << submit_avg_ns << " status_seconds=" << status_seconds
              << " status_avg_ns=" << status_avg_ns << " status_queries=" << status_queries
              << " GiB=" << gib << " Gbps=" << gbps << " task_per_sec=" << iops << "\n";
}

int RunServer(const Config& config)
{
    const size_t task_bytes = config.desc_size * config.desc_count;
    const size_t per_peer_buffer_size = task_bytes * config.inflight_depth;
    const size_t local_buffer_size = per_peer_buffer_size * config.connections;
    PrintConfig(config, local_buffer_size, per_peer_buffer_size);

    TcpMessageChannel control;
    if (!test::expectOk(
            control.Init(test::makeEndpoint(config.local_host, LocalControlPort(config))),
            "server init control channel")) {
        return 1;
    }

    AclRuntime acl;
    if (!acl.ok) { return 1; }

    std::deque<ServerEngine> engines;
    if (!InitServerEngines(config, per_peer_buffer_size, engines)) { return 1; }

    std::cout << "[server] waiting READY\n";
    Endpoint client;
    std::string message;
    if (!test::receiveText(control, client, message, "server receive READY") ||
        message != "READY") {
        return 1;
    }
    if (!SendTextWithRetry(control, client, BuildAddressMessage(engines), config,
                           "server send ADDRS")) {
        return 1;
    }

    std::cout << "[server] waiting DONE\n";
    if (!test::receiveText(control, client, message, "server receive DONE") || message != "DONE") {
        return 1;
    }

    FinalizeServerEngines(engines);
    if (!test::expectOk(control.Shutdown(), "server shutdown control channel")) { return 1; }
    return 0;
}

int RunClient(const Config& config)
{
    const size_t task_bytes = config.desc_size * config.desc_count;
    const size_t per_peer_buffer_size = task_bytes * config.inflight_depth;
    const size_t local_buffer_size = per_peer_buffer_size * config.connections;
    PrintConfig(config, local_buffer_size, per_peer_buffer_size);

    TcpMessageChannel control;
    if (!test::expectOk(
            control.Init(test::makeEndpoint(config.local_host, LocalControlPort(config))),
            "client init control channel")) {
        return 1;
    }
    std::vector<ClientPeer> peers;
    peers.reserve(config.connections);
    for (size_t i = 0; i < config.connections; ++i) {
        if (!SendTextWithRetry(control, ServerControlEndpoint(config, i), "READY", config,
                               "client send READY")) {
            return 1;
        }
        if (!ReceivePeerAddress(control, peers)) { return 1; }
    }

    AclRuntime acl;
    if (!acl.ok) { return 1; }
    DeviceContext client_device;
    if (!client_device.Set(config.client_device_id)) { return 1; }

    HostBuffer buffer;
    buffer.size = local_buffer_size;
    if (aclrtMallocHost(&buffer.addr, buffer.size) != ACL_ERROR_NONE) {
        std::cerr << "client allocate host buffer failed\n";
        return 1;
    }
    if (config.op == OpKind::Write) {
        FillPattern(static_cast<unsigned char*>(buffer.addr), buffer.size);
    } else {
        std::memset(buffer.addr, 0, buffer.size);
    }

    hixl::Hixl engine;
    hixl::MemHandle mem_handle = nullptr;
    hixl::MemDesc mem_desc{reinterpret_cast<uintptr_t>(buffer.addr), buffer.size};
    if (!InitClientEngine(config, engine, mem_desc, mem_handle)) { return 1; }

    if (peers.size() != config.connections) {
        std::cerr << "peer count mismatch: " << peers.size() << " != " << config.connections
                  << "\n";
        return 1;
    }

    if (!ConnectAll(engine, peers, config.connect_timeout_ms)) { return 1; }

    std::vector<PeerWork> work;
    if (!BuildWork(config, peers, reinterpret_cast<uintptr_t>(buffer.addr), per_peer_buffer_size,
                   work)) {
        return 1;
    }

    const auto start_ns = NowNs();
    const bool ok = config.submit_mode == SubmitMode::Serial
                        ? RunSerial(engine, work, config)
                        : RunParallel(engine, work, config, client_device.context);
    const auto elapsed_ns = NowNs() - start_ns;
    if (!ok) { return 1; }
    PrintResult(config, elapsed_ns, work);

    for (size_t i = 0; i < config.connections; ++i) {
        if (!SendTextWithRetry(control, ServerControlEndpoint(config, i), "DONE", config,
                               "client send DONE")) {
            return 1;
        }
    }

    DisconnectAll(engine, peers, config.connect_timeout_ms);
    if (mem_handle != nullptr) { (void)engine.DeregisterMem(mem_handle); }
    engine.Finalize();
    client_device.Reset();
    if (!test::expectOk(control.Shutdown(), "client shutdown control channel")) { return 1; }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Config config;
    if (!ParseArgs(argc, argv, config) || !ValidateConfig(config)) {
        PrintUsage(argv[0]);
        return 1;
    }
    if (config.role == "server") { return RunServer(config); }
    return RunClient(config);
}
