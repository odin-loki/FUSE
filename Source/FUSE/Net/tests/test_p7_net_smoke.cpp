#include <fuse/core/init.hpp>
#include <fuse/net/checksum.hpp>
#include <fuse/net/transport.hpp>

#include "test_helpers.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fuse::net::tests {

void run_p7_net_smoke() {
    fuse::net::LoopbackTransport host;
    fuse::net::LoopbackTransport client;

    expectTrue(host.init(27015), "host loopback init");
    expectTrue(client.init(27016), "client loopback init");
    expectTrue(host.connect("127.0.0.1", 27016), "host connect after init");
    expectTrue(client.connect("127.0.0.1", 27015), "client connect after init");

    fuse::net::LoopbackTransport::link_peers(host, client);
    expectTrue(host.peer_count() == 1u, "host sees linked peer");
    expectTrue(client.peer_count() == 1u, "client sees linked peer");

    const fuse::net::TransportStats host_before = host.stats();
    const fuse::net::TransportStats client_before = client.stats();
    expectTrue(host_before.packets_sent == 0u, "host starts with zero packets_sent");
    expectTrue(client_before.packets_received == 0u, "client starts with zero packets_received");

    const char reliable_payload[] = "p7_reliable";
    const fuse::usize reliable_size = sizeof(reliable_payload) - 1;
    const fuse::u64 reliable_checksum =
        fuse::net::fnv1a64_bytes(reinterpret_cast<const fuse::net::byte*>(reliable_payload), reliable_size);
    expectTrue(reliable_checksum != 0u, "reliable payload checksum is non-zero");

    expectTrue(host.send(1, reinterpret_cast<const fuse::net::byte*>(reliable_payload), reliable_size,
                       fuse::net::PacketChannel::Reliable),
               "host send Reliable is not a no-op");

    const fuse::net::TransportStats host_after_reliable = host.stats();
    expectTrue(host_after_reliable.packets_sent == host_before.packets_sent + 1u,
               "packets_sent increments after Reliable send");
    expectTrue(host_after_reliable.bytes_sent >= reliable_size, "bytes_sent counts Reliable payload");

    bool reliable_received = false;
    client.poll([&](const fuse::net::Packet& packet) {
        reliable_received = true;
        expectTrue(packet.channel == fuse::net::PacketChannel::Reliable, "Reliable channel preserved");
        expectTrue(packet.data.size() == reliable_size, "Reliable payload size intact");
        expectTrue(std::memcmp(packet.data.data(), reliable_payload, reliable_size) == 0,
                   "Reliable payload bytes intact");
        expectTrue(fuse::net::fnv1a64_bytes(packet.data.data(), packet.data.size()) == reliable_checksum,
                   "Reliable receive checksum matches");
    });
    expectTrue(reliable_received, "poll delivered Reliable payload (not a no-op)");

    const fuse::net::TransportStats client_after_reliable = client.stats();
    expectTrue(client_after_reliable.packets_received == client_before.packets_received + 1u,
               "packets_received increments after Reliable poll");

    const char seq_payload[] = "p7_unrel_seq";
    const fuse::usize seq_size = sizeof(seq_payload) - 1;
    const fuse::u64 seq_checksum =
        fuse::net::fnv1a64_bytes(reinterpret_cast<const fuse::net::byte*>(seq_payload), seq_size);
    expectTrue(seq_checksum != 0u, "UnreliableSeq payload checksum is non-zero");

    const fuse::net::TransportStats host_before_seq = host.stats();
    const fuse::net::TransportStats client_before_seq = client.stats();

    expectTrue(host.send(1, reinterpret_cast<const fuse::net::byte*>(seq_payload), seq_size,
                       fuse::net::PacketChannel::UnreliableSeq),
               "host send UnreliableSeq is not a no-op");

    const fuse::net::TransportStats host_after_seq = host.stats();
    expectTrue(host_after_seq.packets_sent == host_before_seq.packets_sent + 1u,
               "packets_sent increments after UnreliableSeq send");

    bool seq_received = false;
    client.poll([&](const fuse::net::Packet& packet) {
        seq_received = true;
        expectTrue(packet.channel == fuse::net::PacketChannel::UnreliableSeq, "UnreliableSeq channel preserved");
        expectTrue(packet.sequence >= 1u, "UnreliableSeq packet has sequence");
        expectTrue(packet.data.size() == seq_size, "UnreliableSeq payload size intact");
        expectTrue(std::memcmp(packet.data.data(), seq_payload, seq_size) == 0,
                   "UnreliableSeq payload bytes intact");
        expectTrue(fuse::net::fnv1a64_bytes(packet.data.data(), packet.data.size()) == seq_checksum,
                   "UnreliableSeq receive checksum matches");
    });
    expectTrue(seq_received, "poll delivered UnreliableSeq payload (not a no-op)");

    const fuse::net::TransportStats client_after_seq = client.stats();
    expectTrue(client_after_seq.packets_received == client_before_seq.packets_received + 1u,
               "packets_received increments after UnreliableSeq poll");

    host.destroy();
    host.destroy();
    client.destroy();
    client.destroy();
    expectTrue(host.peer_count() == 0u, "host destroy twice leaves no peers");
    expectTrue(client.peer_count() == 0u, "client destroy twice leaves no peers");
}

} // namespace fuse::net::tests

int main() {
    fuse::core::initialize();

    fuse::net::tests::run_p7_net_smoke();

    fuse::core::shutdown();

    if (fuse::net::tests::g_failures == 0) {
        std::printf("fuse_p7_net_smoke: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_p7_net_smoke: %d failure(s)\n", fuse::net::tests::g_failures);
    return EXIT_FAILURE;
}
