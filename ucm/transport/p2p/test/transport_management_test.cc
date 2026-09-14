/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 */
#include <array>
#include <gtest/gtest.h>
#include "common/binary_codec.h"
#include "core/transport_manager.h"

namespace {

transport::MemoryRegion HostRegion(void* addr, uint64_t length)
{
    return transport::MemoryRegion{addr, length, transport::MemoryType::Host, -1};
}

}  // namespace

TEST(UCP2PTransportManagementPathSelectionTest, UnsupportedDirectionFailsBeforeSubmit)
{
    transport::TransportManager manager{"127.0.0.1:32001"};
    transport::Operation op;
    op.target_manager = "127.0.0.1:32002";
    op.direct = transport::OperationDirect::LocalDeviceHost;
    op.ops.push_back(transport::Segment{reinterpret_cast<void*>(0x1000), 0x2000, 16});

    ASSERT_EQ(manager.ExecuteSync(op), UC::Status::Unsupported());
    transport::TransferHandle handle = 123;
    ASSERT_EQ(manager.ExecuteAsync(op, handle), UC::Status::Unsupported());
    ASSERT_EQ(handle, transport::kInvalidTransferHandle);
}

TEST(UCP2PTransportManagementStatusTest, InvalidAndUnknownHandlesFail)
{
    transport::TransportManager manager{"127.0.0.1:32003"};
    transport::TransferStatus status = transport::TransferStatus::Waiting;
    ASSERT_EQ(manager.GetStatus(transport::kInvalidTransferHandle, status),
              UC::Status::InvalidParam());
    ASSERT_EQ(manager.GetStatus(42, status), UC::Status::Error());
}

TEST(UCP2PTransportManagementLifecycleTest, InitValidatesManagerId)
{
    transport::TransportManager invalid{"not-a-manager-id"};
    ASSERT_EQ(invalid.Init(), UC::Status::InvalidParam());
}

TEST(UCP2PTransportManagementLifecycleTest, InstallUnsupportedProtocolWhenBackendUnavailable)
{
    transport::TransportManager manager{"127.0.0.1:32004"};
    transport::InitAttrs attrs;
    ASSERT_EQ(manager.InstallTransport(transport::TransportProtocol::Hixl, attrs),
              UC::Status::Unsupported());
}

TEST(UCP2PTransportManagementLifecycleTest, RegisterMemoryValidatesInputs)
{
    transport::TransportManager manager{"127.0.0.1:32005"};
    std::array<std::byte, 64> backing{};
    transport::MemoryHandle handle = transport::kInvalidMemoryHandle;
    ASSERT_EQ(manager.RegisterMemory(HostRegion(nullptr, 64), handle), UC::Status::InvalidParam());
    ASSERT_EQ(handle, transport::kInvalidMemoryHandle);

    ASSERT_EQ(manager.RegisterMemory(HostRegion(backing.data(), backing.size()), handle),
              UC::Status::OK());
    ASSERT_NE(handle, transport::kInvalidMemoryHandle);

    // No protocol is installed in this test build, so unregistering a manager-only
    // registration does not find protocol-owned native state.
    ASSERT_EQ(manager.UnregisterMemory(handle), UC::Status::InvalidParam());
}

TEST(UCP2PTransportManagementMessageCodecTest, EncodesBytesAndStringsForControlPayloads)
{
    transport::Metadata data;
    const transport::Metadata payload{1, 2, 3, 4};
    ASSERT_TRUE(transport::detail::AppendBytes(data, payload));
    ASSERT_TRUE(transport::detail::AppendString(data, "peer-a"));

    size_t offset = 0;
    transport::Metadata decoded_payload;
    std::string decoded_peer;
    ASSERT_TRUE(transport::detail::ReadBytes(data, offset, decoded_payload));
    ASSERT_TRUE(transport::detail::ReadString(data, offset, decoded_peer));
    ASSERT_EQ(decoded_payload, payload);
    ASSERT_EQ(decoded_peer, "peer-a");

    auto truncated = data;
    truncated.pop_back();
    offset = 0;
    ASSERT_TRUE(transport::detail::ReadBytes(truncated, offset, decoded_payload));
    ASSERT_FALSE(transport::detail::ReadString(truncated, offset, decoded_peer));
}
