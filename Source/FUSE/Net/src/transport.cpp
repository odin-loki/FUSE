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
    u32 next_unreliable_seq = 1;
    u32 last_delivered_unreliable_seq = 0;
    TransportStats stats{};
};

std::mutex g_loopback_mutex;
u32 g_next_loopback_id = 1;

TransportStats make_zero_stats() { return {}; }

} // namespace

struct LoopbackTransport::Impl {
    bool active = false;
    u16 port = 0;
    LoopbackPeer peer;
};

TransportBackend active_transport_backend() { return TransportBackend::Loopback; }

const char* transport_backend_name(TransportBackend backend) {
    switch (backend) {
    case TransportBackend::Loopback:
        return "loopback";
    case TransportBackend::ENet:
        return "enet";
    case TransportBackend::Steam:
        return "steam";
    }
    return "unknown";
}

bool transport_backend_available(TransportBackend backend) {
    return backend == TransportBackend::Loopback;
}

std::unique_ptr<Transport> create_transport(TransportBackend backend) {
    switch (backend) {
    case TransportBackend::Loopback:
        return std::make_unique<LoopbackTransport>();
    case TransportBackend::ENet:
        return std::make_unique<ENetTransport>();
    case TransportBackend::Steam:
        return std::make_unique<SteamTransport>();
    }
    return nullptr;
}

LoopbackTransport::LoopbackTransport() : m_impl(std::make_unique<Impl>()) {}

LoopbackTransport::~LoopbackTransport() { destroy(); }

bool LoopbackTransport::init(u16 port) {
    std::lock_guard lock(g_loopback_mutex);
    m_impl->active = true;
    m_impl->port = port;
    m_impl->peer.local_id = g_next_loopback_id++;
    m_impl->peer.connected = true;
    m_impl->peer.stats = {};
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
    m_impl->peer.stats = {};
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

    if (channel == PacketChannel::UnreliableSeq) {
        packet.sequence = m_impl->peer.next_unreliable_seq++;
    }

    packet.peer_id = m_impl->peer.local_id;
    m_impl->peer.linked->m_impl->peer.inbox.push_back(packet);
    ++m_impl->peer.stats.packets_sent;
    m_impl->peer.stats.bytes_sent += size;
    m_impl->peer.linked->m_impl->peer.stats.pending_outbox =
        static_cast<u32>(m_impl->peer.linked->m_impl->peer.inbox.size());
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
        m_impl->peer.stats.pending_outbox = 0;
    }

    const Packet* best_unreliable_seq = nullptr;
    for (const Packet& packet : local_inbox) {
        if (packet.channel != PacketChannel::UnreliableSeq) {
            ++m_impl->peer.stats.packets_received;
            m_impl->peer.stats.bytes_received += packet.data.size();
            on_packet(packet);
            continue;
        }

        if (m_drop_stale_unreliable_seq && packet.sequence <= m_impl->peer.last_delivered_unreliable_seq) {
            ++m_impl->peer.stats.dropped_stale_seq;
            continue;
        }

        if (best_unreliable_seq == nullptr || packet.sequence > best_unreliable_seq->sequence) {
            if (best_unreliable_seq != nullptr) {
                ++m_impl->peer.stats.dropped_stale_seq;
            }
            best_unreliable_seq = &packet;
        } else {
            ++m_impl->peer.stats.dropped_stale_seq;
        }
    }

    if (best_unreliable_seq != nullptr) {
        m_impl->peer.last_delivered_unreliable_seq = best_unreliable_seq->sequence;
        ++m_impl->peer.stats.packets_received;
        m_impl->peer.stats.bytes_received += best_unreliable_seq->data.size();
        on_packet(*best_unreliable_seq);
    }
}

u32 LoopbackTransport::peer_count() const {
    return (m_impl->active && m_impl->peer.linked != nullptr && m_impl->peer.connected) ? 1u : 0u;
}

u32 LoopbackTransport::ping_ms(u32 /*peer_id*/) const { return m_impl->peer.ping; }

TransportStats LoopbackTransport::stats() const {
    std::lock_guard lock(g_loopback_mutex);
    TransportStats stats = m_impl->peer.stats;
    stats.pending_outbox = static_cast<u32>(m_impl->peer.inbox.size());
    return stats;
}

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
TransportStats ENetTransport::stats() const { return make_zero_stats(); }

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
TransportStats SteamTransport::stats() const { return make_zero_stats(); }

} // namespace fuse::net
