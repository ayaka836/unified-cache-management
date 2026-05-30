/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <fmt/ranges.h>
#include "device_staging_pool.h"
#include "flag_buffer_ring.h"
#include "hash_ring.h"
#include "host_dram_pool.h"
#include "logger/logger.h"
#include "protocol.h"
#include "trans/device.h"
#include "ucmstore_v1.h"
#include "xfer_transport.h"

namespace UC::RemoteMemStore {

namespace {

struct ShardKey {
    Detail::BlockId block{};
    size_t shard{0};
    bool operator==(const ShardKey& other) const noexcept
    {
        return block == other.block && shard == other.shard;
    }
};

struct ShardKeyHasher {
    size_t operator()(const ShardKey& key) const noexcept
    {
        static Detail::BlockIdHasher blockHasher;
        static std::hash<size_t> shardHasher;
        constexpr auto goldenSection = 0x9e3779b97f4a7c15ULL;
        const auto h1 = blockHasher(key.block);
        const auto h2 = shardHasher(key.shard);
        return h1 ^ (h2 + goldenSection + (h1 << 6) + (h1 >> 2));
    }
};

struct ShardEntry {
    size_t slot{0};
    size_t bytes{0};
    bool ready{false};
};

enum class TaskType : uint8_t { LOAD, DUMP };

std::vector<size_t> ExpandTensorSizes(const Config& config, size_t nAddrs)
{
    if (config.tensorSizes.size() == 1 && nAddrs > 1) {
        return std::vector<size_t>(nAddrs, config.tensorSizes[0]);
    }
    return config.tensorSizes;
}

}  // namespace

class RemoteMemStore : public StoreV1 {
public:
    Status Setup(const Detail::Dictionary& inConfig) override
    {
        // read config params
        config_ = ParseConfig(inConfig);
        auto s = CheckConfig(config_);
        if (s.Failure()) {
            UC_ERROR("Failed to check remote mem config: {}.", s);
            return s;
        }


        // init hash ring
        s = ring_.Setup(config_);
        if (s.Failure()) { return s; }
        transEnable_ = config_.addressMode == AddressMode::HOST || config_.deviceId >= 0;
        if (transEnable_) {

            // init host DRAM pool, flag buffer ring and transfer transport
            s = pool_.Setup(config_);
            if (s.Failure()) { return s; }
            s = flags_.Setup(config_);
            if (s.Failure()) { return s; }
            s = transport_.Setup(config_);
            if (s.Failure()) { return s; }

            if (config_.addressMode == AddressMode::DEVICE) {
                Trans::Device device;
                s = device.Setup(config_.deviceId);
                if (s.Failure()) { return s; }
                stream_ = device.MakeStream();
                if (!stream_) { return Status::OutOfMemory(); }
                s = staging_.Setup(config_);
                if (s.Failure()) { return s; }
            }
        }

        ShowConfig(config_);
        return Status::OK();
    }

    std::string Readme() const override { return "RemoteMemStore"; }

    Expected<std::vector<uint8_t>> Lookup(const Detail::BlockId* blocks, size_t num) override
    {
        std::vector<uint8_t> founds(num, false);
        {
            std::lock_guard<std::mutex> lock{metaMutex_};
            for (size_t i = 0; i < num; ++i) {
                const auto iter = meta_.find(ShardKey{blocks[i], 0});
                founds[i] = iter != meta_.end() && iter->second.ready;
            }
        }

        if (!config_.storeBackend) { return founds; }
        std::vector<Detail::BlockId> missed;
        std::vector<size_t> missedIndexes;
        for (size_t i = 0; i < num; ++i) {
            if (founds[i]) { continue; }
            missed.push_back(blocks[i]);
            missedIndexes.push_back(i);
        }
        if (missed.empty()) { return founds; }
        auto backendRes = config_.storeBackend->Lookup(missed.data(), missed.size());
        if (!backendRes) { return backendRes.Error(); }
        const auto& backendFounds = backendRes.Value();
        for (size_t i = 0; i < missedIndexes.size(); ++i) {
            founds[missedIndexes[i]] = backendFounds[i];
        }
        return founds;
    }

    Expected<ssize_t> LookupOnPrefix(const Detail::BlockId* blocks, size_t num) override
    {
        auto res = Lookup(blocks, num);
        if (!res) { return res.Error(); }
        const auto& founds = res.Value();
        ssize_t index = -1;
        for (size_t i = 0; i < founds.size() && founds[i]; ++i) {
            index = static_cast<ssize_t>(i);
        }
        return index;
    }

    void Prefetch(const Detail::BlockId* blocks, size_t num) override
    {
        if (config_.storeBackend) { config_.storeBackend->Prefetch(blocks, num); }
    }

    Expected<Detail::TaskHandle> Load(Detail::TaskDesc task) override
    {
        return Submit(TaskType::LOAD, std::move(task));
    }

    Expected<Detail::TaskHandle> Dump(Detail::TaskDesc task) override
    {
        return Submit(TaskType::DUMP, std::move(task));
    }

    Expected<bool> Check(Detail::TaskHandle taskId) override
    {
        std::shared_future<Status> future;
        {
            std::lock_guard<std::mutex> lock{taskMutex_};
            auto iter = tasks_.find(taskId);
            if (iter == tasks_.end()) { return Status::NotFound(); }
            future = iter->second;
        }
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    Status Wait(Detail::TaskHandle taskId) override
    {
        std::shared_future<Status> future;
        {
            std::lock_guard<std::mutex> lock{taskMutex_};
            auto iter = tasks_.find(taskId);
            if (iter == tasks_.end()) { return Status::NotFound(); }
            future = iter->second;
        }
        if (config_.timeoutMs != 0) {
            const auto status = future.wait_for(std::chrono::milliseconds(config_.timeoutMs));
            if (status != std::future_status::ready) { return Status::Timeout(); }
        } else {
            future.wait();
        }
        auto s = future.get();
        {
            std::lock_guard<std::mutex> lock{taskMutex_};
            tasks_.erase(taskId);
        }
        if (s.Failure()) { UC_ERROR("RemoteMem task({}) failed: {}.", taskId, s); }
        return s;
    }

private:
    Config ParseConfig(const Detail::Dictionary& inConfig)
    {
        Config config;
        inConfig.Get("store_backend", config.storeBackend);
        inConfig.Get("remote_mem_local_node", config.localNode);
        inConfig.Get("remote_mem_nodes", config.nodes);
        inConfig.GetNumber("remote_mem_local_rank", config.localRank);
        inConfig.GetNumber("remote_mem_pool_rank", config.poolRank);
        inConfig.GetNumber("device_id", config.deviceId);

        size_t tensorSize = 0;
        inConfig.GetNumber("tensor_size", tensorSize);
        inConfig.GetNumber("shard_size", config.shardSize);
        if (tensorSize != 0 && config.shardSize != 0) {
            config.tensorSizes.assign(config.shardSize / tensorSize, tensorSize);
        } else {
            inConfig.GetNumbers("tensor_size_list", config.tensorSizes);
        }

        inConfig.GetNumber("block_size", config.blockSize);
        size_t poolCapacityGb = 0;
        size_t poolCapacityBytes = 0;
        inConfig.GetNumber("remote_mem_pool_capacity_gb", poolCapacityGb);
        inConfig.GetNumber("remote_mem_pool_capacity_bytes", poolCapacityBytes);
        if (poolCapacityGb != 0) { config.dramPoolCapacity = poolCapacityGb << 30; }
        if (poolCapacityBytes != 0) { config.dramPoolCapacity = poolCapacityBytes; }

        size_t flagCapacityBytes = 0;
        inConfig.GetNumber("remote_mem_flag_buffer_capacity_bytes", flagCapacityBytes);
        if (flagCapacityBytes != 0) { config.flagBufferCapacity = flagCapacityBytes; }
        inConfig.GetNumber("remote_mem_flag_bytes", config.flagBytes);
        inConfig.GetNumber("timeout_ms", config.timeoutMs);
        inConfig.GetNumber("remote_mem_device_staging_slots", config.deviceStagingSlots);
        inConfig.Get("remote_mem_backend_fallback", config.backendFallback);
        inConfig.Get("remote_mem_backend_write_through", config.backendWriteThrough);
        inConfig.Get("remote_mem_register_host_buffer", config.registerHostBuffer);
        inConfig.Get("remote_mem_device_staging_enable", config.deviceStagingEnable);
        inConfig.Get("remote_mem_xfer_transport", config.xferTransport);

        std::string addressMode{"device"};
        inConfig.Get("remote_mem_addr_type", addressMode);
        if (addressMode == "host") { config.addressMode = AddressMode::HOST; }
        return config;
    }

    Status CheckConfig(const Config& config)
    {
        if (config.deviceId < -1) {
            return Status::InvalidParam("invalid remote mem device({})", config.deviceId);
        }
        if (config.deviceId == -1 && config.addressMode == AddressMode::DEVICE) {
            return Status::OK();
        }
        if (config.tensorSizes.empty()) {
            return Status::InvalidParam("invalid remote mem tensor size");
        }
        if (config.shardSize == 0 || config.blockSize == 0) {
            return Status::InvalidParam("invalid remote mem size({},{})", config.shardSize,
                                        config.blockSize);
        }
        const auto totalTensorSize =
            std::accumulate(config.tensorSizes.begin(), config.tensorSizes.end(), size_t(0));
        if (totalTensorSize != config.shardSize) {
            return Status::InvalidParam("invalid remote mem shard size({})", config.shardSize);
        }
        if (config.blockSize % config.shardSize != 0) {
            return Status::InvalidParam("invalid remote mem block size({})", config.blockSize);
        }
        if (config.flagBytes < sizeof(uint64_t)) {
            return Status::InvalidParam("invalid remote mem flag bytes({})", config.flagBytes);
        }
        if (config.deviceStagingSlots == 0) {
            return Status::InvalidParam("invalid remote mem staging slots");
        }
        if (config.addressMode == AddressMode::DEVICE && config.deviceId < 0) {
            return Status::InvalidParam("device address mode requires a valid device id");
        }
        return Status::OK();
    }

    void ShowConfig(const Config& config)
    {
        constexpr const char* ns = "RemoteMemStore";
        std::string buildType = UCM_BUILD_TYPE;
        if (buildType.empty()) { buildType = "Release"; }
        UC_INFO("{}-{}({}).", ns, UCM_COMMIT_ID, buildType);
        UC_INFO("Set {}::LocalNode to {}.", ns, config.localNode);
        UC_INFO("Set {}::Nodes to {}.", ns, config.nodes);
        UC_INFO("Set {}::LocalRank to {}.", ns, config.localRank);
        UC_INFO("Set {}::PoolRank to {}.", ns, config.poolRank);
        UC_INFO("Set {}::DeviceId to {}.", ns, config.deviceId);
        UC_INFO("Set {}::TensorSizes to {}.", ns, config.tensorSizes);
        UC_INFO("Set {}::ShardSize to {}.", ns, config.shardSize);
        UC_INFO("Set {}::BlockSize to {}.", ns, config.blockSize);
        UC_INFO("Set {}::DramPoolCapacity to {}.", ns, config.dramPoolCapacity);
        UC_INFO("Set {}::FlagBufferCapacity to {}.", ns, config.flagBufferCapacity);
        UC_INFO("Set {}::FlagBytes to {}.", ns, config.flagBytes);
        UC_INFO("Set {}::DeviceStagingEnable to {}.", ns, config.deviceStagingEnable);
        UC_INFO("Set {}::DeviceStagingSlots to {}.", ns, config.deviceStagingSlots);
        UC_INFO("Set {}::XferTransport to {}.", ns, config.xferTransport);
        UC_INFO("Set {}::AddressMode to {}.", ns,
                config.addressMode == AddressMode::DEVICE ? "device" : "host");
        UC_INFO("Set {}::BackendFallback to {}.", ns, config.backendFallback);
        UC_INFO("Set {}::BackendWriteThrough to {}.", ns, config.backendWriteThrough);
        if (config.storeBackend) {
            UC_INFO("Set {}::StoreBackend to {}.", ns, config.storeBackend->Readme());
        }
    }

    Expected<Detail::TaskHandle> Submit(TaskType type, Detail::TaskDesc task)
    {
        if (!transEnable_) { return Status::Error("transfer is not enable"); }
        const auto id = NextTaskId();
        auto future =
            std::async(std::launch::async, [this, type, task = std::move(task)]() mutable {
                return RunTask(type, task);
            })
                .share();
        {
            std::lock_guard<std::mutex> lock{taskMutex_};
            const auto inserted = tasks_.emplace(id, std::move(future)).second;
            if (!inserted) { return Status::DuplicateKey(); }
        }
        return id;
    }

    Status RunTask(TaskType type, Detail::TaskDesc& task)
    {
        return type == TaskType::DUMP ? RunDump(task) : RunLoad(task);
    }

    Status RunDump(Detail::TaskDesc& task)
    {
        for (auto& shard : task) {
            auto s = DumpOneShard(shard);
            if (s.Failure()) { return s; }
        }
        if (config_.backendWriteThrough && config_.storeBackend) {
            return DumpBackendFromPool(task);
        }
        return Status::OK();
    }

    Status RunLoad(Detail::TaskDesc& task)
    {
        for (auto& shard : task) {
            auto s = LoadOneShard(shard);
            if (s.Failure()) { return s; }
        }
        return Status::OK();
    }

    Status DumpOneShard(const Detail::Shard& shard)
    {
        auto tensorSizes = ExpandTensorSizes(config_, shard.addrs.size());
        auto s = CheckShardLayout(shard, tensorSizes);
        if (s.Failure()) { return s; }

        const auto key = ShardKey{shard.owner, shard.index};
        size_t slot = 0;
        bool inserted = false;
        bool oldReady = false;
        {
            std::lock_guard<std::mutex> lock{metaMutex_};
            auto iter = meta_.find(key);
            if (iter == meta_.end()) {
                auto slotRes = pool_.Allocate();
                if (!slotRes) { return slotRes.Error(); }
                slot = slotRes.Value();
                inserted = true;
                meta_.emplace(key, ShardEntry{slot, config_.shardSize, false});
            } else {
                slot = iter->second.slot;
                oldReady = iter->second.ready;
                iter->second.ready = false;
            }
        }

        auto rollbackBeforeWrite = [&] {
            std::lock_guard<std::mutex> lock{metaMutex_};
            if (inserted) {
                meta_.erase(key);
                pool_.Release(slot);
                return;
            }
            auto iter = meta_.find(key);
            if (iter != meta_.end()) { iter->second.ready = oldReady; }
        };

        s = WithFlag([&](const FlagBufferRing::Segment& flag) {
            const auto targetNode = ring_.Locate(shard.owner);
            auto message = MakeXferMessage(
                MakeRequest(RemoteOp::PUT, shard, tensorSizes, pool_.Describe(slot), flag,
                            ring_.LocalNode(), targetNode, config_.localRank,
                            config_.poolRank),
                NextXferId());
            auto status = transport_.SendControl(message);
            if (status.Failure()) {
                rollbackBeforeWrite();
                return status;
            }
            status = PutToPool(message, shard, tensorSizes, pool_.Data(slot));
            if (status.Failure()) { return status; }
            status = transport_.Notify(message.completionFlag, 1);
            if (status.Failure()) { return status; }
            return WaitFlag(flag);
        });
        if (s.Failure()) {
            if (inserted) {
                std::lock_guard<std::mutex> lock{metaMutex_};
                meta_.erase(key);
                pool_.Release(slot);
            }
            return s;
        }

        {
            std::lock_guard<std::mutex> lock{metaMutex_};
            auto iter = meta_.find(key);
            if (iter != meta_.end()) { iter->second.ready = true; }
        }
        return Status::OK();
    }

    Status LoadOneShard(const Detail::Shard& shard)
    {
        auto tensorSizes = ExpandTensorSizes(config_, shard.addrs.size());
        auto s = CheckShardLayout(shard, tensorSizes);
        if (s.Failure()) { return s; }

        ShardEntry entry;
        {
            std::lock_guard<std::mutex> lock{metaMutex_};
            auto iter = meta_.find(ShardKey{shard.owner, shard.index});
            if (iter != meta_.end() && iter->second.ready) { entry = iter->second; }
        }
        if (!entry.ready) {
            if (!config_.storeBackend || !config_.backendFallback) { return Status::NotFound(); }
            s = LoadBackendToPool(shard, entry);
            if (s.Failure()) { return s; }
        }

        return WithFlag([&](const FlagBufferRing::Segment& flag) {
            const auto sourceNode = ring_.Locate(shard.owner);
            auto message = MakeXferMessage(
                MakeRequest(RemoteOp::GET, shard, tensorSizes, pool_.Describe(entry.slot), flag,
                            sourceNode, ring_.LocalNode(), config_.poolRank,
                            config_.localRank),
                NextXferId());
            auto status = transport_.SendControl(message);
            if (status.Failure()) { return status; }
            status = GetFromPool(message, pool_.Data(entry.slot), shard, tensorSizes);
            if (status.Failure()) { return status; }
            status = transport_.Notify(message.completionFlag, 1);
            if (status.Failure()) { return status; }
            return WaitFlag(flag);
        });
    }

    RemoteRequest MakeRequest(RemoteOp op, const Detail::Shard& shard,
                              const std::vector<size_t>& tensorSizes,
                              const BufferDesc& poolBuffer,
                              const FlagBufferRing::Segment& flag,
                              const std::string& sourceNode, const std::string& targetNode,
                              uint64_t sourceRank, uint64_t targetRank)
    {
        RemoteRequest request;
        request.op = op;
        request.sourceNode = sourceNode;
        request.targetNode = targetNode;
        request.sourceRank = sourceRank;
        request.targetRank = targetRank;
        request.shard =
            ShardLayoutDesc{shard.owner, shard.index, config_.shardSize, tensorSizes};
        request.poolBuffer = poolBuffer;
        request.completionFlag = flags_.Describe(flag);
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            request.buffers.push_back(
                BufferDesc{reinterpret_cast<uint64_t>(shard.addrs[i]), tensorSizes[i], 0});
        }
        return request;
    }

    Status WithFlag(const std::function<Status(const FlagBufferRing::Segment&)>& fn)
    {
        auto flagRes = flags_.Allocate();
        if (!flagRes) { return flagRes.Error(); }
        const auto flag = flagRes.Value();
        auto s = fn(flag);
        flags_.Release(flag);
        return s;
    }

    Status WaitFlag(const FlagBufferRing::Segment& flag) const
    {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (flags_.Complete(flag)) { return Status::OK(); }
            if (config_.timeoutMs != 0) {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (static_cast<size_t>(elapsedMs) >= config_.timeoutMs) {
                    return Status::Timeout();
                }
            }
            std::this_thread::yield();
        }
    }

    Status CheckShardLayout(const Detail::Shard& shard,
                            const std::vector<size_t>& tensorSizes) const
    {
        if (shard.addrs.size() != tensorSizes.size()) {
            return Status::InvalidParam("invalid remote mem addr/tensor dims({},{})",
                                        shard.addrs.size(), tensorSizes.size());
        }
        const auto total = std::accumulate(tensorSizes.begin(), tensorSizes.end(), size_t(0));
        if (total != config_.shardSize) {
            return Status::InvalidParam("invalid remote mem tensor total({})", total);
        }
        if (config_.blockSize != 0 && shard.index >= ShardsPerBlock()) {
            return Status::InvalidParam("invalid remote mem shard index({})", shard.index);
        }
        return Status::OK();
    }

    Status DumpBackendFromPool(const Detail::TaskDesc& task)
    {
        Detail::TaskDesc backendTask;
        backendTask.brief = "RemoteMemWriteThrough";
        backendTask.reserve(task.size());
        {
            std::lock_guard<std::mutex> lock{metaMutex_};
            for (const auto& shard : task) {
                auto iter = meta_.find(ShardKey{shard.owner, shard.index});
                if (iter == meta_.end() || !iter->second.ready) { return Status::NotFound(); }
                backendTask.push_back(
                    Detail::Shard{shard.owner, shard.index, {pool_.Data(iter->second.slot)}});
            }
        }
        auto res = config_.storeBackend->Dump(std::move(backendTask));
        if (!res) { return res.Error(); }
        return config_.storeBackend->Wait(res.Value());
    }

    Status LoadBackendToPool(const Detail::Shard& shard, ShardEntry& entry)
    {
        const auto key = ShardKey{shard.owner, shard.index};
        {
            std::lock_guard<std::mutex> lock{metaMutex_};
            auto iter = meta_.find(key);
            if (iter != meta_.end() && iter->second.ready) {
                entry = iter->second;
                return Status::OK();
            }
        }

        auto slotRes = pool_.Allocate();
        if (!slotRes) { return slotRes.Error(); }
        const auto slot = slotRes.Value();

        Detail::TaskDesc backendTask;
        backendTask.brief = "RemoteMemFallbackLoad";
        backendTask.push_back(Detail::Shard{shard.owner, shard.index, {pool_.Data(slot)}});
        auto res = config_.storeBackend->Load(std::move(backendTask));
        if (!res) {
            pool_.Release(slot);
            return res.Error();
        }
        auto s = config_.storeBackend->Wait(res.Value());
        if (s.Failure()) {
            pool_.Release(slot);
            return s;
        }

        std::lock_guard<std::mutex> lock{metaMutex_};
        auto iter = meta_.find(key);
        if (iter != meta_.end() && iter->second.ready) {
            pool_.Release(slot);
            entry = iter->second;
            return Status::OK();
        }
        entry = ShardEntry{slot, config_.shardSize, true};
        meta_[key] = entry;
        return Status::OK();
    }

    Status PutToPool(const XferMessage& message, const Detail::Shard& shard,
                     const std::vector<size_t>& tensorSizes, void* host)
    {
        if (config_.addressMode == AddressMode::HOST) {
            return CopyHostToPool(message, shard, tensorSizes, host);
        }

        if (staging_.Enabled()) {
            auto stagingRes = staging_.Allocate();
            if (!stagingRes) { return stagingRes.Error(); }
            const auto staging = stagingRes.Value();
            auto s = PackToDeviceStaging(shard, tensorSizes, staging.addr);
            if (s.Success()) {
                s = transport_.RemoteDeviceToHost(message, staging.addr, host, config_.shardSize,
                                                  stream_.get());
            }
            staging_.Release(staging);
            return s;
        }

        std::lock_guard<std::mutex> lock{copyMutex_};
        size_t offset = 0;
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            auto s = stream_->DeviceToHostAsync(
                shard.addrs[i], static_cast<std::byte*>(host) + offset, tensorSizes[i]);
            if (s.Failure()) { return s; }
            offset += tensorSizes[i];
        }
        return stream_->Synchronized();
    }

    Status GetFromPool(const XferMessage& message, void* host, const Detail::Shard& shard,
                       const std::vector<size_t>& tensorSizes)
    {
        if (config_.addressMode == AddressMode::HOST) {
            return CopyPoolToHost(message, host, shard, tensorSizes);
        }

        if (staging_.Enabled()) {
            auto stagingRes = staging_.Allocate();
            if (!stagingRes) { return stagingRes.Error(); }
            const auto staging = stagingRes.Value();
            auto s = transport_.HostToRemoteDevice(message, host, staging.addr, config_.shardSize,
                                                   stream_.get());
            if (s.Success()) { s = UnpackFromDeviceStaging(staging.addr, shard, tensorSizes); }
            staging_.Release(staging);
            return s;
        }

        std::lock_guard<std::mutex> lock{copyMutex_};
        size_t offset = 0;
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            auto s = stream_->HostToDeviceAsync(
                static_cast<std::byte*>(host) + offset, shard.addrs[i], tensorSizes[i]);
            if (s.Failure()) { return s; }
            offset += tensorSizes[i];
        }
        return stream_->Synchronized();
    }

    Status CopyHostToPool(const XferMessage& message, const Detail::Shard& shard,
                          const std::vector<size_t>& tensorSizes, void* host)
    {
        std::vector<std::byte> packed(config_.shardSize);
        size_t offset = 0;
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            std::memcpy(packed.data() + offset, shard.addrs[i], tensorSizes[i]);
            offset += tensorSizes[i];
        }
        return transport_.HostToHost(message, packed.data(), host, config_.shardSize);
    }

    Status CopyPoolToHost(const XferMessage& message, void* host, const Detail::Shard& shard,
                          const std::vector<size_t>& tensorSizes)
    {
        std::vector<std::byte> packed(config_.shardSize);
        auto s = transport_.HostToHost(message, host, packed.data(), config_.shardSize);
        if (s.Failure()) { return s; }
        size_t offset = 0;
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            std::memcpy(shard.addrs[i], packed.data() + offset, tensorSizes[i]);
            offset += tensorSizes[i];
        }
        return Status::OK();
    }

    Status PackToDeviceStaging(const Detail::Shard& shard,
                               const std::vector<size_t>& tensorSizes, void* staging)
    {
        std::lock_guard<std::mutex> lock{copyMutex_};
        size_t offset = 0;
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            auto s = stream_->DeviceToDeviceAsync(
                shard.addrs[i], static_cast<std::byte*>(staging) + offset, tensorSizes[i]);
            if (s.Failure()) { return s; }
            offset += tensorSizes[i];
        }
        return stream_->Synchronized();
    }

    Status UnpackFromDeviceStaging(void* staging, const Detail::Shard& shard,
                                   const std::vector<size_t>& tensorSizes)
    {
        std::lock_guard<std::mutex> lock{copyMutex_};
        size_t offset = 0;
        for (size_t i = 0; i < shard.addrs.size(); ++i) {
            auto s = stream_->DeviceToDeviceAsync(
                static_cast<std::byte*>(staging) + offset, shard.addrs[i], tensorSizes[i]);
            if (s.Failure()) { return s; }
            offset += tensorSizes[i];
        }
        return stream_->Synchronized();
    }

    size_t ShardsPerBlock() const noexcept
    {
        if (config_.blockSize == 0 || config_.shardSize == 0) { return 1; }
        return config_.blockSize / config_.shardSize;
    }

    Detail::TaskHandle NextTaskId() noexcept
    {
        return taskId_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t NextXferId() noexcept { return xferId_.fetch_add(1, std::memory_order_relaxed); }

private:
    Config config_;
    HashRing ring_;
    HostDramPool pool_;
    FlagBufferRing flags_;
    DeviceStagingPool staging_;
    XferTransport transport_;
    std::unique_ptr<Trans::Stream> stream_{nullptr};
    std::mutex copyMutex_;
    std::mutex metaMutex_;
    std::unordered_map<ShardKey, ShardEntry, ShardKeyHasher> meta_;
    std::atomic<Detail::TaskHandle> taskId_{1};
    std::atomic<uint64_t> xferId_{1};
    bool transEnable_{false};
    std::mutex taskMutex_;
    std::unordered_map<Detail::TaskHandle, std::shared_future<Status>> tasks_;
};

}  // namespace UC::RemoteMemStore

extern "C" UC::StoreV1* MakeRemoteMemStore() { return new UC::RemoteMemStore::RemoteMemStore(); }
