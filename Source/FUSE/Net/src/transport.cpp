#include <fuse/net/transport.hpp>

#include <fuse/alloc/size_class_allocator.hpp>

#if defined(FUSE_NET_HAS_ENET)
#include <enet/enet.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#endif

#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace fuse::net {

namespace {

/// Packet queue whose slots (and their byte buffers) are recycled: after warm-up, loopback
/// send/poll cycles reuse capacity instead of allocating per packet (FUSE_MASTER_PLAN B1.8).
struct PacketQueue {
    std::vector<Packet> slots;
    usize count = 0;

    Packet& push() {
        if (count == slots.size()) {
            slots.emplace_back();
        }
        return slots[count++];
    }
    void clear() { count = 0; }
    [[nodiscard]] usize size() const { return count; }
};

struct LoopbackPeer {
    u32 local_id = 0;
    LoopbackTransport* linked = nullptr;
    u32 linked_remote_id = 0;
    PacketQueue inbox;
    PacketQueue delivering; // inbox contents being dispatched by poll() (swapped out under the lock)
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
#if defined(FUSE_NET_HAS_ENET)
    if (backend == TransportBackend::ENet) {
        return true;
    }
#endif
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
    (void)peer_id;

    std::lock_guard lock(g_loopback_mutex);
    if (m_impl->peer.linked == nullptr || !m_impl->peer.linked->m_impl->active) {
        return false;
    }

    Packet& packet = m_impl->peer.linked->m_impl->peer.inbox.push();
    packet.data.assign(data, data + size);
    packet.channel = channel;
    packet.timestamp_us = 0;
    packet.sequence = channel == PacketChannel::UnreliableSeq ? m_impl->peer.next_unreliable_seq++ : 0u;
    packet.peer_id = m_impl->peer.local_id;
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

    PacketQueue& local_inbox = m_impl->peer.delivering;
    {
        std::lock_guard lock(g_loopback_mutex);
        std::swap(local_inbox, m_impl->peer.inbox);
        m_impl->peer.inbox.clear();
        m_impl->peer.stats.pending_outbox = 0;
    }

    const Packet* best_unreliable_seq = nullptr;
    for (usize i = 0; i < local_inbox.size(); ++i) {
        const Packet& packet = local_inbox.slots[i];
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
    local_inbox.clear();
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

#if defined(FUSE_NET_HAS_ENET)

namespace {

std::mutex g_enet_init_mutex;
u32 g_enet_init_refs = 0;

/// Every ENet-internal allocation (packets, fragments, incoming/outgoing commands,
/// acknowledgements, peers) goes through this process-wide, thread-safe size-class pool instead
/// of malloc: a steady-state send/receive loop recycles pooled blocks (FUSE_MASTER_PLAN B1.8).
/// Constructed on first use and intentionally never destroyed, so ENet memory stays valid for
/// the whole process regardless of static destruction order.
alloc::SizeClassAllocator& enet_pool() {
    alignas(alloc::SizeClassAllocator) static unsigned char storage[sizeof(alloc::SizeClassAllocator)];
    static alloc::SizeClassAllocator* const pool = new (storage) alloc::SizeClassAllocator(
        alloc::SizeClassAllocatorDesc{"net.enet", 64u * 1024u, 0u, true});
    return *pool;
}

void* ENET_CALLBACK enet_pool_malloc(size_t size) { return enet_pool().allocateUnsized(size); }

void ENET_CALLBACK enet_pool_free(void* memory) { enet_pool().deallocateUnsized(memory); }

bool enet_acquire() {
    std::lock_guard lock(g_enet_init_mutex);
    if (g_enet_init_refs == 0) {
        ENetCallbacks callbacks{};
        callbacks.malloc = enet_pool_malloc;
        callbacks.free = enet_pool_free;
        callbacks.no_memory = nullptr; // keep ENet's default (abort)
        if (enet_initialize_with_callbacks(ENET_VERSION, &callbacks) != 0) {
            return false;
        }
    }
    ++g_enet_init_refs;
    return true;
}

void enet_release() {
    std::lock_guard lock(g_enet_init_mutex);
    if (g_enet_init_refs == 0) {
        return;
    }
    if (--g_enet_init_refs == 0) {
        enet_deinitialize();
    }
}

constexpr usize kENetMaxPeers = 32;
constexpr enet_uint8 kENetChannelCount = 3;

enet_uint8 channel_index(PacketChannel channel) {
    switch (channel) {
    case PacketChannel::Reliable:
        return 0;
    case PacketChannel::Unreliable:
        return 1;
    case PacketChannel::UnreliableSeq:
        return 2;
    }
    return 0;
}

enet_uint32 channel_flags(PacketChannel channel) {
    switch (channel) {
    case PacketChannel::Reliable:
        return ENET_PACKET_FLAG_RELIABLE;
    case PacketChannel::Unreliable:
        return ENET_PACKET_FLAG_UNSEQUENCED;
    case PacketChannel::UnreliableSeq:
        return 0; // ENet drops unreliable packets older than the newest on a sequenced channel.
    }
    return ENET_PACKET_FLAG_RELIABLE;
}

PacketChannel channel_from_index(enet_uint8 index) {
    switch (index) {
    case 1:
        return PacketChannel::Unreliable;
    case 2:
        return PacketChannel::UnreliableSeq;
    default:
        return PacketChannel::Reliable;
    }
}

u32 peer_handle(const ENetPeer* peer) {
    return static_cast<u32>(reinterpret_cast<std::uintptr_t>(peer->data));
}

} // namespace

struct ENetTransport::Impl {
    ENetHost* host = nullptr;          // Owned: released with enet_host_destroy.
    bool enet_acquired = false;
    u32 next_peer_id = 1;
    std::unordered_map<u32, ENetPeer*> peers; // Non-owning: peers belong to `host`.
    std::unordered_map<u32, bool> connected;
    TransportStats stats{};
    Packet rx_packet; // reused for every received packet (keeps its buffer capacity)
    u32 timeout_limit = 0;
    u32 timeout_min_ms = 0;
    u32 timeout_max_ms = 0;
    ConnectionCallback on_connection;

    ENetPeer* find(u32 peer_id) const {
        const auto it = peers.find(peer_id);
        return it != peers.end() ? it->second : nullptr;
    }

    u32 register_peer(ENetPeer* peer) {
        const u32 id = next_peer_id++;
        peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        peers[id] = peer;
        connected[id] = false;
        if (timeout_limit != 0 || timeout_min_ms != 0 || timeout_max_ms != 0) {
            enet_peer_timeout(peer, timeout_limit, timeout_min_ms, timeout_max_ms);
        }
        return id;
    }

    void forget_peer(u32 peer_id) {
        peers.erase(peer_id);
        connected.erase(peer_id);
    }
};

ENetTransport::ENetTransport() : m_impl(std::make_unique<Impl>()), m_last_error("") {}

ENetTransport::~ENetTransport() { destroy(); }

bool ENetTransport::init(u16 port) {
    destroy();
    if (!enet_acquire()) {
        m_last_error = "enet_initialize failed";
        return false;
    }
    m_impl->enet_acquired = true;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    m_impl->host = enet_host_create(&address, kENetMaxPeers, kENetChannelCount, 0, 0);
    if (m_impl->host == nullptr) {
        m_last_error = "enet_host_create failed (port in use?)";
        enet_release();
        m_impl->enet_acquired = false;
        return false;
    }
    m_last_error = "";
    return true;
}

void ENetTransport::destroy() {
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->host != nullptr) {
        enet_host_flush(m_impl->host);
        enet_host_destroy(m_impl->host);
        m_impl->host = nullptr;
    }
    m_impl->peers.clear();
    m_impl->connected.clear();
    m_impl->stats = {};
    if (m_impl->enet_acquired) {
        enet_release();
        m_impl->enet_acquired = false;
    }
}

u32 ENetTransport::connect_peer(const char* address, u16 port) {
    if (m_impl->host == nullptr || address == nullptr) {
        m_last_error = "connect before init";
        return 0;
    }
    ENetAddress remote{};
    if (enet_address_set_host(&remote, address) != 0) {
        m_last_error = "unresolvable host";
        return 0;
    }
    remote.port = port;
    ENetPeer* peer = enet_host_connect(m_impl->host, &remote, kENetChannelCount, 0);
    if (peer == nullptr) {
        m_last_error = "no free peer slot";
        return 0;
    }
    const u32 id = m_impl->register_peer(peer);
    enet_host_flush(m_impl->host);
    return id;
}

bool ENetTransport::connect(const char* address, u16 port) { return connect_peer(address, port) != 0; }

void ENetTransport::disconnect(u32 peer_id) {
    ENetPeer* peer = m_impl->find(peer_id);
    if (peer == nullptr || m_impl->host == nullptr) {
        return;
    }
    // Graceful: queued reliable data is delivered before the disconnect notification.
    enet_peer_disconnect_later(peer, 0);
    enet_host_flush(m_impl->host);
}

bool ENetTransport::send(u32 peer_id, const byte* data, usize size, PacketChannel channel) {
    if (m_impl->host == nullptr || data == nullptr || size == 0) {
        return false;
    }
    ENetPeer* peer = m_impl->find(peer_id);
    if (peer == nullptr || peer->state != ENET_PEER_STATE_CONNECTED) {
        return false;
    }
    ENetPacket* packet = enet_packet_create(data, size, channel_flags(channel));
    if (packet == nullptr) {
        return false;
    }
    if (enet_peer_send(peer, channel_index(channel), packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    ++m_impl->stats.packets_sent;
    m_impl->stats.bytes_sent += size;
    return true;
}

void ENetTransport::broadcast(const byte* data, usize size, PacketChannel channel) {
    for (const auto& [peer_id, is_connected] : m_impl->connected) {
        if (is_connected) {
            (void)send(peer_id, data, size, channel);
        }
    }
}

void ENetTransport::poll(std::function<void(const Packet&)> on_packet) {
    if (m_impl->host == nullptr) {
        return;
    }

    ENetEvent event{};
    while (m_impl->host != nullptr && enet_host_service(m_impl->host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            u32 id = event.peer->data != nullptr ? peer_handle(event.peer) : 0u;
            if (id == 0 || m_impl->find(id) != event.peer) {
                id = m_impl->register_peer(event.peer); // Incoming connection.
            }
            m_impl->connected[id] = true;
            if (m_impl->on_connection) {
                m_impl->on_connection(id, true);
            }
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const u32 id = event.peer->data != nullptr ? peer_handle(event.peer) : 0u;
            event.peer->data = nullptr;
            if (id != 0) {
                m_impl->forget_peer(id);
                if (m_impl->on_connection) {
                    m_impl->on_connection(id, false);
                }
            }
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            Packet& packet = m_impl->rx_packet;
            packet.data.assign(event.packet->data, event.packet->data + event.packet->dataLength);
            packet.peer_id = peer_handle(event.peer);
            packet.channel = channel_from_index(event.channelID);
            packet.sequence = 0;
            packet.timestamp_us = static_cast<u64>(enet_time_get()) * 1000ull;
            enet_packet_destroy(event.packet);
            ++m_impl->stats.packets_received;
            m_impl->stats.bytes_received += packet.data.size();
            if (on_packet) {
                on_packet(packet);
            }
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }
}

u32 ENetTransport::peer_count() const {
    u32 count = 0;
    for (const auto& entry : m_impl->connected) {
        count += entry.second ? 1u : 0u;
    }
    return count;
}

u32 ENetTransport::ping_ms(u32 peer_id) const {
    const ENetPeer* peer = m_impl->find(peer_id);
    return peer != nullptr ? static_cast<u32>(peer->roundTripTime) : 0u;
}

TransportStats ENetTransport::stats() const { return m_impl->stats; }

bool ENetTransport::is_connected(u32 peer_id) const {
    const auto it = m_impl->connected.find(peer_id);
    return it != m_impl->connected.end() && it->second;
}

u16 ENetTransport::bound_port() const {
    if (m_impl->host == nullptr) {
        return 0;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(bound));
#else
    socklen_t length = sizeof(bound);
#endif
    if (getsockname(m_impl->host->socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        return 0;
    }
    return ntohs(bound.sin_port);
}

void ENetTransport::set_timeouts(u32 timeout_limit, u32 timeout_min_ms, u32 timeout_max_ms) {
    m_impl->timeout_limit = timeout_limit;
    m_impl->timeout_min_ms = timeout_min_ms;
    m_impl->timeout_max_ms = timeout_max_ms;
    for (const auto& [peer_id, peer] : m_impl->peers) {
        (void)peer_id;
        enet_peer_timeout(peer, timeout_limit, timeout_min_ms, timeout_max_ms);
    }
}

void ENetTransport::set_connection_callback(ConnectionCallback callback) {
    m_impl->on_connection = std::move(callback);
}

#else // !FUSE_NET_HAS_ENET

struct ENetTransport::Impl {};

ENetTransport::ENetTransport() = default;
ENetTransport::~ENetTransport() = default;
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
u32 ENetTransport::connect_peer(const char* /*address*/, u16 /*port*/) { return 0; }
bool ENetTransport::is_connected(u32 /*peer_id*/) const { return false; }
u16 ENetTransport::bound_port() const { return 0; }
void ENetTransport::set_timeouts(u32 /*timeout_limit*/, u32 /*timeout_min_ms*/, u32 /*timeout_max_ms*/) {}
void ENetTransport::set_connection_callback(ConnectionCallback /*callback*/) {}

#endif // FUSE_NET_HAS_ENET

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
