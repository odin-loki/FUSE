#pragma once

#include <fuse/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fuse::net {

using byte = u8;

enum class PacketChannel : u8 {
    Reliable,
    Unreliable,
    UnreliableSeq,
};

struct Packet {
    std::vector<byte> data;
    u32 peer_id = 0;
    PacketChannel channel = PacketChannel::Reliable;
    u64 timestamp_us = 0;
    u32 sequence = 0;
};

struct TransportStats {
    u64 packets_sent = 0;
    u64 packets_received = 0;
    u64 bytes_sent = 0;
    u64 bytes_received = 0;
    u32 pending_outbox = 0;
    u32 dropped_stale_seq = 0;
};

/// Transport abstraction — swap ENet / Steam / loopback without touching game logic (B7.4).
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool init(u16 port) = 0;
    virtual void destroy() = 0;
    virtual bool connect(const char* address, u16 port) = 0;
    virtual void disconnect(u32 peer_id) = 0;
    virtual bool send(u32 peer_id, const byte* data, usize size, PacketChannel channel) = 0;
    virtual void broadcast(const byte* data, usize size, PacketChannel channel) = 0;
    virtual void poll(std::function<void(const Packet&)> on_packet) = 0;
    [[nodiscard]] virtual u32 peer_count() const = 0;
    [[nodiscard]] virtual u32 ping_ms(u32 peer_id) const = 0;
    [[nodiscard]] virtual TransportStats stats() const = 0;
};

enum class TransportBackend : u8 {
    Loopback,
    ENet,
    Steam,
};

[[nodiscard]] TransportBackend active_transport_backend();
[[nodiscard]] const char* transport_backend_name(TransportBackend backend);
[[nodiscard]] bool transport_backend_available(TransportBackend backend);
[[nodiscard]] std::unique_ptr<Transport> create_transport(TransportBackend backend);

/// In-process loopback transport for tests and single-machine dev.
class LoopbackTransport : public Transport {
public:
    LoopbackTransport();
    ~LoopbackTransport() override;

    bool init(u16 port) override;
    void destroy() override;
    bool connect(const char* address, u16 port) override;
    void disconnect(u32 peer_id) override;
    bool send(u32 peer_id, const byte* data, usize size, PacketChannel channel) override;
    void broadcast(const byte* data, usize size, PacketChannel channel) override;
    void poll(std::function<void(const Packet&)> on_packet) override;
    [[nodiscard]] u32 peer_count() const override;
    [[nodiscard]] u32 ping_ms(u32 peer_id) const override;
    [[nodiscard]] TransportStats stats() const override;

    /// Link two loopback peers for bidirectional delivery (test harness).
    static void link_peers(LoopbackTransport& a, LoopbackTransport& b);

    /// Test hook — drop inbound UnreliableSeq packets older than the last delivered sequence.
    void set_drop_stale_unreliable_seq(bool enabled) { m_drop_stale_unreliable_seq = enabled; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_drop_stale_unreliable_seq = true;
};

/// ENet-backed transport — stub until ENet is wired in CI (returns false from init).
class ENetTransport : public Transport {
public:
    bool init(u16 port) override;
    void destroy() override;
    bool connect(const char* address, u16 port) override;
    void disconnect(u32 peer_id) override;
    bool send(u32 peer_id, const byte* data, usize size, PacketChannel channel) override;
    void broadcast(const byte* data, usize size, PacketChannel channel) override;
    void poll(std::function<void(const Packet&)> on_packet) override;
    [[nodiscard]] u32 peer_count() const override;
    [[nodiscard]] u32 ping_ms(u32 peer_id) const override;
    [[nodiscard]] TransportStats stats() const override;

    [[nodiscard]] const char* last_error() const { return m_last_error; }

private:
    const char* m_last_error = "ENet backend not compiled";
};

/// Steam P2P transport — stub until Steamworks SDK is available.
class SteamTransport : public Transport {
public:
    bool init(u16 port) override;
    void destroy() override;
    bool connect(const char* address, u16 port) override;
    void disconnect(u32 peer_id) override;
    bool send(u32 peer_id, const byte* data, usize size, PacketChannel channel) override;
    void broadcast(const byte* data, usize size, PacketChannel channel) override;
    void poll(std::function<void(const Packet&)> on_packet) override;
    [[nodiscard]] u32 peer_count() const override;
    [[nodiscard]] u32 ping_ms(u32 peer_id) const override;
    [[nodiscard]] TransportStats stats() const override;

    [[nodiscard]] const char* last_error() const { return m_last_error; }

private:
    const char* m_last_error = "Steamworks backend not linked";
};

} // namespace fuse::net
