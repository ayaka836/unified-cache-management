/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the MIT License.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "acl/acl.h"
#include "adxl/adxl_types.h"
#include "hixl/hixl.h"

namespace {

constexpr std::chrono::milliseconds kStatusPollInterval{1};
constexpr std::chrono::milliseconds kStatePollInterval{10};
constexpr int32_t kSyncTransferTimeoutMs = 30000;

struct Options {
    std::string role;
    int32_t deviceId{-1};
    std::string localEngine;
    std::string remoteEngine;
    std::vector<std::string> remoteEngines;
    std::string localEid;
    std::string remoteEid;
    std::string backend{"adxl"};
    std::string transferMode{"async"};
    std::string stateDir;
    std::string operation{"write"};
    std::string bufferPool;
    size_t devicePort{0};
    size_t requestSize{4096};
    size_t requestCount{16};
    size_t batchSize{1};
    size_t inflight{0};
    size_t rounds{100};
};

const char* RecentError()
{
    const char* error = aclGetRecentErrMsg();
    return error == nullptr ? "no ACL error" : error;
}

bool ParsePositive(const char* text, size_t& result)
{
    try {
        size_t parsed = 0;
        const auto value = std::stoull(text, &parsed);
        if (parsed != std::strlen(text) || value == 0) { return false; }
        result = static_cast<size_t>(value);
        return static_cast<unsigned long long>(result) == value;
    } catch (...) {
        return false;
    }
}

bool ParseArgs(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) { return false; }
        const std::string key = argv[index];
        const char* value = argv[index + 1];
        if (key == "--role") {
            options.role = value;
        } else if (key == "--device") {
            try {
                options.deviceId = std::stoi(value);
            } catch (...) {
                return false;
            }
        } else if (key == "--local-engine") {
            options.localEngine = value;
        } else if (key == "--remote-engine") {
            options.remoteEngine = value;
        } else if (key == "--remote-engines") {
            std::string engines = value;
            size_t begin = 0;
            while (begin <= engines.size()) {
                const auto end = engines.find(',', begin);
                const auto engine = engines.substr(begin, end - begin);
                if (engine.empty()) { return false; }
                options.remoteEngines.push_back(engine);
                if (end == std::string::npos) { break; }
                begin = end + 1;
            }
        } else if (key == "--local-eid") {
            options.localEid = value;
        } else if (key == "--remote-eid") {
            options.remoteEid = value;
        } else if (key == "--backend") {
            options.backend = value;
        } else if (key == "--transfer-mode") {
            options.transferMode = value;
        } else if (key == "--state-dir") {
            options.stateDir = value;
        } else if (key == "--operation") {
            options.operation = value;
        } else if (key == "--buffer-pool") {
            options.bufferPool = value;
        } else if (key == "--device-port") {
            if (!ParsePositive(value, options.devicePort) || options.devicePort > 65535) {
                return false;
            }
        } else if (key == "--request-size") {
            if (!ParsePositive(value, options.requestSize)) { return false; }
        } else if (key == "--request-count") {
            if (!ParsePositive(value, options.requestCount)) { return false; }
        } else if (key == "--batch-size") {
            if (!ParsePositive(value, options.batchSize)) { return false; }
        } else if (key == "--inflight") {
            if (!ParsePositive(value, options.inflight)) { return false; }
        } else if (key == "--rounds") {
            if (!ParsePositive(value, options.rounds)) { return false; }
        } else {
            return false;
        }
    }

    const bool nativeValid = options.backend != "hixl-engine" ||
                             (!options.localEid.empty() && !options.remoteEid.empty());
    const bool commonValid = options.deviceId >= 0 && !options.localEngine.empty() &&
                             !options.stateDir.empty() && options.devicePort != 0 &&
                             (options.backend == "adxl" || options.backend == "hixl-engine") &&
                             (options.transferMode == "sync" || options.transferMode == "async") &&
                             nativeValid;
    if (options.role == "server") { return commonValid; }
    if (options.remoteEngines.empty() && !options.remoteEngine.empty()) {
        options.remoteEngines.push_back(options.remoteEngine);
    }
    return options.role == "client" && commonValid && !options.remoteEngines.empty() &&
           (options.operation == "read" || options.operation == "write" ||
            options.operation == "alternate");
}

void PrintUsage(const char* program)
{
    std::cerr << "Server: " << program
              << " --role server --device ID --local-engine HOST:PORT --state-dir DIR"
                 " --device-port PORT --local-eid EID --remote-eid EID"
                 " --request-size BYTES --request-count N\n"
              << "Client: " << program
              << " --role client --device ID --local-engine HOST:PORT"
                 " --remote-engines HOST:PORT[,HOST:PORT...]"
                 " --device-port PORT --local-eid EID --remote-eid EID --state-dir DIR"
                 " --request-size BYTES --request-count N --rounds N"
                 " --batch-size N [--inflight N] --operation read|write|alternate"
                 " [--buffer-pool NUM:SIZE_MB]\n";
}

bool Exists(const std::string& path)
{
    std::ifstream input(path);
    return input.good();
}

bool WriteFile(const std::string& path, uintptr_t value = 0)
{
    std::ofstream output(path, std::ios::trunc);
    output << value << '\n';
    return output.good();
}

bool ReadAddress(const std::string& path, uintptr_t& address)
{
    std::ifstream input(path);
    input >> address;
    return input.good() || input.eof();
}

class DeviceContext {
public:
    explicit DeviceContext(int32_t deviceId) : deviceId_(deviceId) {}

    bool Set()
    {
        std::cout << "[INFO] aclrtSetDevice begin device=" << deviceId_ << '\n';
        const auto status = aclrtSetDevice(deviceId_);
        if (status != ACL_ERROR_NONE) {
            std::cerr << "[ERROR] aclrtSetDevice(" << deviceId_ << ") failed: " << status << '\n';
            return false;
        }
        active_ = true;
        std::cout << "[INFO] aclrtSetDevice success device=" << deviceId_ << '\n';
        return true;
    }

    ~DeviceContext()
    {
        if (!active_) { return; }
        const auto status = aclrtResetDevice(deviceId_);
        if (status != ACL_ERROR_NONE) {
            std::cerr << "[ERROR] aclrtResetDevice(" << deviceId_ << ") failed: " << status << '\n';
        }
    }

private:
    int32_t deviceId_;
    bool active_{false};
};

class Buffer {
public:
    explicit Buffer(bool host) : host_(host) {}

    bool Allocate(size_t size)
    {
        size_ = size;
        const auto status = host_ ? aclrtMallocHost(&address_, size_)
                                  : aclrtMalloc(&address_, size_, ACL_MEM_MALLOC_HUGE_FIRST);
        if (status != ACL_ERROR_NONE) {
            std::cerr << "[ERROR] " << (host_ ? "aclrtMallocHost" : "aclrtMalloc") << '(' << size_
                      << ") failed: " << status << '\n';
            return false;
        }
        if (host_) {
            std::memset(address_, 0x5a, size_);
            return true;
        }
        std::vector<uint8_t> initial(size_, 0x5a);
        const auto copyStatus =
            aclrtMemcpy(address_, size_, initial.data(), initial.size(), ACL_MEMCPY_HOST_TO_DEVICE);
        if (copyStatus != ACL_ERROR_NONE) {
            std::cerr << "[ERROR] initialize device buffer failed: " << copyStatus << '\n';
            return false;
        }
        return true;
    }

    void Free()
    {
        if (address_ == nullptr) { return; }
        const auto status = host_ ? aclrtFreeHost(address_) : aclrtFree(address_);
        if (status != ACL_ERROR_NONE) {
            std::cerr << "[ERROR] " << (host_ ? "aclrtFreeHost" : "aclrtFree")
                      << " failed: " << status << '\n';
        }
        address_ = nullptr;
        size_ = 0;
    }

    ~Buffer() { Free(); }

    void* Address() const { return address_; }
    size_t Size() const { return size_; }
    hixl::MemType Type() const { return host_ ? hixl::MEM_HOST : hixl::MEM_DEVICE; }

private:
    bool host_;
    void* address_{nullptr};
    size_t size_{0};
};

class RegisteredMemory {
public:
    RegisteredMemory(hixl::Hixl& engine, Buffer& buffer) : engine_(engine), buffer_(buffer) {}

    bool Register()
    {
        hixl::MemDesc memory{reinterpret_cast<uintptr_t>(buffer_.Address()), buffer_.Size()};
        const auto status = engine_.RegisterMem(memory, buffer_.Type(), handle_);
        if (status != hixl::SUCCESS) {
            std::cerr << "[ERROR] RegisterMem failed: " << status << ", " << RecentError() << '\n';
            return false;
        }
        active_ = true;
        return true;
    }

    ~RegisteredMemory()
    {
        if (!active_) { return; }
        const auto status = engine_.DeregisterMem(handle_);
        if (status != hixl::SUCCESS) {
            std::cerr << "[ERROR] DeregisterMem failed: " << status << ", " << RecentError()
                      << '\n';
        }
    }

private:
    hixl::Hixl& engine_;
    Buffer& buffer_;
    hixl::MemHandle handle_{nullptr};
    bool active_{false};
};

bool InitializeEngine(hixl::Hixl& engine, const Options& options)
{
    std::map<hixl::AscendString, hixl::AscendString> initOptions;
    const auto globalResourceConfig = std::string{"{\"comm_resource_config.listen_port\":\""} +
                                      std::to_string(options.devicePort) + "\"}";
    initOptions[hixl::OPTION_GLOBAL_RESOURCE_CONFIG] = globalResourceConfig.c_str();
    std::string localCommRes;
    if (options.backend == "hixl-engine") {
        localCommRes = std::string{"{\"version\":\"1.3\",\"net_instance_id\":\"hixl-raw-"} +
                       options.role + '-' + std::to_string(options.deviceId) +
                       "\",\"endpoint_list\":[{\"protocol\":\"ub_ctp\",\"comm_id\":\"" +
                       options.localEid + "\",\"placement\":\"device\",\"dst_eid\":\"" +
                       options.remoteEid + "\"}]}";
        initOptions[adxl::OPTION_LOCAL_COMM_RES] = localCommRes.c_str();
    }
    if (!options.bufferPool.empty()) {
        initOptions[hixl::OPTION_BUFFER_POOL] = options.bufferPool.c_str();
    }
    std::cout << "[INFO] HIXL Initialize begin engine=" << options.localEngine
              << " backend=" << options.backend << " transfer_mode=" << options.transferMode
              << " local_comm_res=" << (localCommRes.empty() ? "<unset>" : localCommRes)
              << " global_resource_config=" << globalResourceConfig
              << " buffer_pool=" << (options.bufferPool.empty() ? "<unset>" : options.bufferPool)
              << '\n';
    const auto status = engine.Initialize(options.localEngine.c_str(), initOptions);
    if (status != hixl::SUCCESS) {
        std::cerr << "[ERROR] Initialize(" << options.localEngine << ") failed: " << status << ", "
                  << RecentError() << '\n';
        return false;
    }
    std::cout << "[INFO] HIXL Initialize success engine=" << options.localEngine
              << " backend=" << options.backend << '\n';
    return true;
}

struct PendingBatch {
    size_t index{0};
    size_t channel{0};
    hixl::TransferReq request{nullptr};
};

struct RemotePeer {
    std::string engine;
    uintptr_t address{0};
};

bool SubmitBatch(hixl::Hixl& engine, const Options& options, const std::vector<RemotePeer>& peers,
                 Buffer& local, bool read, size_t round, size_t batch, PendingBatch& pending)
{
    const size_t channel = batch % peers.size();
    const size_t channelBatch = batch / peers.size();
    const auto& peer = peers[channel];
    const size_t descriptorBegin = batch * options.batchSize;
    const size_t descriptorEnd =
        descriptorBegin + std::min(options.batchSize, options.requestCount - descriptorBegin);
    std::vector<hixl::TransferOpDesc> descriptors;
    descriptors.reserve(descriptorEnd - descriptorBegin);
    for (size_t index = descriptorBegin; index < descriptorEnd; ++index) {
        const size_t localOffset = index * options.requestSize;
        const size_t remoteOffset =
            (channelBatch * options.batchSize + index - descriptorBegin) * options.requestSize;
        descriptors.push_back({reinterpret_cast<uintptr_t>(local.Address()) + localOffset,
                               peer.address + remoteOffset, options.requestSize});
    }

    hixl::TransferReq request = nullptr;
    const auto status =
        options.transferMode == "sync"
            ? engine.TransferSync(peer.engine.c_str(), read ? hixl::READ : hixl::WRITE, descriptors,
                                  kSyncTransferTimeoutMs)
            : engine.TransferAsync(peer.engine.c_str(), read ? hixl::READ : hixl::WRITE,
                                   descriptors, {}, request);
    if (status != hixl::SUCCESS) {
        std::cerr << "[ERROR] round=" << round + 1 << " batch=" << batch << " channel=" << channel
                  << " remote=" << peer.engine << " descriptors=[" << descriptorBegin << ','
                  << descriptorEnd << ") Transfer"
                  << (options.transferMode == "sync" ? "Sync" : "Async") << " failed: " << status
                  << ", " << RecentError() << '\n';
        return false;
    }
    pending = PendingBatch{batch, channel, request};
    std::cout << "[SUBMITTED] round=" << round + 1 << " batch=" << batch << " channel=" << channel
              << " remote=" << peer.engine << " descriptors=[" << descriptorBegin << ','
              << descriptorEnd << ")\n";
    return true;
}

bool RunRound(hixl::Hixl& engine, const Options& options, const std::vector<RemotePeer>& peers,
              Buffer& local, bool read, size_t round)
{
    const size_t batchCount =
        options.requestCount / options.batchSize + (options.requestCount % options.batchSize != 0);
    const size_t window =
        options.inflight == 0 ? batchCount : std::min(options.inflight, batchCount);
    std::vector<PendingBatch> pending;
    pending.reserve(window);
    size_t nextBatch = 0;
    size_t succeeded = 0;
    size_t failed = 0;

    const auto refill = [&]() {
        while (pending.size() < window && nextBatch < batchCount) {
            PendingBatch batch;
            const size_t index = nextBatch++;
            if (SubmitBatch(engine, options, peers, local, read, round, index, batch)) {
                pending.push_back(batch);
            } else {
                ++failed;
            }
        }
    };

    refill();
    while (!pending.empty() || nextBatch < batchCount) {
        for (size_t slot = 0; slot < pending.size();) {
            if (options.transferMode == "sync") {
                ++succeeded;
                std::cout << "[DONE] round=" << round + 1 << " batch=" << pending[slot].index
                          << " channel=" << pending[slot].channel
                          << " mode=sync submitted=" << nextBatch << '/' << batchCount << '\n';
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(slot));
                continue;
            }
            hixl::TransferStatus transferStatus = hixl::TransferStatus::WAITING;
            const auto status = engine.GetTransferStatus(pending[slot].request, transferStatus);
            if (status != hixl::SUCCESS) {
                std::cerr << "[ERROR] round=" << round + 1 << " batch=" << pending[slot].index
                          << " channel=" << pending[slot].channel
                          << " GetTransferStatus failed: " << status << ", " << RecentError()
                          << '\n';
                ++failed;
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(slot));
            } else if (transferStatus == hixl::TransferStatus::FAILED ||
                       transferStatus == hixl::TransferStatus::TIMEOUT) {
                std::cerr << "[ERROR] round=" << round + 1 << " batch=" << pending[slot].index
                          << " channel=" << pending[slot].channel
                          << " transfer status=" << static_cast<int>(transferStatus) << '\n';
                ++failed;
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(slot));
            } else if (transferStatus == hixl::TransferStatus::COMPLETED) {
                ++succeeded;
                std::cout << "[DONE] round=" << round + 1 << " batch=" << pending[slot].index
                          << " channel=" << pending[slot].channel
                          << " active=" << pending.size() - 1 << " submitted=" << nextBatch << '/'
                          << batchCount << '\n';
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(slot));
            } else {
                ++slot;
            }
        }
        refill();
        if (!pending.empty()) { std::this_thread::sleep_for(kStatusPollInterval); }
    }

    std::cout << "[INFO] round=" << round + 1 << " terminal batches: succeeded=" << succeeded
              << " failed=" << failed << " total=" << batchCount << '\n';
    return failed == 0;
}

bool RunRounds(hixl::Hixl& engine, const Options& options, const std::vector<RemotePeer>& peers,
               Buffer& local)
{
    bool allSucceeded = true;
    for (size_t round = 0; round < options.rounds; ++round) {
        const bool read =
            options.operation == "read" || (options.operation == "alternate" && round % 2 != 0);
        const size_t batchCount = options.requestCount / options.batchSize +
                                  (options.requestCount % options.batchSize != 0);
        const size_t window =
            options.inflight == 0 ? batchCount : std::min(options.inflight, batchCount);
        std::cout << "[INFO] round=" << round + 1 << '/' << options.rounds
                  << " submit begin operation=" << (read ? "read" : "write")
                  << " requests=" << options.requestCount << " bytes=" << options.requestSize
                  << " descriptors_per_batch=" << options.batchSize << " batch_count=" << batchCount
                  << " channels=" << peers.size() << " inflight=" << window << '\n';
        const bool roundSucceeded = RunRound(engine, options, peers, local, read, round);
        allSucceeded = allSucceeded && roundSucceeded;
        std::cout << (roundSucceeded ? "[PASS]" : "[FAILED]") << " round=" << round + 1 << '/'
                  << options.rounds << " operation=" << (read ? "read" : "write")
                  << " requests=" << options.requestCount << " bytes=" << options.requestSize
                  << " batch_size=" << options.batchSize << " batch_count=" << batchCount
                  << " barrier=complete\n";
    }
    return allSucceeded;
}

int RunServer(const Options& options)
{
    std::cout << "[INFO] server process start device=" << options.deviceId
              << " engine=" << options.localEngine << '\n';
    DeviceContext context(options.deviceId);
    if (!context.Set()) { return EXIT_FAILURE; }
    hixl::Hixl engine;
    if (!InitializeEngine(engine, options)) { return EXIT_FAILURE; }
    Buffer buffer(false);
    std::cout << "[INFO] server allocate device memory bytes="
              << options.requestSize * options.requestCount << '\n';
    if (!buffer.Allocate(options.requestSize * options.requestCount)) {
        engine.Finalize();
        return EXIT_FAILURE;
    }
    bool failed = false;
    {
        RegisteredMemory memory(engine, buffer);
        std::cout << "[INFO] server RegisterMem begin\n";
        if (!memory.Register()) {
            failed = true;
        } else {
            std::cout << "[INFO] server RegisterMem success\n";
            const auto addressPath = options.stateDir + "/server.addr";
            failed = !WriteFile(addressPath, reinterpret_cast<uintptr_t>(buffer.Address()));
            if (!failed) {
                std::cout << "[INFO] server ready device=" << options.deviceId
                          << " engine=" << options.localEngine << " address=" << buffer.Address()
                          << " bytes=" << buffer.Size() << '\n';
                std::cout << "[INFO] server waiting for client completion\n";
                while (!Exists(options.stateDir + "/client.done") &&
                       !Exists(options.stateDir + "/client.failed")) {
                    std::this_thread::sleep_for(kStatePollInterval);
                }
            }
        }
    }
    buffer.Free();
    engine.Finalize();
    std::cout << "[INFO] server stopped result="
              << (failed || Exists(options.stateDir + "/client.failed") ? "failed" : "success")
              << '\n';
    return failed || Exists(options.stateDir + "/client.failed") ? EXIT_FAILURE : EXIT_SUCCESS;
}

int RunClient(const Options& options)
{
    std::cout << "[INFO] client process start device=" << options.deviceId
              << " local=" << options.localEngine << " channels=" << options.remoteEngines.size()
              << '\n';
    DeviceContext context(options.deviceId);
    if (!context.Set()) { return EXIT_FAILURE; }
    hixl::Hixl engine;
    if (!InitializeEngine(engine, options)) { return EXIT_FAILURE; }
    Buffer buffer(true);
    std::cout << "[INFO] client allocate pinned host memory bytes="
              << options.requestSize * options.requestCount << '\n';
    if (!buffer.Allocate(options.requestSize * options.requestCount)) {
        engine.Finalize();
        return EXIT_FAILURE;
    }
    bool success = false;
    {
        const bool useBufferStaging = options.backend == "adxl" && options.transferMode == "sync" &&
                                      !options.bufferPool.empty() && options.bufferPool != "0:0";
        std::unique_ptr<RegisteredMemory> memory;
        std::vector<RemotePeer> peers;
        bool memoryReady = true;
        if (useBufferStaging) {
            std::cout << "[INFO] client skips Host RegisterMem so ADXL BufferPool stages H2D/D2H\n";
        } else {
            memory = std::make_unique<RegisteredMemory>(engine, buffer);
            std::cout << "[INFO] client RegisterMem begin\n";
            memoryReady = memory->Register();
        }
        if (memoryReady) {
            if (!useBufferStaging) {
                std::cout << "[INFO] client RegisterMem success, waiting for server address\n";
            } else {
                std::cout << "[INFO] client waiting for server address\n";
            }
            for (size_t channel = 0; channel < options.remoteEngines.size(); ++channel) {
                uintptr_t remoteAddress = 0;
                const auto addressPath =
                    options.stateDir + "/channel-" + std::to_string(channel) + "/server.addr";
                while (!ReadAddress(addressPath, remoteAddress)) {
                    std::this_thread::sleep_for(kStatePollInterval);
                }
                peers.push_back({options.remoteEngines[channel], remoteAddress});
                std::cout << "[INFO] client received channel=" << channel
                          << " remote=" << peers.back().engine
                          << " address=" << reinterpret_cast<void*>(remoteAddress) << '\n';
            }
            size_t connected = 0;
            for (; connected < peers.size(); ++connected) {
                std::cout << "[INFO] client Connect begin channel=" << connected
                          << " remote=" << peers[connected].engine << '\n';
                const auto status = engine.Connect(peers[connected].engine.c_str());
                if (status != hixl::SUCCESS) {
                    std::cerr << "[ERROR] Connect failed channel=" << connected << ": " << status
                              << ", " << RecentError() << '\n';
                    break;
                }
                std::cout << "[INFO] client connected channel=" << connected
                          << " remote=" << peers[connected].engine << '\n';
            }
            if (connected == peers.size()) { success = RunRounds(engine, options, peers, buffer); }
            while (connected != 0) {
                --connected;
                std::cout << "[INFO] client Disconnect begin channel=" << connected
                          << " remote=" << peers[connected].engine << '\n';
                const auto status = engine.Disconnect(peers[connected].engine.c_str());
                if (status != hixl::SUCCESS) {
                    std::cerr << "[ERROR] Disconnect failed channel=" << connected << ": " << status
                              << ", " << RecentError() << '\n';
                    success = false;
                }
            }
        }
    }
    for (size_t channel = 0; channel < options.remoteEngines.size(); ++channel) {
        WriteFile(options.stateDir + "/channel-" + std::to_string(channel) +
                  (success ? "/client.done" : "/client.failed"));
    }
    buffer.Free();
    engine.Finalize();
    std::cout << "[INFO] client stopped result=" << (success ? "success" : "failed") << '\n';
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (options.requestCount > SIZE_MAX / options.requestSize) {
        std::cerr << "[ERROR] request-size * request-count overflows size_t\n";
        return EXIT_FAILURE;
    }
    return options.role == "server" ? RunServer(options) : RunClient(options);
}
