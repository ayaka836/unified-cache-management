#include <acl/acl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/transport_init_attrs.h"
#include "core/transport_manager.h"
#include "drampool/drampool_config.h"
#include "kv_protocol.h"
#include "two_sided/tcp/tcp_message_channel.h"

namespace {

using UC::DramPool::BlockId;
using UC::DramPool::DramPoolConfig;
using UC::DramPool::KvDumpEntry;
using UC::DramPool::KvDumpRequest;
using UC::DramPool::KvLoadEntry;
using UC::DramPool::KvLoadRequest;
using UC::DramPool::KvLookupEntry;
using UC::DramPool::KvLookupRequest;
using UC::DramPool::KvOpcode;
using UC::DramPool::KvResponse;
using UC::DramPool::ProtocolManager;

constexpr std::uint8_t kSuccessResult = 0;
constexpr auto kPollInterval = std::chrono::microseconds(50);

std::mutex g_print_mutex;

void Print(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << message << std::endl;
}

struct Options {
    std::string config_path = "examples/drampool.yaml";
    std::string pool_control;
    std::string store_control;
    std::vector<std::int32_t> devices;
    std::size_t block_size = 4096;
    std::size_t block_num = 1;
    std::size_t rounds = 1;
    std::size_t put_count = 1;
    std::size_t get_count = 1;
    std::size_t lookup_exist_count = 1;
    std::size_t lookup_miss_count = 1;
};

std::vector<std::string> Split(const std::string& value)
{
    std::vector<std::string> values;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) { values.push_back(item); }
    }
    return values;
}

bool ParseSize(const std::string& value, std::size_t& result)
{
    try {
        std::size_t parsed = 0;
        const auto number = std::stoull(value, &parsed);
        if (parsed != value.size() || number > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        result = static_cast<std::size_t>(number);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--help") { return false; }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << name << std::endl;
            return false;
        }
        const std::string value = argv[++i];
        if (name == "--config") {
            options.config_path = value;
        } else if (name == "--pool-control") {
            options.pool_control = value;
        } else if (name == "--store-control") {
            options.store_control = value;
        } else if (name == "--devices") {
            for (const auto& item : Split(value)) {
                try {
                    options.devices.push_back(std::stoi(item));
                } catch (...) {
                    std::cerr << "invalid device id: " << item << std::endl;
                    return false;
                }
            }
        } else if (name == "--block-size") {
            if (!ParseSize(value, options.block_size)) { return false; }
        } else if (name == "--block-num") {
            if (!ParseSize(value, options.block_num)) { return false; }
        } else if (name == "--rounds") {
            if (!ParseSize(value, options.rounds)) { return false; }
        } else if (name == "--put") {
            if (!ParseSize(value, options.put_count)) { return false; }
        } else if (name == "--get") {
            if (!ParseSize(value, options.get_count)) { return false; }
        } else if (name == "--lookup-exist") {
            if (!ParseSize(value, options.lookup_exist_count)) { return false; }
        } else if (name == "--lookup-miss") {
            if (!ParseSize(value, options.lookup_miss_count)) { return false; }
        } else {
            std::cerr << "unknown option: " << name << std::endl;
            return false;
        }
    }
    return !options.pool_control.empty() && !options.store_control.empty() &&
           !options.devices.empty() && options.block_size > 0 &&
           options.block_size <= std::numeric_limits<std::uint32_t>::max() &&
           options.block_num > 0 &&
           options.block_num <= std::numeric_limits<std::uint16_t>::max() && options.rounds > 0;
}

void PrintUsage(const char* program)
{
    std::cerr << "Usage: " << program << " --config PATH --pool-control IP:PORT"
              << " --store-control IP:PORT --devices ID[,ID...]"
              << " [--block-size N] [--block-num N] [--rounds N] [--put N] [--get N]"
              << " [--lookup-exist N] [--lookup-miss N]" << std::endl;
}

std::uint64_t Mix(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

BlockId MakeKey(std::size_t store_index, std::uint64_t sequence, bool missing)
{
    BlockId key{};
    auto first = Mix((static_cast<std::uint64_t>(store_index) << 48U) ^ sequence ^
                     (missing ? 0xd1b54a32d192ed03ULL : 0));
    auto second = Mix(first);
    std::memcpy(key.data(), &first, sizeof(first));
    std::memcpy(key.data() + sizeof(first), &second, sizeof(second));
    return key;
}

void FillData(void* address, std::size_t length, const BlockId& key)
{
    auto seed = std::uint64_t{0};
    std::memcpy(&seed, key.data(), sizeof(seed));
    auto* bytes = static_cast<std::uint8_t*>(address);
    for (std::size_t i = 0; i < length; ++i) {
        if (i % sizeof(seed) == 0) { seed = Mix(seed + i); }
        bytes[i] = static_cast<std::uint8_t>(seed >> ((i % sizeof(seed)) * 8U));
    }
}

bool CheckData(const void* address, std::size_t length, const BlockId& key)
{
    std::vector<std::uint8_t> expected(length);
    FillData(expected.data(), expected.size(), key);
    return std::memcmp(address, expected.data(), length) == 0;
}

enum class RequestKind {
    Put,
    Get,
    LookupExist,
    LookupMiss,
};

struct RequestPlan {
    RequestKind kind;
    std::vector<BlockId> keys;
};

struct StoreConfig {
    std::size_t index = 0;
    std::int32_t device_id = -1;
    transport::Endpoint control_endpoint;
    transport::ManagerID manager_id;
    transport::Endpoint pool_control_endpoint;
    transport::ManagerID pool_manager_id;
    std::size_t block_size = 0;
    std::size_t block_num = 0;
    std::size_t rounds = 0;
    std::size_t put_count = 0;
    std::size_t get_count = 0;
    std::size_t lookup_exist_count = 0;
    std::size_t lookup_miss_count = 0;
    std::uint32_t timeout_ms = 0;
    std::uint32_t ttl_ms = 0;
};

class SimulatedStore {
public:
    explicit SimulatedStore(StoreConfig config) : config_(std::move(config)) {}

    void Start() { worker_ = std::thread(&SimulatedStore::Run, this); }

    bool Join()
    {
        if (worker_.joinable()) { worker_.join(); }
        if (!error_.empty()) {
            Print("store[" + std::to_string(config_.index) + "] failed: " + error_);
            return false;
        }
        return true;
    }

private:
    struct RequestSlot {
        std::atomic_bool active{false};
        std::atomic_bool done{false};
        bool success = false;
        std::string error;
        RequestKind kind = RequestKind::Put;
        std::vector<BlockId> keys;
        std::uint8_t* data = nullptr;
        std::uint8_t* response = nullptr;
    };

    void Run()
    {
        const auto acl_status = aclrtSetDevice(config_.device_id);
        if (acl_status != ACL_SUCCESS) {
            error_ = "aclrtSetDevice returned " + std::to_string(acl_status);
            return;
        }

        if (Initialize()) {
            const auto warmup_count = std::max(config_.get_count, config_.lookup_exist_count);
            if (warmup_count != 0 && !WarmUp(warmup_count)) {
                if (error_.empty()) { error_ = "warmup failed"; }
            } else {
                for (std::size_t round = 0; round < config_.rounds && error_.empty(); ++round) {
                    if (!RunRound(round)) { break; }
                    Print("store[" + std::to_string(config_.index) + "] round " +
                          std::to_string(round + 1) + " passed");
                }
            }
        }

        Finalize();
        const auto reset_status = aclrtResetDevice(config_.device_id);
        if (reset_status != ACL_SUCCESS && error_.empty()) {
            error_ = "aclrtResetDevice returned " + std::to_string(reset_status);
        }
    }

    bool Initialize()
    {
        const auto round_requests = config_.put_count + config_.get_count +
                                    config_.lookup_exist_count + config_.lookup_miss_count;
        const auto warmup_requests = std::max(config_.get_count, config_.lookup_exist_count);
        slot_count_ = std::max<std::size_t>(1, std::max(round_requests, warmup_requests));
        if (config_.block_num > std::numeric_limits<std::size_t>::max() / config_.block_size) {
            error_ = "buffer size overflow";
            return false;
        }
        data_slot_size_ = config_.block_num * config_.block_size;
        response_slot_size_ =
            UC::DramPool::kResponseResultsOffset +
            UC::DramPool::Packed4BitResultSize(config_.block_num);
        if (slot_count_ > std::numeric_limits<std::size_t>::max() / data_slot_size_) {
            error_ = "buffer size overflow";
            return false;
        }

        manager_ = std::make_unique<transport::TransportManager>(config_.manager_id);
        transport::HixlInitAttrs attrs;
        transport::Endpoint manager_endpoint;
        if (const auto status = UC::DramPool::ParseDramPoolEndpoint(
                "store one-sided endpoint", config_.manager_id, manager_endpoint);
            status.Failure()) {
            error_ = status.ToString();
            return false;
        }
        const auto& host = manager_endpoint.host;
        attrs.ip = host;
        transport::HixlInitAttrs::Instance instance;
        instance.port = -1;
        instance.device_id = config_.device_id;
        attrs.instances.push_back(std::move(instance));
        attrs.connect_timeout_ms = static_cast<std::int32_t>(config_.timeout_ms);
        attrs.transfer_timeout_ms = static_cast<std::int32_t>(config_.timeout_ms);
        if (manager_->InstallTransport(transport::TransportProtocol::Hixl, attrs) !=
            transport::Status::Ok) {
            error_ = "InstallTransport failed";
            return false;
        }

        void* data_buffer = nullptr;
        void* flag_buffer = nullptr;
        if (aclrtMallocHost(&data_buffer, slot_count_ * data_slot_size_) != ACL_SUCCESS ||
            aclrtMallocHost(&flag_buffer, slot_count_ * response_slot_size_) != ACL_SUCCESS) {
            if (data_buffer != nullptr) { aclrtFreeHost(data_buffer); }
            if (flag_buffer != nullptr) { aclrtFreeHost(flag_buffer); }
            error_ = "aclrtMallocHost failed";
            return false;
        }
        data_buffer_ = static_cast<std::uint8_t*>(data_buffer);
        flag_buffer_ = static_cast<std::uint8_t*>(flag_buffer);

        transport::MemoryRegion data_memory{data_buffer_, slot_count_ * data_slot_size_,
                                            transport::MemoryType::Host, config_.device_id};
        transport::MemoryRegion flag_memory{flag_buffer_, slot_count_ * response_slot_size_,
                                            transport::MemoryType::Host, config_.device_id};
        if (manager_->RegisterMemory(data_memory, data_handle_) != transport::Status::Ok ||
            manager_->RegisterMemory(flag_memory, flag_handle_) != transport::Status::Ok) {
            error_ = "RegisterMemory failed";
            return false;
        }
        if (manager_->Init() != transport::Status::Ok) {
            error_ = "TransportManager::Init failed";
            return false;
        }
        manager_started_ = true;
        if (control_.Init(config_.control_endpoint) != transport::Status::Ok) {
            error_ = "TcpMessageChannel::Init failed";
            return false;
        }
        control_started_ = true;
        if (manager_->ExchangeMetadata(config_.pool_manager_id) != transport::Status::Ok ||
            manager_->Connect(transport::TransportProtocol::Hixl, config_.pool_manager_id) !=
                transport::Status::Ok) {
            error_ = "metadata exchange or HIXL connect failed";
            return false;
        }
        connected_ = true;

        for (std::size_t i = 0; i < slot_count_; ++i) {
            auto slot = std::make_unique<RequestSlot>();
            slot->data = data_buffer_ + i * data_slot_size_;
            slot->response = flag_buffer_ + i * response_slot_size_;
            slots_.push_back(std::move(slot));
        }
        poller_stop_.store(false, std::memory_order_release);
        poller_ = std::thread(&SimulatedStore::PollResponses, this);
        return true;
    }

    bool WarmUp(std::size_t count)
    {
        std::vector<RequestPlan> requests;
        requests.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            RequestPlan request{RequestKind::Put, {}};
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                request.keys.push_back(MakeKey(config_.index, next_key_++, false));
            }
            requests.push_back(std::move(request));
        }
        if (!ExecuteRequests(requests)) { return false; }
        for (const auto& request : requests) {
            existing_keys_.insert(existing_keys_.end(), request.keys.begin(), request.keys.end());
        }
        return true;
    }

    bool RunRound(std::size_t round)
    {
        std::vector<RequestPlan> requests;
        requests.reserve(config_.put_count + config_.get_count + config_.lookup_exist_count +
                         config_.lookup_miss_count);
        for (std::size_t i = 0; i < config_.put_count; ++i) {
            RequestPlan request{RequestKind::Put, {}};
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                request.keys.push_back(MakeKey(config_.index, next_key_++, false));
            }
            requests.push_back(std::move(request));
        }
        for (std::size_t i = 0; i < config_.get_count; ++i) {
            RequestPlan request{RequestKind::Get, {}};
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                request.keys.push_back(
                    existing_keys_[(i * config_.block_num + block) % existing_keys_.size()]);
            }
            requests.push_back(std::move(request));
        }
        for (std::size_t i = 0; i < config_.lookup_exist_count; ++i) {
            RequestPlan request{RequestKind::LookupExist, {}};
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                request.keys.push_back(
                    existing_keys_[(i * config_.block_num + block) % existing_keys_.size()]);
            }
            requests.push_back(std::move(request));
        }
        for (std::size_t i = 0; i < config_.lookup_miss_count; ++i) {
            RequestPlan request{RequestKind::LookupMiss, {}};
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                request.keys.push_back(MakeKey(config_.index, next_missing_key_++, true));
            }
            requests.push_back(std::move(request));
        }
        std::mt19937_64 random((config_.index + 1) * 0x9e3779b97f4a7c15ULL + round);
        std::shuffle(requests.begin(), requests.end(), random);
        if (!ExecuteRequests(requests)) { return false; }
        for (const auto& request : requests) {
            if (request.kind == RequestKind::Put) {
                existing_keys_.insert(existing_keys_.end(), request.keys.begin(),
                                      request.keys.end());
            }
        }
        return true;
    }

    bool ExecuteRequests(const std::vector<RequestPlan>& requests)
    {
        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (!Submit(*slots_[i], requests[i])) { return false; }
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(config_.timeout_ms);
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& slot = *slots_[i];
            while (!slot.done.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(kPollInterval);
            }
            slot.active.store(false, std::memory_order_release);
            if (!slot.done.load(std::memory_order_acquire)) {
                error_ = "request timed out";
                return false;
            }
            if (!slot.success) {
                error_ = slot.error;
                return false;
            }
        }
        return true;
    }

    bool Submit(RequestSlot& slot, const RequestPlan& plan)
    {
        slot.done.store(false, std::memory_order_relaxed);
        slot.success = false;
        slot.error.clear();
        slot.kind = plan.kind;
        slot.keys = plan.keys;
        std::memset(slot.response, 0, response_slot_size_);

        KvOpcode opcode = KvOpcode::None;
        std::unique_ptr<UC::DramPool::KvRequest> request;
        if (plan.kind == RequestKind::Put) {
            auto dump = std::make_unique<KvDumpRequest>();
            dump->opcode = KvOpcode::Dump;
            dump->resp_addr = reinterpret_cast<std::uint64_t>(slot.response);
            dump->batch_size = static_cast<std::uint16_t>(config_.block_num);
            dump->ttl = config_.ttl_ms;
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                auto* data = slot.data + block * config_.block_size;
                FillData(data, config_.block_size, plan.keys[block]);
                dump->entries.push_back(
                    {plan.keys[block], reinterpret_cast<std::uint64_t>(data),
                     static_cast<std::uint32_t>(config_.block_size),
                     static_cast<std::uint32_t>(block)});
            }
            opcode = KvOpcode::Dump;
            request = std::move(dump);
        } else if (plan.kind == RequestKind::Get) {
            std::memset(slot.data, 0, data_slot_size_);
            auto load = std::make_unique<KvLoadRequest>();
            load->opcode = KvOpcode::Load;
            load->resp_addr = reinterpret_cast<std::uint64_t>(slot.response);
            load->batch_size = static_cast<std::uint16_t>(config_.block_num);
            for (std::size_t block = 0; block < config_.block_num; ++block) {
                auto* data = slot.data + block * config_.block_size;
                load->entries.push_back(
                    {plan.keys[block], reinterpret_cast<std::uint64_t>(data),
                     static_cast<std::uint32_t>(config_.block_size),
                     static_cast<std::uint32_t>(block)});
            }
            opcode = KvOpcode::Load;
            request = std::move(load);
        } else {
            auto lookup = std::make_unique<KvLookupRequest>();
            lookup->opcode = KvOpcode::Lookup;
            lookup->resp_addr = reinterpret_cast<std::uint64_t>(slot.response);
            lookup->batch_size = static_cast<std::uint16_t>(config_.block_num);
            for (const auto& key : plan.keys) {
                lookup->entries.push_back(KvLookupEntry{key});
            }
            opcode = KvOpcode::Lookup;
            request = std::move(lookup);
        }

        std::vector<std::uint8_t> packed;
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            packed.resize(protocol_.GetPackedSize(opcode, *request));
            const auto status = protocol_.PackRequest(packed.data(), opcode, *request);
            if (status.Failure()) {
                error_ = "PackRequest failed: " + status.ToString();
                return false;
            }
        }
        slot.active.store(true, std::memory_order_release);
        if (control_.Send(config_.pool_control_endpoint, packed.data(), packed.size()) !=
            transport::Status::Ok) {
            slot.active.store(false, std::memory_order_release);
            error_ = "TcpMessageChannel::Send failed";
            return false;
        }
        return true;
    }

    void PollResponses()
    {
        while (!poller_stop_.load(std::memory_order_acquire)) {
            for (const auto& slot_ptr : slots_) {
                auto& slot = *slot_ptr;
                if (!slot.active.load(std::memory_order_acquire) ||
                    slot.done.load(std::memory_order_relaxed)) {
                    continue;
                }

                bool ready = false;
                KvResponse response;
                UC::Status status = UC::Status::OK();
                {
                    std::lock_guard<std::mutex> lock(protocol_mutex_);
                    status = protocol_.IsResponseReady(slot.response, ready);
                    if (status.Success() && ready) {
                        const auto opcode = slot.kind == RequestKind::Put
                                                ? KvOpcode::Dump
                                                : (slot.kind == RequestKind::Get ? KvOpcode::Load
                                                                                 : KvOpcode::Lookup);
                        status = protocol_.UnpackResponse(
                            slot.response, opcode, static_cast<std::uint16_t>(config_.block_num),
                            response);
                    }
                }
                if (status.Failure()) {
                    Complete(slot, false, "invalid response: " + status.ToString());
                    continue;
                }
                if (!ready) { continue; }
                if (response.results.size() != config_.block_num) {
                    Complete(slot, false, "response result count is incorrect");
                    continue;
                }

                bool valid = true;
                for (std::size_t block = 0; block < config_.block_num && valid; ++block) {
                    if (slot.kind == RequestKind::LookupExist) {
                        valid = response.results[block] == 1;
                    } else if (slot.kind == RequestKind::LookupMiss) {
                        valid = response.results[block] == 0;
                    } else {
                        valid = response.results[block] == kSuccessResult;
                        if (valid && slot.kind == RequestKind::Get) {
                            valid = CheckData(slot.data + block * config_.block_size,
                                              config_.block_size, slot.keys[block]);
                        }
                    }
                }
                Complete(slot, valid, valid ? "" : "response validation failed");
            }
            std::this_thread::sleep_for(kPollInterval);
        }
    }

    static void Complete(RequestSlot& slot, bool success, std::string error)
    {
        slot.success = success;
        slot.error = std::move(error);
        slot.done.store(true, std::memory_order_release);
    }

    void Finalize()
    {
        poller_stop_.store(true, std::memory_order_release);
        if (poller_.joinable()) { poller_.join(); }
        if (connected_) {
            manager_->Disconnect(transport::TransportProtocol::Hixl, config_.pool_manager_id);
        }
        if (flag_handle_ != transport::kInvalidMemoryHandle) {
            manager_->UnregisterMemory(flag_handle_);
        }
        if (data_handle_ != transport::kInvalidMemoryHandle) {
            manager_->UnregisterMemory(data_handle_);
        }
        if (control_started_) { control_.Shutdown(); }
        if (manager_started_) { manager_->Shutdown(); }
        manager_.reset();
        if (flag_buffer_ != nullptr) { aclrtFreeHost(flag_buffer_); }
        if (data_buffer_ != nullptr) { aclrtFreeHost(data_buffer_); }
        flag_buffer_ = nullptr;
        data_buffer_ = nullptr;
    }

    StoreConfig config_;
    std::thread worker_;
    std::thread poller_;
    std::atomic_bool poller_stop_{true};
    std::string error_;
    std::unique_ptr<transport::TransportManager> manager_;
    transport::TcpMessageChannel control_;
    ProtocolManager protocol_;
    std::mutex protocol_mutex_;
    std::vector<std::unique_ptr<RequestSlot>> slots_;
    std::vector<BlockId> existing_keys_;
    std::uint8_t* data_buffer_ = nullptr;
    std::uint8_t* flag_buffer_ = nullptr;
    transport::MemoryHandle data_handle_ = transport::kInvalidMemoryHandle;
    transport::MemoryHandle flag_handle_ = transport::kInvalidMemoryHandle;
    std::size_t slot_count_ = 0;
    std::size_t data_slot_size_ = 0;
    std::size_t response_slot_size_ = 0;
    std::uint64_t next_key_ = 1;
    std::uint64_t next_missing_key_ = 1;
    bool manager_started_ = false;
    bool control_started_ = false;
    bool connected_ = false;
};

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 2;
    }

    DramPoolConfig runtime_config;
    if (const auto status = UC::DramPool::ParseDramPoolEndpoint(
            "--pool-control", options.pool_control, runtime_config.addr);
        status.Failure()) {
        std::cerr << status.ToString() << std::endl;
        return 2;
    }
    if (const auto status = UC::DramPool::ParseYamlConfig(options.config_path, runtime_config);
        status.Failure()) {
        std::cerr << status.ToString() << std::endl;
        return 2;
    }

    const auto pool_control_id = runtime_config.addr.ToString();
    const auto pool_manager = runtime_config.twoSidedToOneSided.find(pool_control_id);
    if (pool_manager == runtime_config.twoSidedToOneSided.end()) {
        std::cerr << "pool control endpoint is not present in the config" << std::endl;
        return 2;
    }

    transport::Endpoint pool_control_endpoint;
    UC::DramPool::ParseDramPoolEndpoint("--pool-control", options.pool_control,
                                        pool_control_endpoint);
    transport::Endpoint first_store_control;
    if (const auto status = UC::DramPool::ParseDramPoolEndpoint(
            "--store-control", options.store_control, first_store_control);
        status.Failure()) {
        std::cerr << status.ToString() << std::endl;
        return 2;
    }
    const auto available_ports = static_cast<std::size_t>(
        std::numeric_limits<std::uint16_t>::max() - first_store_control.port);
    if (options.devices.size() - 1 > available_ports) {
        std::cerr << "store control port range is invalid" << std::endl;
        return 2;
    }

    std::vector<std::unique_ptr<SimulatedStore>> stores;
    for (std::size_t i = 0; i < options.devices.size(); ++i) {
        auto control_endpoint = first_store_control;
        control_endpoint.port = static_cast<std::uint16_t>(control_endpoint.port + i);
        const auto manager =
            runtime_config.twoSidedToOneSided.find(control_endpoint.ToString());
        if (manager == runtime_config.twoSidedToOneSided.end()) {
            std::cerr << "store control endpoint is not present in the config: "
                      << control_endpoint.ToString() << std::endl;
            return 2;
        }

        StoreConfig config;
        config.index = i;
        config.device_id = options.devices[i];
        config.control_endpoint = std::move(control_endpoint);
        config.manager_id = manager->second;
        config.pool_control_endpoint = pool_control_endpoint;
        config.pool_manager_id = pool_manager->second;
        config.block_size = options.block_size;
        config.block_num = options.block_num;
        config.rounds = options.rounds;
        config.put_count = options.put_count;
        config.get_count = options.get_count;
        config.lookup_exist_count = options.lookup_exist_count;
        config.lookup_miss_count = options.lookup_miss_count;
        config.timeout_ms = runtime_config.opTimeoutMs;
        config.ttl_ms = static_cast<std::uint32_t>(runtime_config.defaultDumpTtlMs);
        stores.push_back(std::make_unique<SimulatedStore>(std::move(config)));
    }

    for (const auto& store : stores) { store->Start(); }
    bool passed = true;
    for (const auto& store : stores) { passed = store->Join() && passed; }
    Print(passed ? "dramstore simulation passed" : "dramstore simulation failed");
    return passed ? 0 : 1;
}
