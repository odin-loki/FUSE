#include <fuse/net/transport.hpp>

#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace fuse::net {

namespace {

struct LoopbackPeer {
    u32 local_id = 0;
    LoopbackTransport* linked = nullptr;
    u32 linked_remote_id = 0;
    std::deque<Packet> inbox;
    u32 ping = 1;
    bool connected = false;
};

std::mutex g_loopback_mutex;
u32 g_next_loopback_id = 1;

} // namespace

struct LoopbackTransport::Impl {
    bool active = false;
    u16 port = 0;
    LoopbackPeer peer;
};

TransportBackend active_transport_backend() { return TransportBackend::Loopback; }

LoopbackTransport::LoopbackTransport() : m_impl(std::make_unique<Impl>()) {}

LoopbackTransport::~LoopbackTransport() { destroy(); }

bool LoopbackTransport::init(u16 port) {
    std::lock_guard lock(g_loopback_mutex);
    m_impl->active = true;
    m_impl->port = port;
    m_impl->peer.local_id = g_next_loopback_id++;
    m_impl->peer.connected = true;
    return true;
}

void LoopbackTransport::destroy() {
    std::lock_guard lock(g_loopback_mutex);
    if (m_impl->peer.linked != nullptr) {
        m_impl->peer.linked->m_impl->peer.linked = nullptr;
        m_impl->peer.linked->m_impl->peer.connected = false;
        m_impl->peer.linked = nullptr;
    }
    m_impl->active = false;
    m_impl->peer.connected = false;
    m_impl->peer.inbox.clear();
}

bool LoopbackTransport::connect(const char* /*address*/, u16 /*port*/) {
    return m_impl->active;
}

void LoopbackTransport::disconnect(u32 /*peer_id*/) {
    std::lock_guard lock(g_loopback_mutex);
    if (m_impl->peer.linked != nullptr) {
        m_impl->peer.linked->m_impl->peer.linked = nullptr;
        m_impl->peer.linked->m_impl->peer.connected = false;
        m_impl->peer.linked = nullptr;
    }
    m_impl->peer.connected = false;
}

bool LoopbackTransport::send(u32 peer_id, const byte* data, usize size, PacketChannel channel) {
    if (!m_impl->active || data == nullptr || size == 0) {
        return false;
    }

    Packet packet;
    packet.data.assign(data, data + size);
    packet.peer_id = peer_id;
    packet.channel = channel;
    packet.timestamp_us = 0;

    std::lock_guard lock(g_loopback_mutex);
    if (m_impl->peer.linked == nullptr || !m_impl->peer.linked->m_impl->active) {
        return false;
    }

    packet.peer_id = m_impl->peer.local_id;
    m_impl->peer.linked->m_impl->peer.inbox.push_back(std::move(packet));
    return true;
}

void LoopbackTransport::broadcast(const byte* data, usize size, PacketChannel channel) {
    send(m_impl->peer.linked_remote_id, data, size, channel);
}

void LoopbackTransport::poll(std::function<void(const Packet&)> on_packet) {
    if (!on_packet) {
        return;
    }

    std::deque<Packet> local_inbox;
    {
        std::lock_guard lock(g_loopback_mutex);
        local_inbox.swap(m_impl->peer.inbox);
    }

    for (const Packet& packet : local_inbox) {
        on_packet(packet);
    }
}

u32 LoopbackTransport::peer_count() const {
    return (m_impl->active && m_impl->peer.linked != nullptr && m_impl->peer.connected) ? 1u : 0u;
}

u32 LoopbackTransport::ping_ms(u32 /*peer_id*/) const { return m_impl->peer.ping; }

void LoopbackTransport::link_peers(LoopbackTransport& a, LoopbackTransport& b) {
    std::lock_guard lock(g_loopback_mutex);
    a.m_impl->peer.linked = &b;
    b.m_impl->peer.linked = &a;
    a.m_impl->peer.linked_remote_id = b.m_impl->peer.local_id;
    b.m_impl->peer.linked_remote_id = a.m_impl->peer.local_id;
    a.m_impl->peer.connected = true;
    b.m_impl->peer.connected = true;
}

bool ENetTransport::init(u16 /*port*/) { return false; }
void ENetTransport::destroy() {}
bool ENetTransport::connect(const char* /*address*/, u16 /*port*/) { return false; }
void ENetTransport::disconnect(u32 /*peer_id*/) {}
bool ENetTransport::send(u32 /*peer_id*/, const byte* /*data*/, usize /*size*/, PacketChannel /*channel*/) {
    return false;
}
void ENetTransport::broadcast(const byte* /*data*/, usize /*size*/, PacketChannel /*channel*/) {}
void ENetTransport::poll(std::function<void(const Packet&)> /*on_packet*/) {}
u32 ENetTransport::peer_count() const { return 0; }
u32 ENetTransport::ping_ms(u32 /*peer_id*/) const { return 0; }

bool SteamTransport::init(u16 /*port*/) { return false; }
void SteamTransport::destroy() {}
bool SteamTransport::connect(const char* /*address*/, u16 /*port*/) { return false; }
void SteamTransport::disconnect(u32 /*peer_id*/) {}
bool SteamTransport::send(u32 /*peer_id*/, const byte* /*data*/, usize /*size*/, PacketChannel /*channel*/) {
    return false;
}
void SteamTransport::broadcast(const byte* /*data*/, usize /*size*/, PacketChannel /*channel*/) {}
void SteamTransport::poll(std::function<void(const Packet&)> /*on_packet*/) {}
u32 SteamTransport::peer_count() const { return 0; }
u32 SteamTransport::ping_ms(u32 /*peer_id*/) const { return 0; }

} // namespace fuse::net
