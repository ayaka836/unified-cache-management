/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include <array>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "core/memory_region_manager.h"

namespace {

constexpr auto kProtocol = transport::TransportProtocol::Hixl;

transport::MemoryRegion HostRegion(void* addr, uint64_t length)
{
    return transport::MemoryRegion{addr, length, transport::MemoryType::Host, -1};
}

transport::MemoryRegion DeviceRegion(void* addr, uint64_t length, int32_t device_id)
{
    return transport::MemoryRegion{addr, length, transport::MemoryType::Device, device_id};
}

transport::MemoryRegionManager::NativeHandles Handles(std::initializer_list<uint64_t> handles)
{
    return transport::MemoryRegionManager::NativeHandles{handles};
}

void AddRegion(transport::MemoryRegionManager& manager, const transport::MemoryRegion& memory,
               transport::MemoryRegionManager::NativeHandles handles = Handles({1}))
{
    const auto handle = transport::MemoryRegionManager::HashMemoryRegion(memory);
    ASSERT_EQ(manager.ValidateMemoryRegion(memory), UC::Status::OK());
    ASSERT_EQ(manager.AddMemoryRegion(kProtocol, memory, std::move(handles), handle),
              UC::Status::OK());
}

}  // namespace

TEST(UCP2PMemoryManagementNormalTest, RegistersHostAndDeviceMemory)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> host_backing{};
    std::array<std::byte, 128> device_backing{};

    const auto host = HostRegion(host_backing.data(), host_backing.size());
    const auto device = DeviceRegion(device_backing.data(), device_backing.size(), 0);
    AddRegion(manager, host, Handles({11, 12}));
    AddRegion(manager, device, Handles({21}));

    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol,
                                       transport::MemoryRegionManager::HashMemoryRegion(host),
                                       region),
              UC::Status::OK());
    ASSERT_EQ(region.memory.type, transport::MemoryType::Host);
    ASSERT_EQ(region.registrations[static_cast<size_t>(kProtocol)]->native_handles,
              transport::MemoryRegionManager::NativeHandles({11, 12}));

    ASSERT_EQ(manager.FindMemoryRegion(kProtocol,
                                       transport::MemoryRegionManager::HashMemoryRegion(device),
                                       region),
              UC::Status::OK());
    ASSERT_EQ(region.memory.type, transport::MemoryType::Device);
    ASSERT_EQ(region.memory.device_id, 0);
}

TEST(UCP2PMemoryManagementNormalTest, AllowsSameAddressInDifferentDeviceDomains)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> backing{};
    AddRegion(manager, HostRegion(backing.data(), 64), Handles({1}));
    AddRegion(manager, DeviceRegion(backing.data(), 64, 0), Handles({2}));
    AddRegion(manager, DeviceRegion(backing.data(), 64, 1), Handles({3}));

    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, -1, backing.data(), region), UC::Status::OK());
    ASSERT_EQ(region.memory.device_id, -1);
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, 0, backing.data(), region), UC::Status::OK());
    ASSERT_EQ(region.memory.device_id, 0);
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, 1, backing.data(), region), UC::Status::OK());
    ASSERT_EQ(region.memory.device_id, 1);
}

TEST(UCP2PMemoryManagementNormalTest, AllowsAdjacentRegions)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> backing{};
    AddRegion(manager, HostRegion(backing.data(), 64));
    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(backing.data() + 64, 64)),
              UC::Status::OK());
    AddRegion(manager, HostRegion(backing.data() + 64, 64), Handles({2}));

    std::vector<transport::MemoryRegionManager::Region> regions;
    ASSERT_EQ(manager.GetMemoryRegions(kProtocol, regions), UC::Status::OK());
    ASSERT_EQ(regions.size(), 2);
}

TEST(UCP2PMemoryManagementNormalTest, FindsByHandleStartAddressAndContainedRange)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 256> backing{};
    const auto memory = HostRegion(backing.data() + 32, 128);
    AddRegion(manager, memory, Handles({100}));
    const auto handle = transport::MemoryRegionManager::HashMemoryRegion(memory);

    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, handle, region), UC::Status::OK());
    ASSERT_EQ(region.handle, handle);
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, -1, memory.addr, region), UC::Status::OK());
    ASSERT_EQ(region.handle, handle);
    ASSERT_EQ(manager.FindContainingMemoryRegion(kProtocol, -1, backing.data() + 64, 32, region),
              UC::Status::OK());
    ASSERT_EQ(region.handle, handle);
}

TEST(UCP2PMemoryManagementNormalTest, FindsBoundaryRanges)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> backing{};
    AddRegion(manager, HostRegion(backing.data(), 64));

    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindContainingMemoryRegion(kProtocol, -1, backing.data(), 64, region),
              UC::Status::OK());
    ASSERT_EQ(manager.FindContainingMemoryRegion(kProtocol, -1, backing.data() + 63, 1, region),
              UC::Status::OK());
    ASSERT_EQ(manager.FindContainingMemoryRegion(kProtocol, -1, backing.data() + 64, 1, region),
              UC::Status::NotFound());
}

TEST(UCP2PMemoryManagementNormalTest, ExplicitUnregisterClearsProtocolState)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> backing{};
    const auto memory = HostRegion(backing.data(), 64);
    const auto handle = transport::MemoryRegionManager::HashMemoryRegion(memory);
    AddRegion(manager, memory, Handles({7}));

    ASSERT_EQ(manager.RemoveMemoryRegion(kProtocol, handle), UC::Status::OK());
    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, handle, region), UC::Status::NotFound());
}

TEST(UCP2PMemoryManagementNormalTest, EnumeratesOnlyRegisteredProtocolRegions)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> backing{};
    AddRegion(manager, HostRegion(backing.data(), 64), Handles({1}));
    AddRegion(manager, HostRegion(backing.data() + 64, 64), Handles({2}));

    std::vector<transport::MemoryRegionManager::Region> regions;
    ASSERT_EQ(manager.GetMemoryRegions(kProtocol, regions), UC::Status::OK());
    ASSERT_EQ(regions.size(), 2);
}

TEST(UCP2PMemoryManagementExceptionTest, RejectsNullZeroAndOverflowRanges)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 16> backing{};
    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(nullptr, 16)), UC::Status::InvalidParam());
    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(backing.data(), 0)),
              UC::Status::InvalidParam());

    auto overflow =
        HostRegion(reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 3), 8);
    ASSERT_EQ(manager.ValidateMemoryRegion(overflow), UC::Status::InvalidParam());
}

TEST(UCP2PMemoryManagementExceptionTest, RejectsOverlappingRangesWithoutChangingOriginal)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 256> backing{};
    const auto original = HostRegion(backing.data() + 64, 64);
    AddRegion(manager, original, Handles({44}));

    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(backing.data() + 64, 32)),
              UC::Status::InvalidParam());
    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(backing.data() + 32, 64)),
              UC::Status::InvalidParam());
    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(backing.data() + 96, 64)),
              UC::Status::InvalidParam());
    ASSERT_EQ(manager.ValidateMemoryRegion(HostRegion(backing.data() + 32, 128)),
              UC::Status::InvalidParam());

    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol,
                                       transport::MemoryRegionManager::HashMemoryRegion(original),
                                       region),
              UC::Status::OK());
    ASSERT_EQ(region.registrations[static_cast<size_t>(kProtocol)]->native_handles,
              transport::MemoryRegionManager::NativeHandles({44}));
}

TEST(UCP2PMemoryManagementExceptionTest, QueryFailuresReturnNotFound)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 128> backing{};
    AddRegion(manager, HostRegion(backing.data(), 64));

    transport::MemoryRegionManager::Region region;
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, 1234, region), UC::Status::NotFound());
    ASSERT_EQ(manager.FindMemoryRegion(kProtocol, -1, backing.data() + 64, region),
              UC::Status::NotFound());
    ASSERT_EQ(manager.FindContainingMemoryRegion(kProtocol, -1, backing.data() + 32, 64, region),
              UC::Status::NotFound());
}

TEST(UCP2PMemoryManagementExceptionTest, InvalidAndUnknownHandleRemovalFail)
{
    transport::MemoryRegionManager manager;
    ASSERT_EQ(manager.RemoveMemoryRegion(kProtocol, transport::kInvalidMemoryHandle),
              UC::Status::InvalidParam());
    ASSERT_EQ(manager.RemoveMemoryRegion(kProtocol, 9999), UC::Status::InvalidParam());
}

TEST(UCP2PMemoryManagementConcurrentTest, ConcurrentNonOverlappingAddsAreRetained)
{
    transport::MemoryRegionManager manager;
    std::array<std::byte, 4096> backing{};
    constexpr size_t kThreadCount = 8;
    std::vector<std::thread> threads;
    for (size_t i = 0; i < kThreadCount; ++i) {
        threads.emplace_back([&manager, &backing, i]() {
            const auto memory = HostRegion(backing.data() + i * 128, 64);
            AddRegion(manager, memory, Handles({i + 1}));
        });
    }
    for (auto& thread : threads) { thread.join(); }

    std::vector<transport::MemoryRegionManager::Region> regions;
    ASSERT_EQ(manager.GetMemoryRegions(kProtocol, regions), UC::Status::OK());
    ASSERT_EQ(regions.size(), kThreadCount);
}

TEST(UCP2PMemoryManagementHandleTest, SameRegionGeneratesSameHandle)
{
    std::array<std::byte, 64> backing{};
    const auto a = HostRegion(backing.data(), 64);
    const auto b = HostRegion(backing.data(), 64);
    ASSERT_EQ(transport::MemoryRegionManager::HashMemoryRegion(a),
              transport::MemoryRegionManager::HashMemoryRegion(b));
    ASSERT_NE(transport::MemoryRegionManager::HashMemoryRegion(a),
              transport::kInvalidMemoryHandle);
}

TEST(UCP2PMemoryManagementHandleTest, LengthAndDeviceIdParticipateInHandle)
{
    std::array<std::byte, 64> backing{};
    const auto base = HostRegion(backing.data(), 64);
    const auto shorter = HostRegion(backing.data(), 32);
    const auto device = DeviceRegion(backing.data(), 64, 0);
    ASSERT_NE(transport::MemoryRegionManager::HashMemoryRegion(base),
              transport::MemoryRegionManager::HashMemoryRegion(shorter));
    ASSERT_NE(transport::MemoryRegionManager::HashMemoryRegion(base),
              transport::MemoryRegionManager::HashMemoryRegion(device));
}
