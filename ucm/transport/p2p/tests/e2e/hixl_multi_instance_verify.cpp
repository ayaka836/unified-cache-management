#include <acl/acl.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <hixl/hixl.h>
#include <iostream>
#include <map>
#include <mutex>
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
constexpr const char* kHixlOptionGlobalResourceConfig = "GlobalResourceConfig";

#define VERIFY_LOG(expr)           \
    do {                           \
        std::cerr << expr << "\n"; \
        std::cerr.flush();         \
    } while (false)

const char* EnvText(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

int EnvInt(const char* name, int fallback)
{
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') { return fallback; }
    char* end = nullptr;
    const auto value = std::strtol(text, &end, 0);
    return end != nullptr && *end == '\0' ? static_cast<int>(value) : fallback;
}

uint64_t EnvU64(const char* name, uint64_t fallback)
{
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') { return fallback; }
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 0);
    return end != nullptr && *end == '\0' ? value : fallback;
}

std::string EnvString(const char* name, const std::string& fallback = {})
{
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

struct Config {
    std::string role;
    std::string local_host =
        EnvText("HIXL_VERIFY_LOCAL_HOST", EnvText("HIXL_TEST_LOCAL_HOST", "127.0.0.1"));
    std::string peer_host =
        EnvText("HIXL_VERIFY_PEER_HOST", EnvText("HIXL_TEST_PEER_HOST", "127.0.0.1"));
    uint16_t local_control_port =
        test::envPort("HIXL_VERIFY_CONTROL_PORT", test::envPort("TRANSPORT_CONTROL_PORT_A", 4701));
    uint16_t peer_control_port = test::envPort("HIXL_VERIFY_PEER_CONTROL_PORT",
                                               test::envPort("TRANSPORT_CONTROL_PORT_B", 4702));
    uint16_t hixl_port =
        test::envPort("HIXL_VERIFY_HIXL_PORT", test::envPort("HIXL_TEST_PORT_A", 5701));
    std::vector<int> devices;
    size_t instances = static_cast<size_t>(EnvU64("HIXL_VERIFY_INSTANCES", 1));
    size_t desc_size = static_cast<size_t>(EnvU64("HIXL_VERIFY_DESC_SIZE", 4096));
    size_t desc_count = static_cast<size_t>(EnvU64("HIXL_VERIFY_DESC_COUNT", 1));
    int connect_timeout_ms = EnvInt("HIXL_VERIFY_CONNECT_TIMEOUT_MS", kDefaultConnectTimeoutMs);
    int wait_attempts = EnvInt("HIXL_VERIFY_WAIT_ATTEMPTS", kDefaultWaitAttempts);
    int wait_interval_ms = EnvInt("HIXL_VERIFY_WAIT_RETRY_MS", kDefaultWaitIntervalMs);
    std::string op = EnvString("HIXL_VERIFY_OP", "both");
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

    bool Activate() const
    {
        if (context == nullptr) { return false; }
        const auto status = aclrtSetCurrentContext(context);
        if (status != ACL_ERROR_NONE) {
            std::cerr << "aclrtSetCurrentContext(" << device_id
                      << ") failed: " << static_cast<int>(status) << "\n";
            return false;
        }
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

struct RuntimeContextGuard {
    explicit RuntimeContextGuard(aclrtContext context) : context(context)
    {
        if (context == nullptr) { return; }
        const auto get_status = aclrtGetCurrentContext(&previous);
        if (get_status != ACL_ERROR_NONE) { previous = nullptr; }
        if (previous != context) {
            const auto set_status = aclrtSetCurrentContext(context);
            if (set_status != ACL_ERROR_NONE) {
                std::cerr << "aclrtSetCurrentContext failed: " << static_cast<int>(set_status)
                          << "\n";
                ok = false;
                return;
            }
        }
        if (!CheckCurrent("after set current context")) { return; }
        ok = true;
    }

    ~RuntimeContextGuard()
    {
        if (ok && previous != nullptr && previous != context) {
            const auto status = aclrtSetCurrentContext(previous);
            if (status != ACL_ERROR_NONE) {
                std::cerr << "restore acl context failed: " << static_cast<int>(status) << "\n";
            }
        }
    }

    RuntimeContextGuard(const RuntimeContextGuard&) = delete;
    RuntimeContextGuard& operator=(const RuntimeContextGuard&) = delete;

    bool CheckCurrent(const char* step) const
    {
        aclrtContext current = nullptr;
        const auto status = aclrtGetCurrentContext(&current);
        if (status != ACL_ERROR_NONE || current != context) {
            std::cerr << step << " context mismatch: expected=" << context << " current=" << current
                      << " status=" << static_cast<int>(status) << "\n";
            return false;
        }
        return true;
    }

    aclrtContext context = nullptr;
    aclrtContext previous = nullptr;
    bool ok = false;
};

struct HostBuffer {
    ~HostBuffer()
    {
        if (addr != nullptr) { (void)aclrtFreeHost(addr); }
    }

    void* addr = nullptr;
    size_t size = 0;
};

struct DeviceBuffer {
    ~DeviceBuffer()
    {
        if (addr != nullptr) { (void)aclrtFree(addr); }
    }

    void* addr = nullptr;
    size_t size = 0;
};

struct HixlInstance {
    DeviceContext device;
    DeviceBuffer buffer;
    hixl::Hixl engine;
    std::string local_engine;
    hixl::MemHandle mem_handle = nullptr;
};

std::mutex g_hixl_lifecycle_mutex;

struct PeerInstance {
    std::string remote_engine;
    uintptr_t remote_base = 0;
    size_t remote_size = 0;
};

struct ClientWorkerResult {
    bool ok = false;
};

class PhaseBarrier {
public:
    explicit PhaseBarrier(size_t count) : count_(count) {}

    bool Wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (aborted_) { return false; }
        const auto generation = generation_;
        ++arrived_;
        if (arrived_ == count_) {
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
            return true;
        }
        cv_.wait(lock, [&]() { return aborted_ || generation_ != generation; });
        return !aborted_;
    }

    void Abort()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }

private:
    size_t count_ = 0;
    size_t arrived_ = 0;
    size_t generation_ = 0;
    bool aborted_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
};

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
    } else if (key == "local-control-port") {
        if (!ParseU16(value, config.local_control_port)) { return false; }
    } else if (key == "peer-control-port") {
        if (!ParseU16(value, config.peer_control_port)) { return false; }
    } else if (key == "hixl-port") {
        if (!ParseU16(value, config.hixl_port)) { return false; }
    } else if (key == "devices") {
        if (!ParseDeviceList(value, config.devices)) { return false; }
        config.instances = config.devices.size();
    } else if (key == "instances") {
        if (!ParseSize(value, config.instances)) { return false; }
    } else if (key == "desc-size") {
        if (!ParseSize(value, config.desc_size)) { return false; }
    } else if (key == "desc-count") {
        if (!ParseSize(value, config.desc_count)) { return false; }
    } else if (key == "op") {
        config.op = value;
    } else if (key == "connect-timeout-ms") {
        if (!ParseInt(value, config.connect_timeout_ms)) { return false; }
    } else {
        return false;
    }
    return true;
}

bool ParseArgs(int argc, char** argv, Config& config)
{
    const auto env_devices = EnvString("HIXL_VERIFY_DEVICES");
    if (!env_devices.empty() && !ParseDeviceList(env_devices, config.devices)) {
        std::cerr << "invalid HIXL_VERIFY_DEVICES\n";
        return false;
    }
    if (!config.devices.empty()) { config.instances = config.devices.size(); }

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
              << "  --local-host HOST --peer-host HOST\n"
              << "  --local-control-port PORT --peer-control-port PORT\n"
              << "  --hixl-port PORT              base HIXL port; instance i uses base+i\n"
              << "  --devices 0,1,2,3             device list; count is instance count\n"
              << "  --instances N                 used only when --devices is omitted\n"
              << "  --desc-size BYTES             bytes per TransferOpDesc, default 4096\n"
              << "  --desc-count N                descs per transfer, default 1\n"
              << "  --op read|write|both          default both\n";
}

bool ValidateConfig(Config& config)
{
    if (config.role != "server" && config.role != "client") {
        std::cerr << "role must be server or client\n";
        return false;
    }
    if (config.instances == 0 || config.desc_size == 0 || config.desc_count == 0) {
        std::cerr << "instances, desc-size and desc-count must be positive\n";
        return false;
    }
    if (config.op != "read" && config.op != "write" && config.op != "both") {
        std::cerr << "op must be read, write or both\n";
        return false;
    }
    if (config.devices.empty()) {
        config.devices.reserve(config.instances);
        for (size_t i = 0; i < config.instances; ++i) {
            config.devices.push_back(static_cast<int>(i));
        }
    }
    if (config.devices.size() != config.instances) {
        std::cerr << "devices count must equal instances\n";
        return false;
    }
    if (config.role == "server" && config.instances != 1) {
        std::cerr << "server worker process must use exactly one HIXL instance\n";
        return false;
    }
    if (static_cast<uint32_t>(config.hixl_port) + config.instances - 1 > UINT16_MAX) {
        std::cerr << "hixl base port plus instances exceeds uint16 port range\n";
        return false;
    }
    if (static_cast<uint32_t>(config.peer_control_port) + config.instances - 1 > UINT16_MAX) {
        std::cerr << "peer control base port plus instances exceeds uint16 port range\n";
        return false;
    }
    return true;
}

std::string EngineName(const std::string& host, uint16_t base_port, size_t index)
{ return host + ":" + std::to_string(static_cast<uint32_t>(base_port) + index); }

uint16_t InstancePort(uint16_t base_port, size_t index)
{ return static_cast<uint16_t>(static_cast<uint32_t>(base_port) + index); }

std::string HixlGlobalResourceConfig(uint16_t listen_port)
{
    return std::string("{\"comm_resource_config.listen_port\":") + std::to_string(listen_port) +
           "}";
}

Endpoint PeerControlEndpoint(const Config& config, size_t index)
{
    return test::makeEndpoint(
        config.peer_host,
        static_cast<uint16_t>(static_cast<uint32_t>(config.peer_control_port) + index));
}

bool SendTextWithRetry(TcpMessageChannel& control, const Endpoint& peer, const std::string& text,
                       const Config& config, const char* step)
{
    return test::sendTextWithRetry(control, peer, text, config.wait_attempts,
                                   config.wait_interval_ms, step);
}

void FillPattern(unsigned char* data, size_t size, unsigned seed)
{
    for (size_t i = 0; i < size; ++i) { data[i] = static_cast<unsigned char>((seed + i) & 0xff); }
}

bool CopyPatternToDevice(const DeviceContext& context, void* device_addr, size_t size,
                         unsigned seed)
{
    RuntimeContextGuard guard(context.context);
    if (!guard.ok) { return false; }
    HostBuffer host;
    host.size = size;
    if (aclrtMallocHost(&host.addr, host.size) != ACL_ERROR_NONE) {
        std::cerr << "aclrtMallocHost pattern failed\n";
        return false;
    }
    FillPattern(static_cast<unsigned char*>(host.addr), host.size, seed);
    const auto status =
        aclrtMemcpy(device_addr, size, host.addr, host.size, ACL_MEMCPY_HOST_TO_DEVICE);
    if (status != ACL_ERROR_NONE) {
        std::cerr << "aclrtMemcpy H2D pattern failed: " << static_cast<int>(status) << "\n";
        return false;
    }
    return true;
}

bool VerifyDevicePattern(const DeviceContext& context, void* device_addr, size_t size,
                         unsigned seed)
{
    RuntimeContextGuard guard(context.context);
    if (!guard.ok) { return false; }
    HostBuffer host;
    host.size = size;
    if (aclrtMallocHost(&host.addr, host.size) != ACL_ERROR_NONE) {
        std::cerr << "aclrtMallocHost verify failed\n";
        return false;
    }
    const auto status =
        aclrtMemcpy(host.addr, host.size, device_addr, size, ACL_MEMCPY_DEVICE_TO_HOST);
    if (status != ACL_ERROR_NONE) {
        std::cerr << "aclrtMemcpy D2H verify failed: " << static_cast<int>(status) << "\n";
        return false;
    }
    const auto* data = static_cast<const unsigned char*>(host.addr);
    for (size_t i = 0; i < size; ++i) {
        const auto expected = static_cast<unsigned char>((seed + i) & 0xff);
        if (data[i] != expected) {
            std::cerr << "verify failed at offset " << i << ": got "
                      << static_cast<unsigned>(data[i]) << " expected "
                      << static_cast<unsigned>(expected) << "\n";
            return false;
        }
    }
    return true;
}

bool FillHostPattern(HostBuffer& host, unsigned seed)
{
    if (host.addr == nullptr || host.size == 0) { return false; }
    FillPattern(static_cast<unsigned char*>(host.addr), host.size, seed);
    return true;
}

bool VerifyHostPattern(const HostBuffer& host, unsigned seed)
{
    if (host.addr == nullptr || host.size == 0) { return false; }
    const auto* data = static_cast<const unsigned char*>(host.addr);
    for (size_t i = 0; i < host.size; ++i) {
        const auto expected = static_cast<unsigned char>((seed + i) & 0xff);
        if (data[i] != expected) {
            std::cerr << "host verify failed at offset " << i << ": got "
                      << static_cast<unsigned>(data[i]) << " expected "
                      << static_cast<unsigned>(expected) << "\n";
            return false;
        }
    }
    return true;
}

bool InitInstances(const Config& config, std::deque<HixlInstance>& instances)
{
    const size_t buffer_size = config.desc_size * config.desc_count;
    instances.resize(config.instances);
    for (size_t i = 0; i < instances.size(); ++i) {
        auto& item = instances[i];
        if (!item.device.Set(config.devices[i])) { return false; }
        item.buffer.size = buffer_size;
        if (aclrtMalloc(&item.buffer.addr, item.buffer.size, ACL_MEM_MALLOC_HUGE_ONLY) !=
            ACL_ERROR_NONE) {
            std::cerr << "aclrtMalloc device buffer failed for instance " << i << "\n";
            return false;
        }
        if (!CopyPatternToDevice(item.device, item.buffer.addr, item.buffer.size,
                                 static_cast<unsigned>(0x80 + i))) {
            return false;
        }
        item.local_engine = EngineName(config.local_host, config.hixl_port, i);
        const auto global_resource_config =
            HixlGlobalResourceConfig(InstancePort(config.hixl_port, i));
        const std::map<hixl::AscendString, hixl::AscendString> options = {
            {kHixlOptionGlobalResourceConfig, global_resource_config.c_str()}
        };
        RuntimeContextGuard guard(item.device.context);
        if (!guard.ok) { return false; }
        VERIFY_LOG("INIT_CONTEXT index=" << i << " device=" << item.device.device_id << " context="
                                         << item.device.context << " engine=" << item.local_engine
                                         << " global_resource_config=" << global_resource_config);
        std::lock_guard<std::mutex> hixl_lock(g_hixl_lifecycle_mutex);
        auto status = item.engine.Initialize(item.local_engine.c_str(), options);
        if (!guard.CheckCurrent("after Initialize")) { return false; }
        if (status != hixl::SUCCESS) {
            std::cerr << "Initialize(" << item.local_engine
                      << ") failed: " << static_cast<int>(status) << "\n";
            return false;
        }
        hixl::MemDesc mem_desc{reinterpret_cast<uintptr_t>(item.buffer.addr), item.buffer.size};
        status = item.engine.RegisterMem(mem_desc, hixl::MEM_DEVICE, item.mem_handle);
        if (!guard.CheckCurrent("after RegisterMem")) { return false; }
        if (status != hixl::SUCCESS) {
            std::cerr << "RegisterMem(" << item.local_engine
                      << ") failed: " << static_cast<int>(status) << "\n";
            return false;
        }
        VERIFY_LOG("INSTANCE index=" << i << " device=" << config.devices[i]
                                     << " engine=" << item.local_engine << " hbm=0x" << std::hex
                                     << reinterpret_cast<uintptr_t>(item.buffer.addr) << std::dec
                                     << " size=" << item.buffer.size);
    }
    return true;
}

bool FinalizeInstances(std::deque<HixlInstance>& instances)
{
    bool ok = true;
    for (auto& item : instances) {
        RuntimeContextGuard guard(item.device.context);
        if (!guard.ok) {
            ok = false;
            continue;
        }
        std::lock_guard<std::mutex> hixl_lock(g_hixl_lifecycle_mutex);
        if (item.mem_handle != nullptr) {
            (void)item.engine.DeregisterMem(item.mem_handle);
            (void)guard.CheckCurrent("after DeregisterMem");
            item.mem_handle = nullptr;
        }
        item.engine.Finalize();
        (void)guard.CheckCurrent("after Finalize");
        if (item.buffer.addr != nullptr) {
            (void)aclrtFree(item.buffer.addr);
            item.buffer.addr = nullptr;
        }
        item.device.Reset();
    }
    return ok;
}

std::string BuildAddressMessage(const std::deque<HixlInstance>& instances)
{
    std::ostringstream out;
    out << "ADDRS " << instances.size();
    for (const auto& item : instances) {
        out << " " << item.local_engine << " " << std::hex
            << reinterpret_cast<uintptr_t>(item.buffer.addr) << std::dec << " " << item.buffer.size;
    }
    return out.str();
}

bool ParseAddressMessage(const std::string& text, std::vector<PeerInstance>& peers)
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

std::vector<hixl::TransferOpDesc> BuildDescs(const Config& config, uintptr_t local_base,
                                             uintptr_t remote_base)
{
    std::vector<hixl::TransferOpDesc> descs;
    descs.reserve(config.desc_count);
    for (size_t i = 0; i < config.desc_count; ++i) {
        const auto offset = i * config.desc_size;
        descs.push_back(hixl::TransferOpDesc{
            static_cast<uintptr_t>(local_base + offset),
            static_cast<uintptr_t>(remote_base + offset),
            static_cast<size_t>(config.desc_size),
        });
    }
    return descs;
}

bool VerifyAll(const Config& config, const std::deque<HixlInstance>& instances, unsigned base_seed)
{
    const size_t buffer_size = config.desc_size * config.desc_count;
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!VerifyDevicePattern(instances[i].device, instances[i].buffer.addr, buffer_size,
                                 static_cast<unsigned>(base_seed + i))) {
            std::cerr << "verify instance " << i << " failed\n";
            return false;
        }
    }
    return true;
}

size_t RemotePeerIndex(size_t local_index, size_t count)
{ return count == 0 ? 0 : (local_index + 1) % count; }

bool RunClientInstance(const Config& config, size_t index, const PeerInstance& peer,
                       PhaseBarrier& init_barrier, PhaseBarrier& connect_barrier,
                       PhaseBarrier& transfer_barrier)
{
    const size_t buffer_size = config.desc_size * config.desc_count;
    DeviceContext device;
    HostBuffer buffer;
    hixl::Hixl engine;
    std::string local_engine;
    hixl::MemHandle mem_handle = nullptr;
    bool ok = true;
    bool initialized = false;
    auto cleanup = [&]() {
        RuntimeContextGuard guard(device.context);
        if (guard.ok) {
            std::lock_guard<std::mutex> hixl_lock(g_hixl_lifecycle_mutex);
            if (initialized) {
                (void)engine.Disconnect(peer.remote_engine.c_str(), config.connect_timeout_ms);
                (void)guard.CheckCurrent("worker after Disconnect");
                if (mem_handle != nullptr) {
                    (void)engine.DeregisterMem(mem_handle);
                    (void)guard.CheckCurrent("worker after DeregisterMem");
                    mem_handle = nullptr;
                }
                engine.Finalize();
                (void)guard.CheckCurrent("worker after Finalize");
            }
        }
        device.Reset();
    };
    auto fail = [&]() {
        ok = false;
        init_barrier.Abort();
        connect_barrier.Abort();
        transfer_barrier.Abort();
        return false;
    };

    if (!device.Set(config.devices[index])) { return fail(); }
    buffer.size = buffer_size;
    if (aclrtMallocHost(&buffer.addr, buffer.size) != ACL_ERROR_NONE) {
        std::cerr << "client worker " << index << " aclrtMallocHost buffer failed\n";
        return fail();
    }
    if (!FillHostPattern(buffer, static_cast<unsigned>(0x80))) { return fail(); }

    local_engine = EngineName(config.local_host, config.hixl_port, index);
    const auto global_resource_config =
        HixlGlobalResourceConfig(InstancePort(config.hixl_port, index));
    const std::map<hixl::AscendString, hixl::AscendString> options = {
        {kHixlOptionGlobalResourceConfig, global_resource_config.c_str()}
    };
    {
        RuntimeContextGuard guard(device.context);
        if (!guard.ok) { return fail(); }
        VERIFY_LOG("WORKER_INIT index=" << index << " device=" << device.device_id
                                        << " context=" << device.context
                                        << " engine=" << local_engine << " mem=host"
                                        << " global_resource_config=" << global_resource_config);
        std::lock_guard<std::mutex> hixl_lock(g_hixl_lifecycle_mutex);
        auto status = engine.Initialize(local_engine.c_str(), options);
        if (!guard.CheckCurrent("worker after Initialize")) { return fail(); }
        if (status != hixl::SUCCESS) {
            std::cerr << "Initialize(" << local_engine << ") failed: " << static_cast<int>(status)
                      << "\n";
            return fail();
        }
        initialized = true;
        hixl::MemDesc mem_desc{reinterpret_cast<uintptr_t>(buffer.addr), buffer.size};
        status = engine.RegisterMem(mem_desc, hixl::MEM_HOST, mem_handle);
        if (!guard.CheckCurrent("worker after RegisterMem")) { return fail(); }
        if (status != hixl::SUCCESS) {
            std::cerr << "RegisterMem(" << local_engine << ") failed: " << static_cast<int>(status)
                      << "\n";
            return fail();
        }
        VERIFY_LOG("WORKER_INSTANCE index=" << index << " device=" << device.device_id
                                            << " engine=" << local_engine << " host=0x" << std::hex
                                            << reinterpret_cast<uintptr_t>(buffer.addr) << std::dec
                                            << " size=" << buffer.size);
    }
    if (!init_barrier.Wait()) {
        cleanup();
        return false;
    }

    {
        RuntimeContextGuard guard(device.context);
        if (!guard.ok) { return fail(); }
        VERIFY_LOG("WORKER_CONNECT index=" << index << " device=" << device.device_id << " context="
                                           << device.context << " local_engine=" << local_engine
                                           << " remote_engine=" << peer.remote_engine);
        std::lock_guard<std::mutex> hixl_lock(g_hixl_lifecycle_mutex);
        const auto status = engine.Connect(peer.remote_engine.c_str(), config.connect_timeout_ms);
        if (!guard.CheckCurrent("worker after Connect")) { return fail(); }
        if (status != hixl::SUCCESS) {
            std::cerr << "Connect(" << peer.remote_engine
                      << ") failed: " << static_cast<int>(status) << "\n";
            ok = false;
        }
    }
    if (!connect_barrier.Wait()) {
        cleanup();
        return false;
    }
    if (!ok) {
        transfer_barrier.Abort();
        cleanup();
        return false;
    }

    if (config.op == "write" || config.op == "both") {
        if (!FillHostPattern(buffer, static_cast<unsigned>(0x10 + index))) { return fail(); }
        RuntimeContextGuard guard(device.context);
        if (!guard.ok) {
            transfer_barrier.Abort();
            cleanup();
            return false;
        }
        const auto descs =
            BuildDescs(config, reinterpret_cast<uintptr_t>(buffer.addr), peer.remote_base);
        VERIFY_LOG("WORKER_TRANSFER_WRITE index="
                   << index << " device=" << device.device_id << " context=" << device.context
                   << " local_engine=" << local_engine << " remote_engine=" << peer.remote_engine);
        const auto status = engine.TransferSync(peer.remote_engine.c_str(), hixl::WRITE, descs,
                                                config.connect_timeout_ms);
        if (!guard.CheckCurrent("worker after write TransferSync")) {
            transfer_barrier.Abort();
            cleanup();
            return false;
        }
        if (status != hixl::SUCCESS) {
            std::cerr << "TransferSync write(" << peer.remote_engine << ", index=" << index
                      << ") failed: " << static_cast<int>(status) << "\n";
            ok = false;
        }
    }

    if (config.op == "read" || config.op == "both") {
        const auto read_seed = config.op == "both" ? static_cast<unsigned>(0x10 + index) : 0x80;
        RuntimeContextGuard guard(device.context);
        if (!guard.ok) {
            transfer_barrier.Abort();
            cleanup();
            return false;
        }
        const auto descs =
            BuildDescs(config, reinterpret_cast<uintptr_t>(buffer.addr), peer.remote_base);
        VERIFY_LOG("WORKER_TRANSFER_READ index="
                   << index << " device=" << device.device_id << " context=" << device.context
                   << " local_engine=" << local_engine << " remote_engine=" << peer.remote_engine);
        const auto status = engine.TransferSync(peer.remote_engine.c_str(), hixl::READ, descs,
                                                config.connect_timeout_ms);
        if (!guard.CheckCurrent("worker after read TransferSync")) {
            transfer_barrier.Abort();
            cleanup();
            return false;
        }
        if (status != hixl::SUCCESS) {
            std::cerr << "TransferSync read(" << peer.remote_engine << ", index=" << index
                      << ") failed: " << static_cast<int>(status) << "\n";
            ok = false;
        }
        if (ok && !VerifyHostPattern(buffer, read_seed)) { ok = false; }
    }
    if (!transfer_barrier.Wait()) { ok = false; }
    cleanup();
    VERIFY_LOG("WORKER_DONE index=" << index);
    return ok;
}

void PrintConfig(const Config& config)
{
    VERIFY_LOG("role=" << config.role << " local=" << config.local_host
                       << " peer=" << config.peer_host << " instances=" << config.instances
                       << " desc_size=" << config.desc_size << " desc_count=" << config.desc_count
                       << " op=" << config.op
                       << " buffer_size=" << config.desc_size * config.desc_count);
}

int RunServer(const Config& config)
{
    PrintConfig(config);
    TcpMessageChannel control;
    if (!test::expectOk(
            control.Init(test::makeEndpoint(config.local_host, config.local_control_port)),
            "server init control channel")) {
        return 1;
    }
    AclRuntime acl;
    if (!acl.ok) { return 1; }
    std::deque<HixlInstance> instances;
    if (!InitInstances(config, instances)) { return 1; }

    Endpoint client;
    std::string message;
    VERIFY_LOG("SERVER_WAIT_READY control=" << config.local_host << ":"
                                            << config.local_control_port);
    if (!test::receiveText(control, client, message, "server receive READY") ||
        message != "READY") {
        return 1;
    }
    VERIFY_LOG("SERVER_RECEIVED_READY from=" << client.ToString());
    if (!SendTextWithRetry(control, client, BuildAddressMessage(instances), config,
                           "server send ADDRS")) {
        return 1;
    }
    VERIFY_LOG("SERVER_SENT_ADDRS");

    while (true) {
        if (!test::receiveText(control, client, message, "server receive command")) { return 1; }
        VERIFY_LOG("SERVER_COMMAND " << message);
        if (message.rfind("CHECK_WRITE", 0) == 0) {
            std::istringstream in(message);
            std::string command;
            unsigned seed = 0x10;
            in >> command >> seed;
            const bool ok = VerifyAll(config, instances, seed);
            VERIFY_LOG("SERVER_CHECK_WRITE seed=" << seed << " result=" << (ok ? "PASS" : "FAIL"));
            if (!SendTextWithRetry(control, client, ok ? "PASS" : "FAIL", config,
                                   "server send CHECK_WRITE result")) {
                return 1;
            }
            if (!ok) { return 1; }
            continue;
        }
        if (message == "DONE") { break; }
        std::cerr << "unexpected command: " << message << "\n";
        return 1;
    }

    if (!FinalizeInstances(instances)) { return 1; }
    if (!test::expectOk(control.Shutdown(), "server shutdown control channel")) { return 1; }
    VERIFY_LOG("SERVER_DONE");
    return 0;
}

int RunClient(const Config& config)
{
    PrintConfig(config);
    TcpMessageChannel control;
    if (!test::expectOk(
            control.Init(test::makeEndpoint(config.local_host, config.local_control_port)),
            "client init control channel")) {
        return 1;
    }
    std::string message;
    std::vector<PeerInstance> peers;
    peers.resize(config.instances);
    for (size_t i = 0; i < config.instances; ++i) {
        const auto peer_index = RemotePeerIndex(i, config.instances);
        if (!SendTextWithRetry(control, PeerControlEndpoint(config, peer_index), "READY", config,
                               "client send READY")) {
            return 1;
        }
        std::vector<PeerInstance> received;
        if (!test::receiveText(control, message, "client receive ADDRS") ||
            !ParseAddressMessage(message, received) || received.size() != 1) {
            return 1;
        }
        peers[i] = received.front();
        VERIFY_LOG("PAIR local_index=" << i << " remote_index=" << peer_index
                                       << " remote_engine=" << peers[i].remote_engine);
    }

    AclRuntime acl;
    if (!acl.ok) { return 1; }

    std::vector<ClientWorkerResult> results(config.instances);
    PhaseBarrier init_barrier(config.instances);
    PhaseBarrier connect_barrier(config.instances);
    PhaseBarrier transfer_barrier(config.instances);
    std::vector<std::thread> threads;
    threads.reserve(config.instances);
    for (size_t i = 0; i < config.instances; ++i) {
        threads.emplace_back([&, i]() {
            results[i].ok = RunClientInstance(config, i, peers[i], init_barrier, connect_barrier,
                                              transfer_barrier);
        });
    }
    bool ok = true;
    for (auto& thread : threads) { thread.join(); }
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].ok) {
            std::cerr << "client worker " << i << " failed\n";
            ok = false;
        }
    }
    if (!ok) { return 1; }

    if (config.op == "write" || config.op == "both") {
        for (size_t i = 0; i < config.instances; ++i) {
            const auto peer_index = RemotePeerIndex(i, config.instances);
            const auto command = std::string("CHECK_WRITE ") + std::to_string(0x10 + i);
            if (!SendTextWithRetry(control, PeerControlEndpoint(config, peer_index), command,
                                   config, "client send CHECK_WRITE")) {
                return 1;
            }
            if (!test::receiveText(control, message, "client receive CHECK_WRITE result") ||
                message != "PASS") {
                std::cerr << "write verification failed: " << message << "\n";
                return 1;
            }
        }
        VERIFY_LOG("VERIFY write PASS");
    }

    if (config.op == "read" || config.op == "both") { VERIFY_LOG("VERIFY read PASS"); }

    for (size_t i = 0; i < config.instances; ++i) {
        if (!SendTextWithRetry(control, PeerControlEndpoint(config, i), "DONE", config,
                               "client send DONE")) {
            return 1;
        }
    }
    if (!test::expectOk(control.Shutdown(), "client shutdown control channel")) { return 1; }
    VERIFY_LOG("RESULT instances=" << config.instances << " op=" << config.op << " PASS");
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
