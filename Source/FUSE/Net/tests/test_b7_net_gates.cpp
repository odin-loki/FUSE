// B7.4 / B7.10 networking gate proofs.
//
// Every check compares against an independent reference: regenerated payload bytes, hand-written
// integrators, analytic motion, brute-force relevance, or a separately-driven simulation.
// Network impairment (loss / duplication / reordering) is injected by a UDP proxy that sits
// between two real ENet endpoints; all sockets bind port 0.

#include <fuse/core/sanitizer.hpp>
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/net/checksum.hpp>
#include <fuse/net/interest_management.hpp>
#include <fuse/net/rollback.hpp>
#include <fuse/net/serializer.hpp>
#include <fuse/net/snapshot_delta.hpp>
#include <fuse/net/state_sync.hpp>
#include <fuse/net/transport.hpp>

#include "test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define FUSE_NET_GATES_POSIX 1
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fuse::net::tests {

namespace {

// ---------------------------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------------------------

struct Rng {
    u64 state;
    explicit Rng(u64 seed) : state(seed != 0 ? seed : 0x9E3779B97F4A7C15ull) {}
    u64 next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    f64 unit() { return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0); }
    u32 range(u32 lo, u32 hi) { return lo + static_cast<u32>(next() % (static_cast<u64>(hi - lo) + 1u)); }
    f32 real(f32 lo, f32 hi) { return lo + static_cast<f32>(unit()) * (hi - lo); }
};

// Only the POSIX two-process ENet gates use wall-clock deadlines.
[[maybe_unused]] u64 now_us() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

bool bits_equal(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof(f32)) == 0; }

bool vec_bits_equal(const ecs::vec3& a, const ecs::vec3& b) {
    return bits_equal(a.x, b.x) && bits_equal(a.y, b.y) && bits_equal(a.z, b.z) && bits_equal(a.w, b.w);
}

bool quat_bits_equal(const ecs::quat& a, const ecs::quat& b) {
    return bits_equal(a.x, b.x) && bits_equal(a.y, b.y) && bits_equal(a.z, b.z) && bits_equal(a.w, b.w);
}

// ---------------------------------------------------------------------------------------------
// Row: ENet transport sends and receives reliable packets between two local processes
// ---------------------------------------------------------------------------------------------

#if defined(FUSE_NET_GATES_POSIX) && defined(FUSE_NET_HAS_ENET)

/// Reference payload for message `index` — regenerated independently on both ends.
std::vector<byte> make_message(u32 index, u32 total) {
    u32 size = 0;
    if (index + 1 == total) {
        size = 64u * 1024u; // Large: ~47 ENet fragments.
    } else if (index % 10 == 0) {
        size = 3000u + (index * 37u) % 5000u; // Fragmented (> MTU).
    } else {
        size = 1u + (index * 131u) % 600u;
    }
    std::vector<byte> out(4u + size);
    std::memcpy(out.data(), &index, 4);
    Rng rng(0xC0FFEEull * (index + 1u));
    for (u32 i = 0; i < size; ++i) {
        out[4u + i] = static_cast<byte>(rng.next() >> 24);
    }
    return out;
}

u32 message_index(const std::vector<byte>& data) {
    u32 index = 0xFFFFFFFFu;
    if (data.size() >= 4) {
        std::memcpy(&index, data.data(), 4);
    }
    return index;
}

struct Impairment {
    f64 drop = 0.0;
    f64 duplicate = 0.0;
    u32 max_delay_ms = 0; // Uniform random per-datagram delay => reordering.
};

/// UDP relay between one client and a server port, injecting loss / duplication / reordering.
class LossyUdpProxy {
public:
    ~LossyUdpProxy() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }

    bool open(u16 server_port, Impairment impairment, u64 seed) {
        m_impairment = impairment;
        m_rng = Rng(seed);
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0) {
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            return false;
        }
        socklen_t length = sizeof(local);
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
            return false;
        }
        m_port = ntohs(local.sin_port);
        m_server.sin_family = AF_INET;
        m_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        m_server.sin_port = htons(server_port);
        return true;
    }

    [[nodiscard]] u16 port() const { return m_port; }

    void pump() {
        byte buffer[65536];
        for (;;) {
            sockaddr_in from{};
            socklen_t from_length = sizeof(from);
            const ssize_t received = ::recvfrom(m_fd, buffer, sizeof(buffer), MSG_DONTWAIT,
                                                reinterpret_cast<sockaddr*>(&from), &from_length);
            if (received <= 0) {
                break;
            }
            const bool from_server = from.sin_port == m_server.sin_port;
            if (!from_server) {
                m_client = from;
                m_has_client = true;
            }
            const u64 arrival = m_next_arrival++;
            if (m_rng.unit() < m_impairment.drop) {
                ++dropped;
                continue;
            }
            enqueue(!from_server, buffer, static_cast<usize>(received), arrival);
            if (m_rng.unit() < m_impairment.duplicate) {
                ++duplicated;
                enqueue(!from_server, buffer, static_cast<usize>(received), arrival);
            }
        }

        const u64 now = now_us();
        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const Pending& a, const Pending& b) { return a.due_us < b.due_us; });
        usize sent = 0;
        for (; sent < m_queue.size() && m_queue[sent].due_us <= now; ++sent) {
            const Pending& item = m_queue[sent];
            if (!item.to_server && !m_has_client) {
                continue;
            }
            const sockaddr_in& dest = item.to_server ? m_server : m_client;
            u64& last = item.to_server ? m_last_arrival_to_server : m_last_arrival_to_client;
            if (item.arrival < last) {
                ++reordered;
            }
            last = std::max(last, item.arrival);
            (void)::sendto(m_fd, item.bytes.data(), item.bytes.size(), 0,
                           reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            ++forwarded;
        }
        m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(sent));
    }

    u64 forwarded = 0;
    u64 dropped = 0;
    u64 duplicated = 0;
    u64 reordered = 0;

private:
    struct Pending {
        u64 due_us = 0;
        u64 arrival = 0;
        bool to_server = false;
        std::vector<byte> bytes;
    };

    void enqueue(bool to_server, const byte* data, usize size, u64 arrival) {
        Pending item;
        const u64 delay_ms = m_impairment.max_delay_ms == 0 ? 0 : m_rng.range(0, m_impairment.max_delay_ms);
        item.due_us = now_us() + delay_ms * 1000ull;
        item.arrival = arrival;
        item.to_server = to_server;
        item.bytes.assign(data, data + size);
        m_queue.push_back(std::move(item));
    }

    int m_fd = -1;
    u16 m_port = 0;
    Impairment m_impairment{};
    Rng m_rng{1};
    sockaddr_in m_server{};
    sockaddr_in m_client{};
    bool m_has_client = false;
    u64 m_next_arrival = 0;
    u64 m_last_arrival_to_server = 0;
    u64 m_last_arrival_to_client = 0;
    std::vector<Pending> m_queue;
};

enum ChildExit : int {
    kChildOk = 0,
    kChildInitFailed = 10,
    kChildConnectFailed = 11,
    kChildCorrupt = 12,
    kChildOutOfOrder = 13,
    kChildTimeout = 14,
    kChildSendFailed = 15,
};

/// Client process: connect via the proxy, send `total` reliable messages, verify the echoes.
int run_echo_client(u16 proxy_port, u32 total) {
    ENetTransport client;
    client.set_timeouts(32, 5000, 20000);
    if (!client.init(0)) {
        return kChildInitFailed;
    }
    const u32 peer = client.connect_peer("127.0.0.1", proxy_port);
    const u64 connect_deadline = now_us() + 10'000'000ull;
    while (peer != 0 && !client.is_connected(peer) && now_us() < connect_deadline) {
        client.poll([](const Packet&) {});
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (peer == 0 || !client.is_connected(peer)) {
        return kChildConnectFailed;
    }

    for (u32 i = 0; i < total; ++i) {
        const std::vector<byte> message = make_message(i, total);
        if (!client.send(peer, message.data(), message.size(), PacketChannel::Reliable)) {
            return kChildSendFailed;
        }
    }

    u32 next_expected = 0;
    int status = kChildOk;
    const u64 deadline = now_us() + 40'000'000ull;
    while (next_expected < total && status == kChildOk && now_us() < deadline) {
        client.poll([&](const Packet& packet) {
            if (status != kChildOk) {
                return;
            }
            const u32 index = message_index(packet.data);
            if (index != next_expected) {
                status = kChildOutOfOrder;
                return;
            }
            if (packet.data != make_message(index, total)) {
                status = kChildCorrupt;
                return;
            }
            ++next_expected;
        });
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (status == kChildOk && next_expected < total) {
        status = kChildTimeout;
    }

    client.disconnect(peer);
    const u64 linger = now_us() + 300'000ull;
    while (now_us() < linger) {
        client.poll([](const Packet&) {});
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    client.destroy();
    return status;
}

void run_enet_two_process_case(const char* label, Impairment impairment, u32 total) {
    ENetTransport server;
    server.set_timeouts(32, 5000, 20000);
    expectTrue(server.init(0), "ENet server binds ephemeral port");
    const u16 server_port = server.bound_port();
    expectTrue(server_port != 0, "ENet server reports OS-assigned port");

    LossyUdpProxy proxy;
    expectTrue(proxy.open(server_port, impairment, 0xA11CEull + total), "impairment proxy opens");

    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = ::fork();
    if (child == 0) {
        ::_exit(run_echo_client(proxy.port(), total));
    }
    expectTrue(child > 0, "fork client process");
    if (child <= 0) {
        return;
    }

    u32 next_expected = 0;
    u32 out_of_order = 0;
    u32 corrupt = 0;
    u64 bytes = 0;
    int child_status = -1;
    bool child_done = false;
    const u64 start = now_us();
    const u64 deadline = start + 60'000'000ull;
    while (!child_done && now_us() < deadline) {
        server.poll([&](const Packet& packet) {
            const u32 index = message_index(packet.data);
            if (index != next_expected) {
                ++out_of_order;
            }
            if (index >= total || packet.data != make_message(index, total)) {
                ++corrupt;
            }
            bytes += packet.data.size();
            next_expected = index + 1;
            (void)server.send(packet.peer_id, packet.data.data(), packet.data.size(), PacketChannel::Reliable);
        });
        proxy.pump();
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) {
            child_done = true;
            child_status = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (!child_done) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
    }
    const f64 seconds = static_cast<f64>(now_us() - start) / 1e6;

    std::printf("  [enet %s] %u msgs %.1f KiB in %.2fs | proxy fwd=%llu drop=%llu dup=%llu reorder=%llu | "
                "server in-order=%u ooo=%u corrupt=%u | client exit=%d\n",
                label, total, static_cast<f64>(bytes) / 1024.0, seconds,
                static_cast<unsigned long long>(proxy.forwarded), static_cast<unsigned long long>(proxy.dropped),
                static_cast<unsigned long long>(proxy.duplicated), static_cast<unsigned long long>(proxy.reordered),
                next_expected, out_of_order, corrupt, child_status);

    expectTrue(child_done, "client process finished before deadline");
    expectTrue(child_status == kChildOk, "client process verified every echo in order, byte-exact");
    expectTrue(next_expected == total, "server received every reliable message");
    expectTrue(out_of_order == 0, "server saw reliable messages strictly in order");
    expectTrue(corrupt == 0, "server saw no corrupted payload");
    if (impairment.drop > 0.0) {
        expectTrue(proxy.dropped > 0 && proxy.reordered > 0 && proxy.duplicated > 0,
                   "impairment proxy actually dropped, duplicated and reordered datagrams");
    }
    server.destroy();
}

void run_enet_timeout_cases() {
    // Handshake timeout: connect to a bound UDP socket that never answers.
    {
        const int black_hole = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::bind(black_hole, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        socklen_t length = sizeof(local);
        (void)::getsockname(black_hole, reinterpret_cast<sockaddr*>(&local), &length);

        ENetTransport client;
        client.set_timeouts(4, 300, 1000);
        expectTrue(client.init(0), "timeout client init");
        bool saw_disconnect = false;
        bool saw_connect = false;
        client.set_connection_callback([&](u32, bool connected) {
            (connected ? saw_connect : saw_disconnect) = true;
        });
        const u64 start = now_us();
        const u32 peer = client.connect_peer("127.0.0.1", ntohs(local.sin_port));
        expectTrue(peer != 0, "connect request queued");
        while (!saw_disconnect && now_us() - start < 5'000'000ull) {
            client.poll([](const Packet&) {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const f64 elapsed_ms = static_cast<f64>(now_us() - start) / 1000.0;
        // ENet evaluates timeouts at retransmit boundaries; before any RTT sample the retransmit
        // timeout starts at 500 ms and doubles, so expiry lands on the first boundary past 1000 ms.
        std::printf("  [enet handshake timeout] failed after %.0f ms (policy max 1000 ms + 500 ms RTO granularity)\n",
                    elapsed_ms);
        expectTrue(!saw_connect, "no connection to a silent endpoint");
        expectTrue(saw_disconnect, "handshake failure reported as disconnect");
        expectTrue(elapsed_ms >= 1000.0 && elapsed_ms <= 2000.0, "handshake timeout honours configured policy");
        expectTrue(!client.is_connected(peer) && client.peer_count() == 0u, "timed-out peer forgotten");
        ::close(black_hole);
    }

    // Peer loss: server vanishes without a disconnect; client detects it within the policy.
    {
        ENetTransport server;
        ENetTransport client;
        expectTrue(server.init(0), "loss server init");
        expectTrue(client.init(0), "loss client init");
        client.set_timeouts(4, 300, 1000);
        const u32 peer = client.connect_peer("127.0.0.1", server.bound_port());
        const u64 connect_start = now_us();
        u32 server_side_peer = 0;
        server.set_connection_callback([&](u32 id, bool connected) {
            if (connected) {
                server_side_peer = id;
            }
        });
        while ((!client.is_connected(peer) || server_side_peer == 0) && now_us() - connect_start < 3'000'000ull) {
            server.poll([](const Packet&) {});
            client.poll([](const Packet&) {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const f64 handshake_ms = static_cast<f64>(now_us() - connect_start) / 1000.0;
        expectTrue(client.is_connected(peer) && server.is_connected(server_side_peer), "handshake completes");
        if (fuse::core::timingBudgetsEnforcedNoted()) {
            expectTrue(handshake_ms < 1000.0, "loopback handshake under 1 s");
        }

        // Graceful disconnect is observed promptly by the other side.
        bool server_saw_leave = false;
        server.set_connection_callback([&](u32, bool connected) {
            if (!connected) {
                server_saw_leave = true;
            }
        });

        // Warm up the RTT estimate with reliable echo traffic so retransmit timers reflect loopback.
        const u64 warm_end = now_us() + 400'000ull;
        u32 echoes = 0;
        while (now_us() < warm_end) {
            const byte probe = 1;
            (void)client.send(peer, &probe, 1, PacketChannel::Reliable);
            server.poll([&](const Packet& packet) {
                (void)server.send(packet.peer_id, packet.data.data(), packet.data.size(), PacketChannel::Reliable);
            });
            client.poll([&](const Packet&) { ++echoes; });
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const u32 rtt_ms = client.ping_ms(peer);
        expectTrue(echoes > 50u, "reliable echo traffic flowed during warm-up");

        server.destroy(); // Hard stop: no disconnect packet sent.
        bool client_saw_loss = false;
        client.set_connection_callback([&](u32, bool connected) {
            if (!connected) {
                client_saw_loss = true;
            }
        });
        const byte ping = 7;
        (void)client.send(peer, &ping, 1, PacketChannel::Reliable);
        const u64 loss_start = now_us();
        while (!client_saw_loss && now_us() - loss_start < 5'000'000ull) {
            client.poll([](const Packet&) {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const f64 detect_ms = static_cast<f64>(now_us() - loss_start) / 1000.0;
        std::printf("  [enet peer loss] handshake %.1f ms, rtt %u ms, silent peer detected after %.0f ms "
                    "(policy min 300 / max 1000 ms)\n",
                    handshake_ms, rtt_ms, detect_ms);
        expectTrue(client_saw_loss, "client detects vanished server");
        expectTrue(detect_ms >= 250.0 && detect_ms <= 1500.0, "peer-loss detection honours timeout policy");
        (void)server_saw_leave;
    }

    // Graceful disconnect notifies the remote side.
    {
        ENetTransport server;
        ENetTransport client;
        expectTrue(server.init(0) && client.init(0), "graceful pair init");
        bool server_saw_leave = false;
        server.set_connection_callback([&](u32, bool connected) {
            if (!connected) {
                server_saw_leave = true;
            }
        });
        const u32 peer = client.connect_peer("127.0.0.1", server.bound_port());
        const u64 start = now_us();
        while (!client.is_connected(peer) && now_us() - start < 3'000'000ull) {
            server.poll([](const Packet&) {});
            client.poll([](const Packet&) {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        client.disconnect(peer);
        const u64 leave_start = now_us();
        while (!server_saw_leave && now_us() - leave_start < 3'000'000ull) {
            server.poll([](const Packet&) {});
            client.poll([](const Packet&) {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const f64 leave_ms = static_cast<f64>(now_us() - leave_start) / 1000.0;
        std::printf("  [enet graceful disconnect] observed by server after %.1f ms\n", leave_ms);
        expectTrue(server_saw_leave && server.peer_count() == 0u, "graceful disconnect reaches server");
        if (fuse::core::timingBudgetsEnforcedNoted()) {
            expectTrue(leave_ms < 500.0, "graceful disconnect observed within 500 ms");
        }
    }
}

void run_enet_unreliable_sequenced_case() {
    // UnreliableSeq: receiver never sees an older packet after a newer one (newest wins).
    ENetTransport server;
    ENetTransport client;
    expectTrue(server.init(0) && client.init(0), "sequenced pair init");
    LossyUdpProxy proxy;
    expectTrue(proxy.open(server.bound_port(), Impairment{0.1, 0.1, 40}, 0x5E0ull), "sequenced proxy");
    client.set_timeouts(32, 5000, 20000);
    const u32 peer = client.connect_peer("127.0.0.1", proxy.port());
    const u64 start = now_us();
    while (!client.is_connected(peer) && now_us() - start < 10'000'000ull) {
        server.poll([](const Packet&) {});
        client.poll([](const Packet&) {});
        proxy.pump();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    expectTrue(client.is_connected(peer), "sequenced client connected through proxy");

    u32 received = 0;
    u32 regressions = 0;
    u32 last = 0;
    std::set<u32> seen;
    u32 duplicates = 0;
    const u64 stream_end = now_us() + 1'500'000ull;
    u32 counter = 1;
    while (now_us() < stream_end) {
        if (counter <= 400) {
            (void)client.send(peer, reinterpret_cast<const byte*>(&counter), sizeof(counter),
                              PacketChannel::UnreliableSeq);
            ++counter;
        }
        client.poll([](const Packet&) {});
        proxy.pump();
        server.poll([&](const Packet& packet) {
            u32 value = 0;
            std::memcpy(&value, packet.data.data(), sizeof(value));
            ++received;
            if (value <= last) {
                ++regressions;
            }
            if (!seen.insert(value).second) {
                ++duplicates;
            }
            last = std::max(last, value);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
    std::printf("  [enet unreliable-seq] sent 400, delivered %u, newest %u, regressions %u, duplicates %u "
                "(proxy drop=%llu dup=%llu reorder=%llu)\n",
                received, last, regressions, duplicates, static_cast<unsigned long long>(proxy.dropped),
                static_cast<unsigned long long>(proxy.duplicated), static_cast<unsigned long long>(proxy.reordered));
    // Newest-wins drops anything overtaken in flight, so heavy reordering lowers delivery by design.
    expectTrue(received >= 20u, "sequenced packets delivered through impairment");
    expectTrue(last >= 380u, "newest sequenced data reaches the receiver");
    expectTrue(regressions == 0u && duplicates == 0u, "sequenced channel never delivers stale or duplicate data");
}

#endif // FUSE_NET_GATES_POSIX && FUSE_NET_HAS_ENET

void run_transport_gates() {
#if defined(FUSE_NET_GATES_POSIX) && defined(FUSE_NET_HAS_ENET)
    std::printf("[gate] ENet reliable transport between two local processes\n");
    run_enet_two_process_case("clean", Impairment{}, 300);
    run_enet_two_process_case("10% loss, 5% dup, 0-30 ms jitter", Impairment{0.10, 0.05, 30}, 300);
    run_enet_timeout_cases();
    run_enet_unreliable_sequenced_case();
#else
    std::printf("[gate] ENet two-process transport: SKIPPED (needs POSIX + FUSE_NET_HAS_ENET)\n");
#endif
}

// ---------------------------------------------------------------------------------------------
// Rollback fixtures
// ---------------------------------------------------------------------------------------------

struct BodyRecord {
    ecs::vec3 position;
    ecs::quat rotation;
    ecs::vec3 scale;
    ecs::vec3 velocity;
    ecs::vec3 angular_velocity;
    f32 mass;
    f32 inv_mass;
};

std::vector<ecs::EntityID> populate_world(ecs::Registry& registry, u32 count, u64 seed) {
    Rng rng(seed);
    std::vector<ecs::EntityID> entities;
    for (u32 i = 0; i < count; ++i) {
        const ecs::EntityID entity = registry.create();
        ecs::Transform transform{};
        transform.position = {rng.real(-500.f, 500.f), rng.real(-50.f, 50.f), rng.real(-500.f, 500.f), 1.f};
        const f32 angle = rng.real(-3.f, 3.f);
        transform.rotation = {0.f, std::sin(angle * 0.5f), 0.f, std::cos(angle * 0.5f)};
        transform.scale = {rng.real(0.5f, 2.f), rng.real(0.5f, 2.f), rng.real(0.5f, 2.f), 0.f};
        if (i == 3) {
            transform.position.x = -0.f; // Sign of zero must survive.
        }
        ecs::RigidBody body{};
        body.velocity = {rng.real(-5.f, 5.f), rng.real(-5.f, 5.f), rng.real(-5.f, 5.f), 0.f};
        body.angular_velocity = {rng.real(-1.f, 1.f), rng.real(-1.f, 1.f), rng.real(-1.f, 1.f), 0.f};
        body.mass = rng.real(0.5f, 50.f);
        body.inv_mass = 1.f / body.mass;
        if (i % 7 == 0) {
            body.is_static = true; // Static: finite mass but inv_mass 0 — restore must not "fix" it.
            body.inv_mass = 0.f;
        }
        registry.add<ecs::Transform>(entity, transform);
        registry.add<ecs::RigidBody>(entity, body);
        entities.push_back(entity);
    }
    return entities;
}

std::vector<BodyRecord> record_bodies(ecs::Registry& registry, const std::vector<ecs::EntityID>& entities) {
    std::vector<BodyRecord> out;
    for (const ecs::EntityID entity : entities) {
        const ecs::Transform* t = registry.get<ecs::Transform>(entity);
        const ecs::RigidBody* b = registry.get<ecs::RigidBody>(entity);
        out.push_back({t->position, t->rotation, t->scale, b->velocity, b->angular_velocity, b->mass, b->inv_mass});
    }
    return out;
}

bool bodies_identical(const std::vector<BodyRecord>& a, const std::vector<BodyRecord>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        if (!vec_bits_equal(a[i].position, b[i].position) || !quat_bits_equal(a[i].rotation, b[i].rotation) ||
            !vec_bits_equal(a[i].scale, b[i].scale) || !vec_bits_equal(a[i].velocity, b[i].velocity) ||
            !vec_bits_equal(a[i].angular_velocity, b[i].angular_velocity) || !bits_equal(a[i].mass, b[i].mass) ||
            !bits_equal(a[i].inv_mass, b[i].inv_mass)) {
            return false;
        }
    }
    return true;
}

PlayerInput make_input(u32 frame, Rng& rng) {
    PlayerInput input{};
    input.frame = frame;
    input.player_id = 1;
    input.axis_lx = static_cast<std::int16_t>(static_cast<i32>(rng.range(0, 65534)) - 32767);
    input.axis_ly = static_cast<std::int16_t>(static_cast<i32>(rng.range(0, 65534)) - 32767);
    input.axis_rx = static_cast<std::int16_t>(static_cast<i32>(rng.range(0, 65534)) - 32767);
    input.axis_ry = static_cast<std::int16_t>(static_cast<i32>(rng.range(0, 65534)) - 32767);
    return input;
}

/// Hand-written reference integrator (independent of RollbackManager) — same float operations.
void reference_integrate(std::vector<BodyRecord>& bodies, const std::vector<bool>& is_static, const PlayerInput& input,
                         f32 dt) {
    const f32 dx = static_cast<f32>(input.axis_lx) / 32767.f * dt;
    const f32 dy = static_cast<f32>(input.axis_ly) / 32767.f * dt;
    for (usize i = 0; i < bodies.size(); ++i) {
        bodies[i].position.x += dx;
        bodies[i].position.y += dy;
        if (!is_static[i]) {
            bodies[i].velocity.x += static_cast<f32>(input.axis_rx) / 32767.f * dt;
            bodies[i].velocity.y += static_cast<f32>(input.axis_ry) / 32767.f * dt;
        }
    }
}

std::vector<byte> capture_world_bytes(ecs::Registry& registry) {
    RollbackManager probe;
    probe.init(8);
    probe.bind_registry(&registry);
    probe.save_snapshot(0);
    const GameSnapshot* snapshot = probe.buffer().snapshot(0);
    std::vector<byte> bytes = snapshot->ecs_state;
    bytes.insert(bytes.end(), snapshot->physics_state.begin(), snapshot->physics_state.end());
    return bytes;
}

// ---------------------------------------------------------------------------------------------
// Row: Rollback snapshot captures and restores rigid body positions — byte-identical round-trip
// ---------------------------------------------------------------------------------------------

void run_rollback_snapshot_roundtrip_gate() {
    std::printf("[gate] rollback snapshot capture/restore byte-identical\n");
    ecs::Registry registry;
    registry.init(256);
    const std::vector<ecs::EntityID> entities = populate_world(registry, 48, 0xB0D1E5ull);
    const std::vector<BodyRecord> reference = record_bodies(registry, entities);
    const std::vector<byte> reference_bytes = capture_world_bytes(registry);

    RollbackManager rollback;
    rollback.init(8);
    rollback.bind_registry(&registry);
    Rng rng(42);
    constexpr f32 dt = 1.f / 60.f;
    for (u32 frame = 0; frame < 6; ++frame) {
        rollback.set_local_input(make_input(frame, rng));
        rollback.tick(dt);
    }
    // Also mutate state outside the simulation (mass edits etc.) before restoring.
    registry.get<ecs::RigidBody>(entities[1])->mass = 999.f;
    registry.get<ecs::Transform>(entities[2])->scale.x = 7.f;
    expectTrue(!bodies_identical(record_bodies(registry, entities), reference), "world diverged after 6 frames");

    expectTrue(rollback.rewind_to(0), "rewind to frame 0 accepted");
    const std::vector<BodyRecord> restored = record_bodies(registry, entities);
    u32 mismatched = 0;
    for (usize i = 0; i < restored.size(); ++i) {
        mismatched += bodies_identical({restored[i]}, {reference[i]}) ? 0u : 1u;
    }
    std::printf("  48 bodies restored, %u field mismatches (bitwise incl. w lanes, -0.0, static inv_mass)\n",
                mismatched);
    expectTrue(mismatched == 0, "every rigid body restored bit-for-bit (position/rotation/scale/vel/mass)");
    expectTrue(capture_world_bytes(registry) == reference_bytes, "re-captured snapshot bytes identical");

    const GameSnapshot* frame0 = rollback.buffer().snapshot(0);
    expectTrue(frame0 != nullptr && verify_snapshot_checksum(*frame0), "stored snapshot checksum verifies");
    rollback.destroy();
    registry.destroy();
}

// ---------------------------------------------------------------------------------------------
// Row: Rollback resimulates 4 frames after remote input arrives — matches non-rollback reference
// ---------------------------------------------------------------------------------------------

void run_rollback_resim_gate() {
    std::printf("[gate] rollback resimulation vs non-rollback reference\n");
    constexpr f32 dt = 1.f / 60.f;
    constexpr u32 kEntities = 12;

    // Case 1: exactly 4 frames of resimulation.
    {
        ecs::Registry rolled;
        ecs::Registry reference;
        rolled.init(64);
        reference.init(64);
        const auto rolled_entities = populate_world(rolled, kEntities, 7);
        (void)populate_world(reference, kEntities, 7);

        RollbackManager rollback;
        rollback.init(8);
        rollback.bind_registry(&rolled);
        RollbackManager straight;
        straight.init(8);
        straight.bind_registry(&reference);

        Rng rng(99);
        std::vector<PlayerInput> truth;
        for (u32 f = 0; f < 10; ++f) {
            truth.push_back(make_input(f, rng));
        }
        PlayerInput predicted = truth[5];
        for (u32 f = 0; f < 10; ++f) {
            // Frames 6..9 are predicted by repeating frame 5 (GGPO-style); truth differs.
            rollback.set_local_input(f <= 5 ? truth[f] : PlayerInput{f, 1, predicted.buttons, predicted.axis_lx,
                                                                      predicted.axis_ly, predicted.axis_rx,
                                                                      predicted.axis_ry});
            rollback.tick(dt);
        }
        for (u32 f = 0; f < 10; ++f) {
            straight.set_local_input(truth[f]);
            straight.tick(dt);
        }
        expectTrue(capture_world_bytes(rolled) != capture_world_bytes(reference), "misprediction diverges state");

        bool all_rolled = true;
        for (u32 f = 6; f < 10; ++f) {
            // Remote confirmations for 6..9 arrive at frame 10 — first one resimulates 4 frames.
            if (f == 6) {
                expectTrue(rollback.resimulate_count_to(6) == 0u && rollback.current_frame() - 6u == 4u,
                           "rollback depth for first confirmation is 4 frames");
            }
            all_rolled = rollback.apply_remote_input(truth[f]) && all_rolled;
        }
        expectTrue(all_rolled, "late remote inputs trigger rollback");
        expectTrue(rollback.current_frame() == 10u, "resimulation returns to the present frame");
        const bool identical = capture_world_bytes(rolled) == capture_world_bytes(reference);
        expectTrue(identical, "4-frame resimulation byte-identical to non-rollback reference");

        u32 checksum_mismatch = 0;
        for (u32 f = 0; f < 10; ++f) {
            checksum_mismatch += rollback.local_checksum(f) == straight.local_checksum(f) ? 0u : 1u;
        }
        expectTrue(checksum_mismatch == 0u, "per-frame snapshot checksums match reference after resim");

        // Independent reference: hand-written integrator.
        std::vector<BodyRecord> expected;
        {
            ecs::Registry initial;
            initial.init(64);
            const auto ids = populate_world(initial, kEntities, 7);
            expected = record_bodies(initial, ids);
            initial.destroy();
        }
        std::vector<bool> is_static;
        for (u32 i = 0; i < kEntities; ++i) {
            is_static.push_back(i % 7 == 0);
        }
        for (u32 f = 0; f < 10; ++f) {
            reference_integrate(expected, is_static, truth[f], dt);
        }
        expectTrue(bodies_identical(record_bodies(rolled, rolled_entities), expected),
                   "resimulated bodies bit-identical to hand-written reference integrator");
        std::printf("  4-frame resim: byte-identical=%s, frame-checksum mismatches=%u\n", identical ? "yes" : "no",
                    checksum_mismatch);
        rollback.destroy();
        straight.destroy();
        rolled.destroy();
        reference.destroy();
    }

    // Case 2: 240-frame session (ring wraps 64-slot buffer 3x), each remote input arrives 4 frames late.
    {
        ecs::Registry rolled;
        ecs::Registry reference;
        rolled.init(64);
        reference.init(64);
        (void)populate_world(rolled, kEntities, 11);
        (void)populate_world(reference, kEntities, 11);
        RollbackManager rollback;
        rollback.init(8);
        rollback.bind_registry(&rolled);
        RollbackManager straight;
        straight.init(8);
        straight.bind_registry(&reference);

        Rng rng(1234);
        std::vector<PlayerInput> truth;
        constexpr u32 kFrames = 240;
        constexpr u32 kLatency = 4;
        for (u32 f = 0; f < kFrames; ++f) {
            // Inputs change every 3 frames so prediction (repeat last confirmed) is sometimes right.
            truth.push_back(f % 3 == 0 || f == 0 ? make_input(f, rng) : PlayerInput{f, 1, 0, truth.back().axis_lx,
                                                                                   truth.back().axis_ly,
                                                                                   truth.back().axis_rx,
                                                                                   truth.back().axis_ry});
        }
        u32 rollbacks = 0;
        u32 frame_checksum_mismatch = 0;
        for (u32 f = 0; f < kFrames; ++f) {
            PlayerInput prediction = f >= kLatency ? truth[f - kLatency] : PlayerInput{};
            prediction.frame = f;
            rollback.set_local_input(prediction);
            rollback.tick(dt);
            if (f + 1 >= kLatency) {
                rollbacks += rollback.apply_remote_input(truth[f + 1 - kLatency]) ? 1u : 0u;
            }
            straight.set_local_input(truth[f]);
            straight.tick(dt);
            // Frames older than the latency window are fully confirmed: checksums must agree.
            if (f >= kLatency + 1) {
                const u32 confirmed = f + 1 - kLatency - 1;
                if (rollback.local_checksum(confirmed + 1) != straight.local_checksum(confirmed + 1)) {
                    ++frame_checksum_mismatch;
                }
            }
        }
        for (u32 f = kFrames + 1 - kLatency; f < kFrames; ++f) {
            rollbacks += rollback.apply_remote_input(truth[f]) ? 1u : 0u;
        }
        const bool identical = capture_world_bytes(rolled) == capture_world_bytes(reference);
        std::printf("  240-frame session: %u rollbacks, confirmed-frame checksum mismatches=%u, final identical=%s\n",
                    rollbacks, frame_checksum_mismatch, identical ? "yes" : "no");
        expectTrue(frame_checksum_mismatch == 0u, "every confirmed frame matches reference across ring wrap");
        expectTrue(identical, "240-frame rollback session byte-identical to reference");
        rollback.destroy();
        straight.destroy();
        rolled.destroy();
        reference.destroy();
    }
}

// ---------------------------------------------------------------------------------------------
// Row: Desync detection flags checksum mismatch when physics state diverges
// ---------------------------------------------------------------------------------------------

void run_desync_gate() {
    std::printf("[gate] desync detection via per-frame checksums\n");
    constexpr f32 dt = 1.f / 60.f;
    ecs::Registry a;
    ecs::Registry b;
    a.init(64);
    b.init(64);
    (void)populate_world(a, 16, 5);
    const auto b_entities = populate_world(b, 16, 5);
    RollbackManager peer_a;
    RollbackManager peer_b;
    peer_a.init(8);
    peer_b.init(8);
    peer_a.bind_registry(&a);
    peer_b.bind_registry(&b);

    Rng rng(77);
    constexpr u32 kDivergeAfter = 20;
    u32 false_positives = 0;
    u32 detected_frame = std::numeric_limits<u32>::max();
    for (u32 f = 0; f < 30; ++f) {
        const PlayerInput input = make_input(f, rng);
        peer_a.set_local_input(input);
        peer_b.set_local_input(input);
        if (f == kDivergeAfter) {
            // Physics-only divergence: one ULP on a single body's velocity (positions unaffected).
            ecs::RigidBody* body = b.get<ecs::RigidBody>(b_entities[9]);
            body->angular_velocity.z = std::nextafter(body->angular_velocity.z, 1e9f);
        }
        peer_a.tick(dt);
        peer_b.tick(dt);
        const DesyncCheck check = peer_a.check_remote_checksum(f, peer_b.local_checksum(f));
        if (f < kDivergeAfter && check != DesyncCheck::Match) {
            ++false_positives;
        }
        if (check == DesyncCheck::Mismatch && detected_frame == std::numeric_limits<u32>::max()) {
            detected_frame = f;
        }
    }
    std::printf("  divergence injected before frame %u, first mismatch at frame %u, false positives %u\n",
                kDivergeAfter, detected_frame, false_positives);
    expectTrue(false_positives == 0u, "identical simulations never flag desync");
    expectTrue(detected_frame == kDivergeAfter, "desync flagged on the first diverged frame");
    expectTrue(peer_a.desync_detected() && peer_a.first_desync_frame() == kDivergeAfter,
               "desync latch records first diverged frame");
    expectTrue(peer_a.check_remote_checksum(1000, 1) == DesyncCheck::Unknown, "unretained frame reports unknown");
    peer_a.destroy();
    peer_b.destroy();
    a.destroy();
    b.destroy();
}

// ---------------------------------------------------------------------------------------------
// Row: ClientInterpolator smoothly interpolates entity position over 3 states — no discontinuity
// ---------------------------------------------------------------------------------------------

struct InterpolationStats {
    f64 max_position_error = 0.0;
    f64 max_step = 0.0;
    f64 max_angle_error = 0.0;
    f64 max_norm_error = 0.0;
};

InterpolationStats run_interpolation_sim(u32 state_count, u32 interval_ms, u32 delay_ms, u32 base_latency_ms,
                                         u32 jitter_ms, f64 loss, f64 duplicate, u64 seed, u32 sample_from_ms,
                                         u32 sample_to_ms, usize* max_buffered) {
    constexpr f64 kVelocity = 3.0;      // m/s along x
    constexpr f64 kAngularRate = 1.0;   // rad/s about z
    ecs::Registry registry;
    registry.init(8);
    const ecs::EntityID entity = registry.create();
    registry.add<ecs::Transform>(entity);

    struct InFlight {
        u64 arrival_us;
        EntityNetState state;
    };
    Rng rng(seed);
    std::vector<InFlight> network;
    for (u32 i = 0; i < state_count; ++i) {
        EntityNetState state{};
        state.entity = entity;
        state.sequence = i + 1;
        state.timestamp = static_cast<u64>(i) * interval_ms * 1000ull;
        const f64 t = static_cast<f64>(state.timestamp) / 1e6;
        state.position = {static_cast<f32>(kVelocity * t), 2.f, -1.f, 1.f};
        const f64 half = 0.5 * kAngularRate * t;
        state.rotation = {0.f, 0.f, static_cast<f32>(std::sin(half)), static_cast<f32>(std::cos(half))};
        // The first and last state always arrive so the sampled window stays defined.
        if (i != 0 && i + 1 != state_count && rng.unit() < loss) {
            continue;
        }
        const u64 latency = static_cast<u64>(base_latency_ms + (jitter_ms ? rng.range(0, jitter_ms) : 0)) * 1000ull;
        network.push_back({state.timestamp + latency, state});
        if (rng.unit() < duplicate) {
            network.push_back({state.timestamp + latency + rng.range(0, 30) * 1000ull, state});
        }
    }
    std::sort(network.begin(), network.end(),
              [](const InFlight& x, const InFlight& y) { return x.arrival_us < y.arrival_us; });

    ClientInterpolator interpolator;
    interpolator.set_interpolation_delay_ms(delay_ms);
    InterpolationStats stats;
    usize delivered = 0;
    bool have_previous = false;
    f64 previous_x = 0.0;
    const f64 last_ts = static_cast<f64>(state_count - 1) * interval_ms / 1000.0;
    for (u64 now = 0; now <= static_cast<u64>(sample_to_ms) * 1000ull; now += 1000ull) {
        while (delivered < network.size() && network[delivered].arrival_us <= now) {
            interpolator.receive_state(network[delivered].state);
            ++delivered;
        }
        interpolator.update(registry, now);
        if (max_buffered != nullptr) {
            *max_buffered = std::max(*max_buffered, interpolator.buffered_state_count(entity));
        }
        if (now < static_cast<u64>(sample_from_ms) * 1000ull) {
            continue;
        }
        const ecs::Transform* transform = registry.get<ecs::Transform>(entity);
        const f64 render_s =
            std::clamp((static_cast<f64>(now) - static_cast<f64>(delay_ms) * 1000.0) / 1e6, 0.0, last_ts);
        const f64 x = transform->position.x;
        if (std::getenv("FUSE_NET_GATES_DEBUG") != nullptr && std::fabs(x - kVelocity * render_s) > 1e-3) {
            std::printf("    t=%llu render=%.4f x=%.5f ref=%.5f buffered=%zu\n", static_cast<unsigned long long>(now),
                        render_s, x, kVelocity * render_s, interpolator.buffered_state_count(entity));
        }
        stats.max_position_error = std::max(stats.max_position_error, std::fabs(x - kVelocity * render_s));
        if (have_previous) {
            stats.max_step = std::max(stats.max_step, std::fabs(x - previous_x));
        }
        previous_x = x;
        have_previous = true;
        const ecs::quat q = transform->rotation;
        const f64 norm = std::sqrt(static_cast<f64>(q.x) * q.x + static_cast<f64>(q.y) * q.y +
                                   static_cast<f64>(q.z) * q.z + static_cast<f64>(q.w) * q.w);
        stats.max_norm_error = std::max(stats.max_norm_error, std::fabs(norm - 1.0));
        const f64 angle = 2.0 * std::atan2(static_cast<f64>(q.z), static_cast<f64>(q.w));
        stats.max_angle_error = std::max(stats.max_angle_error, std::fabs(angle - kAngularRate * render_s));
    }
    registry.destroy();
    return stats;
}

void run_interpolation_gate() {
    std::printf("[gate] ClientInterpolator continuity\n");
    constexpr f64 kVelocityPerMs = 3.0 / 1000.0;

    // Exactly 3 states (0/50/100 ms), 100 ms delay, 20 ms latency; render every 1 ms.
    const InterpolationStats three =
        run_interpolation_sim(3, 50, 100, 20, 0, 0.0, 0.0, 1, 0, 260, nullptr);
    std::printf("  3 states: max |x - ref| = %.2e m, max 1 ms step = %.2e m (bound %.2e), angle err %.2e rad, "
                "|q| err %.2e\n",
                three.max_position_error, three.max_step, kVelocityPerMs, three.max_angle_error,
                three.max_norm_error);
    expectTrue(three.max_position_error < 1e-4, "3-state interpolation tracks reference trajectory");
    expectTrue(three.max_step <= kVelocityPerMs * 1.01 + 1e-6, "3-state interpolation has no discontinuity");
    expectTrue(three.max_angle_error < 1e-4 && three.max_norm_error < 1e-5, "rotation slerp exact and unit length");

    // 3 s stream at 20 Hz with 30-70 ms jitter (reorders), 5% loss, 10% duplicates. The delay
    // (180 ms) covers one lost snapshot: 2 x 50 ms interval + 70 ms worst-case latency.
    usize max_buffered = 0;
    const InterpolationStats stream =
        run_interpolation_sim(61, 50, 180, 30, 40, 0.05, 0.10, 2024, 250, 3200, &max_buffered);
    std::printf("  jittery stream: max |x - ref| = %.2e m, max step = %.2e m, angle err %.2e, max buffered %zu\n",
                stream.max_position_error, stream.max_step, stream.max_angle_error, max_buffered);
    expectTrue(stream.max_position_error < 1e-4, "jittered/reordered/lossy stream tracks reference");
    expectTrue(stream.max_step <= kVelocityPerMs * 1.01 + 1e-6, "jittered stream has no discontinuity");
    expectTrue(max_buffered <= ClientInterpolator::kMaxBufferedStates, "jitter buffer stays bounded");
}

// ---------------------------------------------------------------------------------------------
// Snapshot delta compression: exact round-trip, bandwidth, and hostile input
// ---------------------------------------------------------------------------------------------

struct SimEntity {
    u32 index;
    u32 generation;
    f32 ecs[10];
    f32 phys[7];
};

GameSnapshot encode_world(u32 frame, const std::vector<SimEntity>& world) {
    GameSnapshot snapshot;
    snapshot.frame = frame;
    NetSerializer ecs_writer;
    NetSerializer physics_writer;
    for (const SimEntity& e : world) {
        ecs_writer.write_u32(e.index);
        ecs_writer.write_u32(e.generation);
        for (f32 v : e.ecs) {
            ecs_writer.write_f32(v);
        }
        physics_writer.write_u32(e.index);
        physics_writer.write_u32(e.generation);
        for (f32 v : e.phys) {
            physics_writer.write_f32(v);
        }
    }
    snapshot.ecs_state = std::move(ecs_writer.buffer);
    snapshot.physics_state = std::move(physics_writer.buffer);
    snapshot.checksum = compute_snapshot_checksum(snapshot);
    return snapshot;
}

f32 random_value(Rng& rng) {
    switch (rng.range(0, 9)) {
    case 0:
        return -0.f;
    case 1:
        return 0.f;
    case 2: {
        const u32 nan_bits = 0x7FC00000u | rng.range(1, 0xFFFF);
        f32 value;
        std::memcpy(&value, &nan_bits, sizeof(value));
        return value;
    }
    default:
        return rng.real(-1000.f, 1000.f);
    }
}

SimEntity random_entity(u32 index, Rng& rng) {
    SimEntity e{};
    e.index = index;
    e.generation = rng.range(0, 3);
    for (f32& v : e.ecs) {
        v = random_value(rng);
    }
    for (f32& v : e.phys) {
        v = random_value(rng);
    }
    return e;
}

void run_snapshot_delta_gates() {
    std::printf("[gate] snapshot delta compression round-trip exactness + bandwidth\n");
    Rng rng(0xDE17Aull);
    u32 failures = 0;
    u32 patches = 0;
    u32 fulls = 0;
    u32 nones = 0;
    constexpr u32 kIterations = 3000;
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        std::vector<SimEntity> base;
        std::vector<u32> free_indices;
        for (u32 i = 0; i < 64; ++i) {
            free_indices.push_back(i);
        }
        const u32 count = rng.range(1, 48);
        for (u32 i = 0; i < count; ++i) {
            const u32 pick = rng.range(0, static_cast<u32>(free_indices.size() - 1));
            base.push_back(random_entity(free_indices[pick], rng));
            free_indices.erase(free_indices.begin() + pick);
        }
        std::vector<SimEntity> target = base;
        const u32 mode = rng.range(0, 9);
        if (mode <= 5) {
            // Mutate a few fields (the common case).
            const u32 edits = rng.range(0, 4);
            for (u32 e = 0; e < edits; ++e) {
                SimEntity& victim = target[rng.range(0, static_cast<u32>(target.size() - 1))];
                if (rng.range(0, 1) == 0) {
                    victim.ecs[rng.range(0, 9)] = random_value(rng);
                } else {
                    victim.phys[rng.range(0, 6)] = random_value(rng);
                }
            }
        } else if (mode == 6 && !free_indices.empty()) {
            target.push_back(random_entity(free_indices.front(), rng)); // Spawn.
        } else if (mode == 7 && target.size() > 1) {
            target.erase(target.begin() + rng.range(0, static_cast<u32>(target.size() - 1))); // Despawn.
        } else if (mode == 8 && target.size() > 1) {
            std::swap(target.front(), target.back()); // Reordered rows.
        } else {
            target[0].ecs[0] = std::nextafter(target[0].ecs[0], 1e9f);
        }

        const GameSnapshot base_snapshot = encode_world(iteration, base);
        const GameSnapshot target_snapshot = encode_world(iteration + 1, target);
        const SnapshotDelta delta = compute_snapshot_delta(base_snapshot, target_snapshot);
        NetSerializer wire;
        serialize_snapshot_delta(delta, wire);
        wire.reset_read();
        const SnapshotDelta decoded = deserialize_snapshot_delta(wire);
        const DeltaApplyResult applied = apply_snapshot_delta_verified(base_snapshot, decoded);
        const bool exact = applied.snapshot.ecs_state == target_snapshot.ecs_state &&
                           applied.snapshot.physics_state == target_snapshot.physics_state &&
                           applied.snapshot.frame == target_snapshot.frame && applied.target_checksum_ok &&
                           applied.base_checksum_ok;
        failures += exact ? 0u : 1u;
        patches += delta.kind == SnapshotDeltaKind::EntityPatch ? 1u : 0u;
        fulls += delta.kind == SnapshotDeltaKind::Full ? 1u : 0u;
        nones += delta.kind == SnapshotDeltaKind::None ? 1u : 0u;
    }
    std::printf("  %u random deltas (-0.0/NaN/spawn/despawn/reorder): %u inexact | patch=%u full=%u none=%u\n",
                kIterations, failures, patches, fulls, nones);
    expectTrue(failures == 0u, "every delta round-trips byte-exact through the wire format");
    expectTrue(patches > kIterations / 3, "most small changes encode as entity patches");

    // Bandwidth: 64 bodies, 20 Hz, 3 bodies moving per tick.
    std::vector<SimEntity> world;
    for (u32 i = 0; i < 64; ++i) {
        world.push_back(random_entity(i, rng));
        for (f32& v : world.back().ecs) {
            v = rng.real(-100.f, 100.f);
        }
        for (f32& v : world.back().phys) {
            v = rng.real(-100.f, 100.f);
        }
    }
    usize full_wire = 0;
    usize delta_wire = 0;
    usize idle_wire = 0;
    u32 bandwidth_failures = 0;
    GameSnapshot previous = encode_world(0, world);
    for (u32 tick = 1; tick <= 100; ++tick) {
        for (u32 m = 0; m < 3; ++m) {
            SimEntity& mover = world[(tick * 7 + m * 13) % 64];
            mover.ecs[0] += 0.1f;
            mover.ecs[2] -= 0.05f;
            mover.phys[0] = static_cast<f32>(tick);
        }
        const GameSnapshot current = encode_world(tick, world);
        NetSerializer full_writer;
        SnapshotDelta full{};
        full.kind = SnapshotDeltaKind::Full;
        full.target_frame = tick;
        full.full_ecs_state = current.ecs_state;
        full.full_physics_state = current.physics_state;
        serialize_snapshot_delta(full, full_writer);
        full_wire += full_writer.buffer.size();

        NetSerializer delta_writer;
        const SnapshotDelta delta = compute_snapshot_delta(previous, current);
        serialize_snapshot_delta(delta, delta_writer);
        delta_wire += delta_writer.buffer.size();
        const GameSnapshot rebuilt = apply_snapshot_delta(previous, delta);
        bandwidth_failures += rebuilt.ecs_state == current.ecs_state && rebuilt.physics_state == current.physics_state
                                  ? 0u
                                  : 1u;
        previous = current;
    }
    {
        GameSnapshot idle = previous;
        idle.frame += 1;
        NetSerializer idle_writer;
        serialize_snapshot_delta(compute_snapshot_delta(previous, idle), idle_writer);
        idle_wire = idle_writer.buffer.size();
    }
    const f64 ratio = static_cast<f64>(delta_wire) / static_cast<f64>(full_wire);
    std::printf("  64 bodies, 3 moving/tick: full %.0f B/tick, delta %.0f B/tick (%.1f%%), idle tick %zu B; "
                "20 Hz delta = %.2f kbit/s\n",
                static_cast<f64>(full_wire) / 100.0, static_cast<f64>(delta_wire) / 100.0, ratio * 100.0, idle_wire,
                static_cast<f64>(delta_wire) / 100.0 * 20.0 * 8.0 / 1000.0);
    expectTrue(bandwidth_failures == 0u, "bandwidth run reconstructs every tick exactly");
    expectTrue(ratio < 0.10, "entity-patch deltas use < 10% of full-snapshot bandwidth");
    expectTrue(idle_wire <= 64u, "unchanged world across frames sends only a header");

    // Hostile wire input: absurd lengths must not allocate or crash.
    {
        NetSerializer hostile;
        hostile.write_u8(static_cast<u8>(SnapshotDeltaKind::Full));
        for (int i = 0; i < 8; ++i) {
            hostile.write_u32(0);
        }
        hostile.write_u32(0xFFFFFFFFu);
        hostile.write_u32(0xFFFFFFFFu);
        hostile.reset_read();
        const SnapshotDelta decoded = deserialize_snapshot_delta(hostile);
        expectTrue(decoded.full_ecs_state.size() <= 64u && decoded.full_physics_state.size() <= 64u,
                   "hostile Full lengths clamped to buffer");

        NetSerializer hostile_patch;
        hostile_patch.write_u8(static_cast<u8>(SnapshotDeltaKind::EntityPatch));
        for (int i = 0; i < 8; ++i) {
            hostile_patch.write_u32(0);
        }
        hostile_patch.write_u32(0xFFFFFFFFu);
        hostile_patch.reset_read();
        const SnapshotDelta decoded_patch = deserialize_snapshot_delta(hostile_patch);
        expectTrue(decoded_patch.entity_patches.size() <= 1u, "hostile patch count clamped to buffer");

        Rng fuzz(31337);
        for (u32 i = 0; i < 2000; ++i) {
            NetSerializer garbage;
            const u32 size = fuzz.range(0, 200);
            for (u32 b = 0; b < size; ++b) {
                garbage.write_u8(static_cast<u8>(fuzz.next()));
            }
            garbage.reset_read();
            const SnapshotDelta junk = deserialize_snapshot_delta(garbage);
            usize payload = junk.full_ecs_state.size() + junk.full_physics_state.size();
            for (const SnapshotEntityPatch& patch : junk.entity_patches) {
                payload += patch.ecs_bytes.size() + patch.physics_bytes.size();
            }
            if (payload > 200u || junk.entity_patches.size() > 200u) {
                expectTrue(false, "garbage delta decode bounded by input size");
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Entity replication relevance (AOI with hysteresis) vs brute force
// ---------------------------------------------------------------------------------------------

void run_relevance_gate() {
    std::printf("[gate] replication relevance (AOI + hysteresis) vs brute force\n");
    InterestPolicy policy;
    policy.relevance_radius = 100.f;
    policy.unload_radius = 130.f;
    policy.always_relevant_radius = 10.f;

    InterestManager manager;
    manager.set_policy(policy);
    manager.set_observer_position({0.f, 0.f, 0.f, 1.f});

    struct Mover {
        ecs::EntityID id;
        ecs::vec3 position;
        ecs::vec3 velocity;
        bool registered;
    };
    Rng rng(8080);
    std::vector<Mover> movers;
    for (u32 i = 0; i < 200; ++i) {
        Mover m{};
        m.id = ecs::EntityID{i, 1};
        m.position = {rng.real(-200.f, 200.f), 0.f, rng.real(-200.f, 200.f), 1.f};
        m.velocity = {rng.real(-6.f, 6.f), 0.f, rng.real(-6.f, 6.f), 0.f};
        m.registered = i < 150;
        if (m.registered) {
            (void)manager.register_entity({m.id, m.position, 0.f});
        }
        movers.push_back(m);
    }

    std::set<u32> reference_scope;
    std::set<u32> replicated; // Client-side view built only from enter/leave diffs.
    manager.evaluate();
    for (const Mover& m : movers) {
        const f32 d2 = distance_sq_3d({0.f, 0.f, 0.f, 1.f}, m.position);
        if (m.registered && d2 <= 100.f * 100.f) {
            reference_scope.insert(m.id.index);
        }
    }
    for (const ecs::EntityID id : manager.scope_set().entities) {
        replicated.insert(id.index);
    }

    u32 scope_mismatch = 0;
    u32 view_mismatch = 0;
    u32 enters = 0;
    u32 leaves = 0;
    for (u32 step = 0; step < 300; ++step) {
        ecs::vec3 observer{std::sin(step * 0.02f) * 50.f, 0.f, std::cos(step * 0.015f) * 50.f, 1.f};
        manager.set_observer_position(observer);
        for (Mover& m : movers) {
            m.position.x += m.velocity.x;
            m.position.z += m.velocity.z;
            if (std::fabs(m.position.x) > 250.f) {
                m.velocity.x = -m.velocity.x;
            }
            if (std::fabs(m.position.z) > 250.f) {
                m.velocity.z = -m.velocity.z;
            }
            if (m.registered) {
                (void)manager.update_entity_position(m.id, m.position);
            }
        }
        // Churn: late joiners register mid-session (must not reset others' hysteresis).
        if (step % 20 == 10) {
            for (Mover& m : movers) {
                if (!m.registered) {
                    m.registered = true;
                    (void)manager.register_entity({m.id, m.position, 0.f});
                    break;
                }
            }
        }

        std::set<u32> next_scope;
        for (const Mover& m : movers) {
            if (!m.registered) {
                continue;
            }
            const f32 d2 = distance_sq_3d(observer, m.position);
            const bool was_in = reference_scope.count(m.id.index) != 0;
            if (d2 <= 100.f * 100.f || (was_in && d2 <= 130.f * 130.f)) {
                next_scope.insert(m.id.index);
            }
        }
        reference_scope = next_scope;

        InterestSetDiff diff;
        (void)manager.evaluate_and_diff(diff);
        for (const ecs::EntityID id : diff.entered) {
            replicated.insert(id.index);
            ++enters;
        }
        for (const ecs::EntityID id : diff.left) {
            replicated.erase(id.index);
            ++leaves;
        }
        std::set<u32> manager_scope;
        for (const ecs::EntityID id : manager.scope_set().entities) {
            manager_scope.insert(id.index);
        }
        scope_mismatch += manager_scope == reference_scope ? 0u : 1u;
        view_mismatch += replicated == reference_scope ? 0u : 1u;
    }
    std::printf("  300 steps, 200 movers (50 late joiners): scope mismatches %u, diff-replicated view mismatches %u, "
                "enters %u leaves %u\n",
                scope_mismatch, view_mismatch, enters, leaves);
    expectTrue(scope_mismatch == 0u, "AOI scope matches brute-force hysteresis reference every step");
    expectTrue(view_mismatch == 0u, "enter/leave diffs reproduce the relevant set on the client");
    expectTrue(enters > 0u && leaves > 0u, "relevance churn exercised");
}

} // namespace

} // namespace fuse::net::tests

int main() {
    // Transport gates fork a client process — run them before core starts any worker threads.
    fuse::net::tests::run_transport_gates();

    fuse::core::initialize();

    fuse::net::tests::run_rollback_snapshot_roundtrip_gate();
    fuse::net::tests::run_rollback_resim_gate();
    fuse::net::tests::run_desync_gate();
    fuse::net::tests::run_interpolation_gate();
    fuse::net::tests::run_snapshot_delta_gates();
    fuse::net::tests::run_relevance_gate();

    fuse::core::shutdown();

    if (fuse::net::tests::g_failures == 0) {
        std::printf("fuse_b7_net_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b7_net_gates: %d failure(s)\n", fuse::net::tests::g_failures);
    return EXIT_FAILURE;
}
