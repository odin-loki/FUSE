#include <fuse/net/transport.hpp>

#include "test_helpers.hpp"

#include <cstring>

namespace fuse::net::tests {

void run_transport_tests() {
    fuse::net::LoopbackTransport host;
    fuse::net::LoopbackTransport client;

    expectTrue(host.init(27015), "host loopback init");
    expectTrue(client.init(27016), "client loopback init");
    fuse::net::LoopbackTransport::link_peers(host, client);

    expectTrue(host.peer_count() == 1u, "host sees linked peer");
    expectTrue(client.peer_count() == 1u, "client sees linked peer");

    const char payload[] = "fuse_net_ping";
    expectTrue(!host.send(1, nullptr, 0, fuse::net::PacketChannel::Reliable), "empty send rejected");

    expectTrue(host.send(1, reinterpret_cast<const fuse::net::byte*>(payload), sizeof(payload) - 1,
                       fuse::net::PacketChannel::Reliable),
               "host sends reliable packet");

    bool received = false;
    client.poll([&](const fuse::net::Packet& packet) {
        received = true;
        expectTrue(packet.channel == fuse::net::PacketChannel::Reliable, "reliable channel preserved");
        expectTrue(packet.data.size() == sizeof(payload) - 1, "payload size matches");
        expectTrue(std::memcmp(packet.data.data(), payload, packet.data.size()) == 0, "payload bytes match");
    });
    expectTrue(received, "client received host packet");

    const fuse::net::byte unreliable_byte = 42;
    expectTrue(client.send(1, &unreliable_byte, 1, fuse::net::PacketChannel::Unreliable),
               "client sends unreliable packet");

    bool host_received = false;
    host.poll([&](const fuse::net::Packet& packet) {
        host_received = true;
        expectTrue(packet.channel == fuse::net::PacketChannel::Unreliable, "unreliable channel preserved");
        expectTrue(packet.data.size() == 1u && packet.data[0] == 42, "unreliable payload intact");
    });
    expectTrue(host_received, "host received client packet");

    fuse::net::ENetTransport enet;
    expectTrue(!enet.init(27017), "ENet transport stub rejects init");
    expectTrue(enet.peer_count() == 0u, "ENet stub has no peers");

    fuse::net::SteamTransport steam;
    expectTrue(!steam.init(27018), "Steam transport stub rejects init");

    expectTrue(fuse::net::active_transport_backend() == fuse::net::TransportBackend::Loopback,
               "default backend is loopback");

    auto loopback = fuse::net::create_transport(fuse::net::TransportBackend::Loopback);
    expectTrue(loopback != nullptr, "loopback factory succeeds");
    expectTrue(fuse::net::transport_backend_available(fuse::net::TransportBackend::Loopback),
               "loopback backend available");
    expectTrue(!fuse::net::transport_backend_available(fuse::net::TransportBackend::ENet),
               "ENet backend unavailable in stub build");

    const fuse::net::byte seq_payload_a = 1;
    const fuse::net::byte seq_payload_b = 2;
    expectTrue(host.send(1, &seq_payload_a, 1, fuse::net::PacketChannel::UnreliableSeq),
               "host sends first unreliable-seq packet");
    expectTrue(host.send(1, &seq_payload_b, 1, fuse::net::PacketChannel::UnreliableSeq),
               "host sends second unreliable-seq packet");

    fuse::u32 delivered = 0;
    client.poll([&](const fuse::net::Packet& packet) {
        ++delivered;
        expectTrue(packet.sequence >= 1u, "unreliable-seq packet has sequence");
        expectTrue(packet.data[0] == 2, "newest unreliable-seq payload delivered");
    });
    expectTrue(delivered == 1u, "stale unreliable-seq packet dropped");

    const fuse::net::TransportStats host_stats = host.stats();
    expectTrue(host_stats.packets_sent >= 3u, "transport stats count sends");
    expectTrue(host_stats.bytes_sent > 0u, "transport stats count bytes");
}

} // namespace fuse::net::tests
