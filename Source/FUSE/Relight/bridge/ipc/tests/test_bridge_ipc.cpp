// FUSE Relight RL-2.1: bridge IPC core tests (ctest rl_bridge_ipc_*). Linux-native and, in the
// MinGW tree, the same binary under Wine. Cross-process modes spawn this executable as the peer.
//
//   unit                 in-process: wire codec round trip of every command, malformed-input fuzz,
//                        atomic/blocking queues, data ring, shared heap, session handshake (versions,
//                        layout, timeouts), graceful close, back-pressure
//   fuzz [--count N] [--blocking]
//                        cross-process: N random commands client -> host; the host decodes each
//                        (generated dispatch), re-encodes it and sends it back; the client checks the
//                        echo byte-for-byte and by re-decoding. Small rings force rollover and
//                        back-pressure in both directions.
//   heap [--count N]     cross-process shared heap: random-size blocks through a small heap (segment
//                        growth, reclaim, allocation back-pressure), verified and released by the host
//   peer-death           the peer is killed while this process is blocked in receive, send (full
//                        queue) and heap allocation (full heap), as client and as host; every wait
//                        must return PeerDead within 1 s
//   child <role> <name> [args]   peer roles used by the modes above
// Copyright (c) 2026 FUSE contributors (AGPL-3.0).
#include <fuse/relight/bridge/ipc/atomic_queue.hpp>
#include <fuse/relight/bridge/ipc/blocking_queue.hpp>
#include <fuse/relight/bridge/ipc/data_ring.hpp>
#include <fuse/relight/bridge/ipc/session.hpp>
#include <fuse/relight/bridge/ipc/shared_heap.hpp>
#include <fuse/relight/bridge/schema/commands_random.gen.hpp>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace ipc = fuse::relight::bridge::ipc;
namespace schema = fuse::relight::bridge::schema;
using ipc::Result;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

#define CHECK_RESULT(expr, expected)                                                               \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const Result _r = (expr);                                                                  \
        if (_r != (expected)) {                                                                    \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s -> %s (expected %s)\n", __FILE__, __LINE__, #expr, \
                         ipc::toString(_r), ipc::toString(expected));                              \
        }                                                                                          \
    } while (0)

// splitmix64 with a configurable length profile for counted fields.
struct Rng {
    uint64_t state;
    uint32_t largeMax = 16384;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t length() {
        const uint64_t r = next() % 1000;
        if (r < 700) {
            return static_cast<uint32_t>(next() % 9);
        }
        if (r < 990) {
            return static_cast<uint32_t>(next() % 257);
        }
        return static_cast<uint32_t>(next() % (largeMax + 1));
    }
};

std::string uniqueName(const char* tag) {
    static std::atomic<uint32_t> counter {0};
    return std::string("t") + std::to_string(ipc::currentPid()) + "-" + tag + "-" + std::to_string(counter++) + "-" +
           std::to_string(ipc::nowMs() % 100000);
}

template <class Cmd>
std::vector<uint8_t> encodeBytes(const Cmd& c) {
    std::vector<uint8_t> out(schema::cmd::encodedSize(c));
    ipc::WireWriter w(out.data(), out.size());
    schema::cmd::encode(w, c);
    return out;
}

// ============================================================================================
// unit: wire codec
// ============================================================================================

struct Noop {
    template <class T, class... Rest>
    void operator()(const T&, const Rest&...) const {}
};

// Re-encodes whatever dispatch decoded and compares with the expected id and bytes.
struct Reencode {
    uint16_t id = 0;
    const std::vector<uint8_t>* expected = nullptr;
    bool ok = false;
    template <class T>
    void operator()(const T& got) {
        ok = static_cast<uint16_t>(T::kId) == id && encodeBytes(got) == *expected;
    }
};

void testCodecRoundTrip() {
    struct Sample {
        uint16_t id;
        std::vector<uint8_t> bytes;
    };
    std::vector<Sample> samples;
    Rng rng(0x5EED0001);
    constexpr int kRounds = 40;
    for (int round = 0; round < kRounds; ++round) {
        for (uint32_t index = 0; index < schema::kCommandCount; ++index) {
            schema::withRandomCommand(index, rng, [&](const auto& c) {
                using Cmd = std::decay_t<decltype(c)>;
                std::vector<uint8_t> bytes = encodeBytes(c);
                Cmd back;
                ipc::WireReader r(bytes.data(), bytes.size());
                CHECK(schema::cmd::decode(r, back) && r.atEnd());
                CHECK(back == c);
                CHECK(static_cast<uint32_t>(Cmd::kId) == index + 1);
                samples.push_back({static_cast<uint16_t>(Cmd::kId), std::move(bytes)});
            });
        }
    }
    uint32_t decodedOk = 0;
    for (const Sample& smp : samples) {
        Reencode v;
        v.id = smp.id;
        v.expected = &smp.bytes;
        CHECK(schema::dispatch(smp.id, smp.bytes.data(), smp.bytes.size(), v) == schema::DecodeStatus::Ok);
        CHECK(v.ok);
        decodedOk += v.ok ? 1 : 0;
        // Truncation and trailing garbage are both malformed (for non-empty payloads).
        if (!smp.bytes.empty()) {
            CHECK(schema::dispatch(smp.id, smp.bytes.data(), smp.bytes.size() - 1, Noop {}) ==
                  schema::DecodeStatus::Malformed);
        }
        std::vector<uint8_t> longer = smp.bytes;
        longer.push_back(0xAB);
        CHECK(schema::dispatch(smp.id, longer.data(), longer.size(), Noop {}) == schema::DecodeStatus::Malformed);
    }
    CHECK(decodedOk == uint32_t(kRounds) * schema::kCommandCount);
    CHECK(schema::dispatch(0, nullptr, 0, Noop {}) == schema::DecodeStatus::UnknownCommand);
    CHECK(schema::dispatch(static_cast<uint16_t>(schema::kCommandCount + 1), nullptr, 0, Noop {}) ==
          schema::DecodeStatus::UnknownCommand);
    CHECK(std::strcmp(schema::commandName(schema::CommandId::Bridge_Syn), "Bridge_Syn") == 0);
    CHECK(std::strcmp(schema::commandName(uint16_t(0xFFFF)), "Unknown") == 0);
    // The table grows and shrinks (RL-2.2 retyped it): check the generated ids are dense and named,
    // not a fixed row count.
    CHECK(schema::kCommandCount > 0);
    CHECK(std::strcmp(schema::commandName(uint16_t(schema::kCommandCount)), "Unknown") != 0);
    CHECK(std::strcmp(schema::commandName(uint16_t(schema::kCommandCount + 1)), "Unknown") == 0);
}

void testMalformedFuzz() {
    // Random bytes to random decoders: must never crash or over-read (ASan/Wine catch that), and a
    // decoded result must re-encode to exactly the accepted bytes.
    Rng rng(0xBAD0001);
    std::vector<uint8_t> buf;
    uint32_t accepted = 0;
    for (int i = 0; i < 200000; ++i) {
        const uint16_t id = static_cast<uint16_t>(rng.next() % (schema::kCommandCount + 3));
        buf.resize(rng.next() % 96);
        for (uint8_t& b : buf) {
            b = static_cast<uint8_t>(rng.next());
        }
        // Bias the leading count words small so counted fields are sometimes well-formed.
        if (buf.size() >= 4 && (rng.next() & 1)) {
            buf[1] = buf[2] = buf[3] = 0;
            buf[0] &= 0x0F;
        }
        const auto st = schema::dispatch(id, buf.data(), buf.size(), [&](const auto& got) {
            CHECK(encodeBytes(got) == buf);
            ++accepted;
        });
        CHECK(st != schema::DecodeStatus::UnknownCommand || !schema::isKnownCommand(id));
    }
    std::printf("  malformed fuzz: 200000 inputs, %u decoded and re-encoded identically\n", accepted);
}

// ============================================================================================
// unit: queues, ring, heap
// ============================================================================================

struct Elem {
    uint32_t seq;
    uint32_t check;
};

template <class Queue>
void queueThroughput(Queue& producer, Queue& consumer, uint32_t count) {
    std::atomic<bool> ok {true};
    std::thread t([&] {
        for (uint32_t i = 0; i < count; ++i) {
            Elem e {};
            if (consumer.pull(e, ipc::kInfinite, nullptr) != Result::Success || e.seq != i || e.check != ~i) {
                ok = false;
                return;
            }
        }
    });
    for (uint32_t i = 0; i < count; ++i) {
        if (producer.push(Elem {i, ~i}, ipc::kInfinite, nullptr) != Result::Success) {
            ok = false;
            break;
        }
    }
    t.join();
    CHECK(ok.load());
}

void testAtomicQueue() {
    constexpr uint32_t kCap = 64;
    std::vector<uint64_t> mem((ipc::AtomicQueue<Elem>::bytesFor(kCap) + 7) / 8);
    ipc::AtomicQueue<Elem> w;
    ipc::AtomicQueue<Elem> r;
    CHECK_RESULT(w.initialize(mem.data(), 63), Result::Malformed);  // not a power of two
    CHECK_RESULT(w.initialize(mem.data(), kCap), Result::Success);
    CHECK_RESULT(r.attach(mem.data(), kCap), Result::Success);
    Elem e {};
    CHECK_RESULT(r.pull(e, ipc::kNoWait, nullptr), Result::Timeout);
    CHECK_RESULT(r.pull(e, 30, nullptr), Result::Timeout);
    // Back-pressure: exactly kCap elements fit, then push times out.
    for (uint32_t i = 0; i < kCap; ++i) {
        CHECK_RESULT(w.push(Elem {i, ~i}, ipc::kNoWait, nullptr), Result::Success);
    }
    CHECK(w.size() == kCap);
    CHECK_RESULT(w.push(Elem {99, 0}, ipc::kNoWait, nullptr), Result::Timeout);
    const uint64_t t0 = ipc::nowMs();
    CHECK_RESULT(w.push(Elem {99, 0}, 50, nullptr), Result::Timeout);
    CHECK(ipc::nowMs() - t0 >= 45);
    CHECK_RESULT(r.peek(e, ipc::kNoWait, nullptr), Result::Success);
    CHECK(e.seq == 0);
    for (uint32_t i = 0; i < kCap; ++i) {
        CHECK_RESULT(r.pull(e, ipc::kNoWait, nullptr), Result::Success);
        CHECK(e.seq == i);
    }
    CHECK(r.isEmpty());
    // Early-out signal (upstream pbEarlyOutSignal).
    std::atomic<bool> cancel {true};
    ipc::WaitContext ctx;
    ctx.cancel = &cancel;
    CHECK_RESULT(r.pull(e, ipc::kInfinite, &ctx), Result::Timeout);
    queueThroughput(w, r, 200000);
    // A corrupt peer index is detected, not followed.
    auto* ctl = reinterpret_cast<ipc::QueueControl*>(mem.data());
    ctl->tail.store(ctl->head.load() + kCap + 5);
    CHECK_RESULT(r.pull(e, ipc::kNoWait, nullptr), Result::Malformed);
}

void testBlockingQueue() {
    constexpr uint32_t kCap = 32;
    std::vector<uint64_t> mem((ipc::BlockingQueue<Elem>::bytesFor(kCap) + 7) / 8);
    const std::string name = uniqueName("bq");
    ipc::BlockingQueue<Elem> w;
    ipc::BlockingQueue<Elem> r;
    CHECK_RESULT(w.initialize(mem.data(), kCap, name), Result::Success);
    CHECK_RESULT(r.attach(mem.data(), kCap, name), Result::Success);
    Elem e {};
    CHECK_RESULT(r.pull(e, ipc::kNoWait, nullptr), Result::Timeout);
    CHECK_RESULT(r.pull(e, 30, nullptr), Result::Timeout);
    for (uint32_t i = 0; i < kCap; ++i) {
        CHECK_RESULT(w.push(Elem {i, ~i}, ipc::kNoWait, nullptr), Result::Success);
    }
    CHECK_RESULT(w.push(Elem {99, 0}, ipc::kNoWait, nullptr), Result::Timeout);
    CHECK_RESULT(w.push(Elem {99, 0}, 40, nullptr), Result::Timeout);
    for (uint32_t i = 0; i < kCap; ++i) {
        CHECK_RESULT(r.pull(e, ipc::kNoWait, nullptr), Result::Success);
        CHECK(e.seq == i);
    }
    queueThroughput(w, r, 50000);
}

void testDataRing() {
    constexpr uint32_t kCap = 1024;
    std::vector<uint64_t> mem((ipc::DataRing::bytesFor(kCap) + 7) / 8);
    ipc::DataRing w;
    ipc::DataRing r;
    CHECK_RESULT(w.initialize(mem.data(), kCap), Result::Success);
    CHECK_RESULT(r.attach(mem.data(), kCap), Result::Success);
    ipc::DataRing::Reservation res;
    CHECK_RESULT(w.reserve(kCap / 2 + 1, ipc::kNoWait, nullptr, res), Result::TooLarge);
    // Rollover: 300 + 300 + 300 bytes; the third does not fit before the end and wraps to 0 once
    // the first two were released.
    uint32_t ends[3] = {};
    uint32_t offsets[3] = {};
    for (int i = 0; i < 2; ++i) {
        CHECK_RESULT(w.reserve(300, ipc::kNoWait, nullptr, res), Result::Success);
        std::memset(res.data, 0x10 + i, 300);
        w.commit(res);
        offsets[i] = res.offset;
        ends[i] = res.end;
    }
    CHECK_RESULT(w.reserve(500, ipc::kNoWait, nullptr, res), Result::Timeout);  // back-pressure
    const uint8_t* p = nullptr;
    uint32_t end = 0;
    CHECK_RESULT(r.view(offsets[0], 300, p, end), Result::Success);
    CHECK(p[0] == 0x10 && p[299] == 0x10 && end == ends[0]);
    r.release(end);
    CHECK_RESULT(r.view(offsets[1], 300, p, end), Result::Success);
    CHECK(p[0] == 0x11);
    r.release(end);
    CHECK_RESULT(w.reserve(500, ipc::kNoWait, nullptr, res), Result::Success);
    CHECK((res.offset & (kCap - 1)) == 0);  // rolled over
    offsets[2] = res.offset;
    w.commit(res);
    CHECK_RESULT(r.view(offsets[2], 500, p, end), Result::Success);
    // Headers pointing outside the valid window are rejected.
    CHECK_RESULT(r.view(offsets[2] + 2 * kCap, 8, p, end), Result::Malformed);
    CHECK_RESULT(r.view(offsets[2] + 1000, 64, p, end), Result::Malformed);
    CHECK_RESULT(r.view(offsets[2], kCap, p, end), Result::Malformed);
    // Zero-byte payloads are fine even when the ring is full.
    r.release(end);
    CHECK_RESULT(w.reserve(0, ipc::kNoWait, nullptr, res), Result::Success);
}

void testSharedHeapInProcess() {
    const std::string name = uniqueName("heap");
    ipc::HeapConfig cfg;
    cfg.chunkBytes = 1024;
    cfg.segmentBytes = 16 * 1024;
    cfg.maxBytes = 64 * 1024;  // 4 segments of 16 chunks
    ipc::SharedHeap owner;
    ipc::SharedHeap peer;
    CHECK_RESULT(owner.create(name, cfg), Result::Success);
    CHECK_RESULT(peer.open(name), Result::Success);
    ipc::HeapRef a, b, c;
    CHECK_RESULT(owner.allocate(10 * 1024, a, ipc::kNoWait), Result::Success);
    CHECK_RESULT(owner.allocate(5 * 1024, b, ipc::kNoWait), Result::Success);
    std::memset(owner.data(a), 0xA5, 10 * 1024);
    std::memset(owner.data(b), 0x5A, 5 * 1024);
    // Does not fit in segment 0's last free chunk: goes to a new segment (upstream would have tried
    // the gap and failed on the boundary check).
    CHECK_RESULT(owner.allocate(3 * 1024, c, ipc::kNoWait), Result::Success);
    CHECK(owner.stats().segments == 2);
    CHECK(c.firstChunk == 16);
    const uint8_t* pa = peer.resolve(a);  // peer maps segment 1 on demand
    const uint8_t* pc = peer.resolve(c);
    CHECK(pa != nullptr && pa[0] == 0xA5 && pa[10 * 1024 - 1] == 0xA5);
    CHECK(pc != nullptr);
    CHECK(peer.release(a));
    CHECK(!peer.release(a));  // double release detected
    CHECK(peer.resolve(a) == nullptr);
    ipc::HeapRef bogus {4000, 16};
    CHECK(peer.resolve(bogus) == nullptr);
    ipc::HeapRef crossing {12, 8 * 1024};  // would cross the segment 0/1 boundary
    CHECK(peer.resolve(crossing) == nullptr);
    // Like upstream, free space is used before released runs are reclaimed; once reclaimed, the
    // released 10 chunks are reused first-fit.
    CHECK(owner.collect() == 1);
    CHECK(owner.collect() == 0);
    ipc::HeapRef d;
    CHECK_RESULT(owner.allocate(8 * 1024, d, ipc::kNoWait), Result::Success);
    CHECK(d.firstChunk == 0);
    // Fill to the cap, then back-pressure until the peer releases.
    std::vector<ipc::HeapRef> fill;
    for (;;) {
        ipc::HeapRef x;
        const Result r = owner.allocate(4 * 1024, x, ipc::kNoWait);
        if (r != Result::Success) {
            CHECK(r == Result::Timeout);
            break;
        }
        fill.push_back(x);
    }
    CHECK(owner.stats().segments == 4);
    ipc::HeapRef blocked;
    const uint64_t t0 = ipc::nowMs();
    CHECK_RESULT(owner.allocate(4 * 1024, blocked, 60), Result::Timeout);
    CHECK(ipc::nowMs() - t0 >= 55);
    std::thread releaser([&] {
        ipc::sleepMs(30);
        peer.resolve(fill.back());
        peer.release(fill.back());
    });
    CHECK_RESULT(owner.allocate(4 * 1024, blocked, 2000), Result::Success);
    releaser.join();
    CHECK(blocked.firstChunk == fill.back().firstChunk);
    // A request larger than the whole heap, or larger than every segment once the heap is at its
    // segment cap, is refused outright instead of waiting.
    ipc::HeapRef huge;
    CHECK_RESULT(owner.allocate(cfg.maxBytes + 1, huge, ipc::kNoWait), Result::TooLarge);
    CHECK_RESULT(owner.allocate(cfg.segmentBytes + 1, huge, 5000), Result::TooLarge);
}

// ============================================================================================
// unit: sessions (client and host in one process, two threads)
// ============================================================================================

ipc::SessionConfig smallConfig(ipc::QueueKind kind) {
    ipc::SessionConfig c;
    c.queueKind = kind;
    c.clientToHostSlots = 16;
    c.clientToHostDataBytes = 4096;
    c.hostToClientSlots = 16;
    c.hostToClientDataBytes = 4096;
    c.heap.chunkBytes = 4096;
    c.heap.segmentBytes = 64 * 1024;
    c.heap.maxBytes = 256 * 1024;
    return c;
}

struct Pair {
    std::unique_ptr<ipc::Session> client;
    std::unique_ptr<ipc::Session> host;
    Result clientHs = Result::Failure;
    Result hostHs = Result::Failure;
};

Pair connectPair(const ipc::SessionConfig& cfg, uint32_t clientMajor = schema::kProtocolMajor,
                 uint64_t clientHash = schema::kSchemaHash, uint32_t clientMinor = schema::kProtocolMinor) {
    Pair p;
    const std::string name = uniqueName("s");
    if (ipc::Session::create(name, cfg, p.client) != Result::Success) {
        return p;
    }
    p.client->setAdvertised(clientMajor, clientMinor, clientHash);
    std::thread host([&] {
        if (ipc::Session::open(name, 2000, p.host) == Result::Success) {
            p.hostHs = p.host->handshake(2000);
        }
    });
    p.clientHs = p.client->handshake(3000);
    host.join();
    return p;
}

void testSessionHandshake() {
    for (ipc::QueueKind kind : {ipc::QueueKind::Atomic, ipc::QueueKind::Blocking}) {
        Pair p = connectPair(smallConfig(kind));
        CHECK_RESULT(p.clientHs, Result::Success);
        CHECK_RESULT(p.hostHs, Result::Success);
        if (p.clientHs != Result::Success || p.hostHs != Result::Success) {
            continue;
        }
        CHECK(p.client->peer().schemaHash == schema::kSchemaHash);
        CHECK(p.host->peer().pointerBits == sizeof(void*) * 8);
        CHECK(p.host->peer().buildTag == "fuse-relight");
        CHECK(p.client->peerState() == ipc::PeerState::Running);
        CHECK(p.host->peerState() == ipc::PeerState::Running);
        CHECK(p.client->hasHeap() && p.host->hasHeap());
        // A typed command, with a handle and flags, both ways.
        schema::cmd::IDirect3DDevice9Ex_SetTransform st;
        st.state = 256;
        for (int i = 0; i < 16; ++i) {
            st.matrix[size_t(i)] = float(i) * 0.5f;
        }
        CHECK_RESULT(p.client->send(st, 0x1234, ipc::kDataInSharedHeap), Result::Success);
        bool seen = false;
        CHECK_RESULT(p.host->receiveAndDispatch(
                         [&](const auto& c, const ipc::MessageHeader& h) {
                             using C = std::decay_t<decltype(c)>;
                             if constexpr (std::is_same_v<C, schema::cmd::IDirect3DDevice9Ex_SetTransform>) {
                                 seen = c == st && h.handle == 0x1234 && h.flags == ipc::kDataInSharedHeap &&
                                        h.uid == p.client->lastSentUid();
                             }
                         },
                         1000),
                     Result::Success);
        CHECK(seen);
        schema::cmd::Bridge_Response resp;
        resp.result = -5;
        resp.payload = {1, 2, 3};
        CHECK_RESULT(p.host->send(resp), Result::Success);
        ipc::Session::Incoming in;
        CHECK_RESULT(p.client->receive(in, 1000), Result::Success);
        CHECK(in.header.command == uint16_t(schema::CommandId::Bridge_Response));
        p.client->release(in);
        // Graceful close: queued messages drain first, then PeerClosed.
        CHECK_RESULT(p.client->send(schema::cmd::Bridge_Terminate {}), Result::Success);
        p.client->close();
        CHECK_RESULT(p.host->receive(in, 1000), Result::Success);
        p.host->release(in);
        CHECK_RESULT(p.host->receive(in, 1000), Result::PeerClosed);
        CHECK_RESULT(p.host->send(schema::cmd::Bridge_Continue {}, 0, 0, 1000), Result::Success);  // room left
    }
}

void testSessionVersionMismatch() {
    const ipc::SessionConfig cfg = smallConfig(ipc::QueueKind::Atomic);
    {
        Pair p = connectPair(cfg, schema::kProtocolMajor + 1);
        CHECK_RESULT(p.clientHs, Result::VersionMismatch);
        CHECK_RESULT(p.hostHs, Result::VersionMismatch);
        CHECK(p.client && p.client->peer().rejectReason == "protocol major version differs");
    }
    {
        Pair p = connectPair(cfg, schema::kProtocolMajor, schema::kSchemaHash ^ 1);
        CHECK_RESULT(p.clientHs, Result::VersionMismatch);
        CHECK_RESULT(p.hostHs, Result::VersionMismatch);
        CHECK(p.client && p.client->peer().rejectReason == "command schema hash differs");
    }
    {
        // A different minor version is compatible.
        Pair p = connectPair(cfg, schema::kProtocolMajor, schema::kSchemaHash, schema::kProtocolMinor + 7);
        CHECK_RESULT(p.clientHs, Result::Success);
        CHECK_RESULT(p.hostHs, Result::Success);
        CHECK(p.host && p.host->peer().protocolMinor == schema::kProtocolMinor + 7);
    }
    {
        // Control-block layout mismatch is caught before any queue is touched.
        const std::string name = uniqueName("layout");
        std::unique_ptr<ipc::Session> client;
        CHECK_RESULT(ipc::Session::create(name, cfg, client), Result::Success);
        ipc::SharedMemory raw;
        CHECK_RESULT(raw.open(name, sizeof(ipc::SessionControl)), Result::Success);
        static_cast<ipc::SessionControl*>(raw.data())->layoutVersion = ipc::kLayoutVersion + 1;
        std::unique_ptr<ipc::Session> host;
        CHECK_RESULT(ipc::Session::open(name, 500, host), Result::VersionMismatch);
    }
    {
        // No host: the client's handshake times out; no client: open times out.
        std::unique_ptr<ipc::Session> client;
        const std::string name = uniqueName("nohost");
        CHECK_RESULT(ipc::Session::create(name, cfg, client), Result::Success);
        // Exists: the name is taken while no host has attached (a host unlinks POSIX names).
        std::unique_ptr<ipc::Session> dup;
        CHECK_RESULT(ipc::Session::create(name, cfg, dup), Result::Exists);
        const uint64_t t0 = ipc::nowMs();
        CHECK_RESULT(client->handshake(150), Result::Timeout);
        CHECK(ipc::nowMs() - t0 < 1000);
        std::unique_ptr<ipc::Session> host;
        CHECK_RESULT(ipc::Session::open(uniqueName("noclient"), 100, host), Result::Timeout);
    }
}

void testSessionBackPressure() {
    for (ipc::QueueKind kind : {ipc::QueueKind::Atomic, ipc::QueueKind::Blocking}) {
        ipc::SessionConfig cfg = smallConfig(kind);
        cfg.clientToHostSlots = 8;
        cfg.clientToHostDataBytes = 1024;
        Pair p = connectPair(cfg);
        if (p.clientHs != Result::Success || p.hostHs != Result::Success) {
            CHECK(false);
            continue;
        }
        // Header-queue back-pressure: exactly 8 empty commands fit while the host is stalled.
        uint32_t sent = 0;
        while (p.client->send(schema::cmd::IDirect3DDevice9Ex_BeginScene {}, 0, 0, ipc::kNoWait) == Result::Success) {
            ++sent;
        }
        CHECK(sent == 8);
        CHECK_RESULT(p.client->send(schema::cmd::IDirect3DDevice9Ex_BeginScene {}, 0, 0, 40), Result::Timeout);
        ipc::Session::Incoming in;
        for (uint32_t i = 0; i < sent; ++i) {
            CHECK_RESULT(p.host->receive(in, 1000), Result::Success);
            p.host->release(in);
        }
        // Data-ring back-pressure: a 400-byte payload blocks once 2 are queued (1 KiB ring).
        schema::cmd::IDirect3DDevice9Ex_DrawPrimitiveUP up;
        up.vertexData.assign(400 - 16, 0x7E);
        CHECK_RESULT(p.client->send(up, 0, 0, ipc::kNoWait), Result::Success);
        CHECK_RESULT(p.client->send(up, 0, 0, ipc::kNoWait), Result::Success);
        CHECK_RESULT(p.client->send(up, 0, 0, 30), Result::Timeout);
        up.vertexData.assign(600, 0);
        CHECK_RESULT(p.client->send(up, 0, 0, ipc::kNoWait), Result::TooLarge);
        for (int i = 0; i < 2; ++i) {
            CHECK_RESULT(p.host->receive(in, 1000), Result::Success);
            p.host->release(in);
        }
        // Slow consumer: every message arrives intact and in order while the producer is throttled.
        constexpr uint32_t kCount = 3000;
        std::atomic<uint32_t> bad {0};
        std::thread consumer([&] {
            Rng rng(77);
            for (uint32_t i = 0; i < kCount; ++i) {
                if (i % 500 == 0) {
                    ipc::sleepMs(5);
                }
                const uint32_t len = static_cast<uint32_t>(rng.next() % 300);
                const Result r = p.host->receiveAndDispatch(
                    [&](const auto& c, const ipc::MessageHeader& h) {
                        using C = std::decay_t<decltype(c)>;
                        if constexpr (std::is_same_v<C, schema::cmd::IDirect3DDevice9Ex_DrawPrimitiveUP>) {
                            const bool ok = h.handle == i && c.primitiveCount == i && c.vertexData.size() == len &&
                                            std::all_of(c.vertexData.begin(), c.vertexData.end(),
                                                        [&](uint8_t b) { return b == uint8_t(i); });
                            bad += ok ? 0 : 1;
                        } else {
                            ++bad;
                        }
                    },
                    5000);
                if (r != Result::Success) {
                    ++bad;
                    return;
                }
            }
        });
        Rng rng(77);
        uint32_t sendFailures = 0;
        for (uint32_t i = 0; i < kCount; ++i) {
            schema::cmd::IDirect3DDevice9Ex_DrawPrimitiveUP m;
            m.primitiveCount = i;
            m.vertexData.assign(static_cast<size_t>(rng.next() % 300), uint8_t(i));
            sendFailures += p.client->send(m, i, 0, 5000) == Result::Success ? 0 : 1;
        }
        consumer.join();
        CHECK(sendFailures == 0);
        CHECK(bad.load() == 0);
    }
}

int runUnit() {
    std::printf("rl_bridge_ipc_unit: schema %u commands, protocol %u.%u, hash 0x%016" PRIx64 "\n",
                unsigned(schema::kCommandCount), schema::kProtocolMajor, schema::kProtocolMinor, schema::kSchemaHash);
    testCodecRoundTrip();
    testMalformedFuzz();
    testAtomicQueue();
    testBlockingQueue();
    testDataRing();
    testSharedHeapInProcess();
    testSessionHandshake();
    testSessionVersionMismatch();
    testSessionBackPressure();
    std::printf("rl_bridge_ipc_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ============================================================================================
// cross-process helpers
// ============================================================================================

std::string g_self;

Result spawnChild(ipc::ChildProcess& child, const std::vector<std::string>& args) {
    std::vector<std::string> full {g_self, "child"};
    full.insert(full.end(), args.begin(), args.end());
    return child.spawn(full);
}

ipc::SessionConfig fuzzConfig(ipc::QueueKind kind) {
    ipc::SessionConfig c;
    c.queueKind = kind;
    c.clientToHostSlots = 64;  // small: the producer regularly waits on a full header queue
    c.clientToHostDataBytes = 1u << 20;
    c.hostToClientSlots = 64;
    c.hostToClientDataBytes = 1u << 20;
    c.heap.maxBytes = 0;
    return c;
}

// Host role: echo every command back, re-encoded from the decoded struct.
int childEcho(const std::string& name) {
    std::unique_ptr<ipc::Session> s;
    Result r = ipc::Session::open(name, 10000, s);
    if (r == Result::Success) {
        r = s->handshake(10000);
    }
    if (r != Result::Success) {
        std::fprintf(stderr, "echo child: connect failed: %s\n", ipc::toString(r));
        return 2;
    }
    uint64_t echoed = 0;
    for (;;) {
        Result sendResult = Result::Success;
        r = s->receiveAndDispatch(
            [&](const auto& c, const ipc::MessageHeader& h) { sendResult = s->send(c, h.handle, h.flags); }, ipc::kInfinite);
        if (r == Result::PeerClosed) {
            break;
        }
        if (r != Result::Success || sendResult != Result::Success) {
            std::fprintf(stderr, "echo child: %s / %s after %" PRIu64 " messages\n", ipc::toString(r),
                         ipc::toString(sendResult), echoed);
            return 3;
        }
        ++echoed;
    }
    std::printf("echo child: %" PRIu64 " messages echoed\n", echoed);
    return 0;
}

int runFuzz(uint64_t count, ipc::QueueKind kind) {
    const std::string name = uniqueName("fuzz");
    std::unique_ptr<ipc::Session> s;
    CHECK_RESULT(ipc::Session::create(name, fuzzConfig(kind), s), Result::Success);
    if (!s) {
        return 1;
    }
    ipc::ChildProcess child;
    CHECK_RESULT(spawnChild(child, {"echo", name}), Result::Success);
    const Result hs = s->handshake(20000);
    CHECK_RESULT(hs, Result::Success);
    if (hs != Result::Success) {
        return 1;
    }
    constexpr uint64_t kSeed = 0xF0221D1Eull;
    const uint64_t t0 = ipc::nowMs();
    std::atomic<uint64_t> bytesSent {0};
    std::atomic<bool> senderOk {true};
    std::thread sender([&] {
        Rng rng(kSeed);
        for (uint64_t i = 0; i < count; ++i) {
            const uint32_t index = static_cast<uint32_t>(rng.next() % schema::kCommandCount);
            Result r = Result::Success;
            schema::withRandomCommand(index, rng, [&](const auto& c) {
                bytesSent += schema::cmd::encodedSize(c);
                r = s->send(c, static_cast<uint32_t>(i), static_cast<uint16_t>(i & 1), 30000);
            });
            if (r != Result::Success) {
                std::fprintf(stderr, "fuzz: send %" PRIu64 " failed: %s\n", i, ipc::toString(r));
                senderOk = false;
                return;
            }
        }
    });
    Rng expect(kSeed);
    uint64_t verified = 0;
    uint64_t mismatches = 0;
    std::vector<uint8_t> expBytes;
    for (uint64_t i = 0; i < count; ++i) {
        const uint32_t index = static_cast<uint32_t>(expect.next() % schema::kCommandCount);
        uint16_t expId = 0;
        schema::withRandomCommand(index, expect, [&](const auto& c) {
            expId = static_cast<uint16_t>(std::decay_t<decltype(c)>::kId);
            expBytes = encodeBytes(c);
        });
        ipc::Session::Incoming in;
        const Result r = s->receive(in, 30000);
        if (r != Result::Success) {
            std::fprintf(stderr, "fuzz: receive %" PRIu64 " failed: %s\n", i, ipc::toString(r));
            ++mismatches;
            break;
        }
        bool ok = in.header.command == expId && in.header.handle == uint32_t(i) && in.header.flags == uint16_t(i & 1) &&
                  in.header.dataSize == expBytes.size() &&
                  (expBytes.empty() || std::memcmp(in.data, expBytes.data(), expBytes.size()) == 0);
        // And the client-side decoder agrees with the host's encoder.
        const auto st = schema::dispatch(in.header.command, in.data, in.header.dataSize,
                                         [&](const auto& got) { ok = ok && encodeBytes(got) == expBytes; });
        ok = ok && st == schema::DecodeStatus::Ok;
        s->release(in);
        if (!ok) {
            if (mismatches < 5) {
                std::fprintf(stderr, "fuzz: mismatch at %" PRIu64 " (%s)\n", i, schema::commandName(expId));
            }
            ++mismatches;
        } else {
            ++verified;
        }
    }
    sender.join();
    const uint64_t dt = ipc::nowMs() - t0;
    s->close();
    int code = -1;
    CHECK_RESULT(child.wait(30000, &code), Result::Success);
    CHECK(code == 0);
    CHECK(senderOk.load());
    CHECK(mismatches == 0);
    CHECK(verified == count);
    std::printf("rl_bridge_ipc_fuzz (%s): %" PRIu64 " commands round-tripped (%" PRIu64 " MiB each way) in %" PRIu64
                " ms, %d failures\n",
                kind == ipc::QueueKind::Atomic ? "atomic" : "blocking", verified, bytesSent.load() >> 20, dt, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ---- heap -------------------------------------------------------------------------------------

uint32_t pattern(uint32_t seq, uint32_t i) { return (seq * 2654435761u) ^ (i * 40503u); }

// Host role: resolve each HeapRef (payload of Bridge_Response with kDataInSharedHeap), verify the
// pattern, release it and answer.
int childHeap(const std::string& name) {
    std::unique_ptr<ipc::Session> s;
    Result r = ipc::Session::open(name, 10000, s);
    if (r == Result::Success) {
        r = s->handshake(10000);
    }
    if (r != Result::Success) {
        return 2;
    }
    uint32_t bad = 0;
    for (;;) {
        ipc::Session::Incoming in;
        r = s->receive(in, ipc::kInfinite);
        if (r == Result::PeerClosed) {
            break;
        }
        if (r != Result::Success) {
            return 3;
        }
        schema::cmd::Bridge_Response msg;
        ipc::WireReader rd(in.data, in.header.dataSize);
        const bool okDecode = schema::cmd::decode(rd, msg) && rd.atEnd() && msg.payload.size() == 8 &&
                              (in.header.flags & ipc::kDataInSharedHeap) != 0;
        s->release(in);
        int32_t verdict = 0;
        if (okDecode) {
            ipc::HeapRef ref;
            std::memcpy(&ref.firstChunk, msg.payload.data(), 4);
            std::memcpy(&ref.bytes, msg.payload.data() + 4, 4);
            const uint8_t* p = s->heap().resolve(ref);
            verdict = p != nullptr ? 1 : -1;
            for (uint32_t i = 0; p != nullptr && i + 4 <= ref.bytes; i += 4) {
                uint32_t v;
                std::memcpy(&v, p + i, 4);
                if (v != pattern(in.header.handle, i)) {
                    verdict = -2;
                    break;
                }
            }
            if (p != nullptr && !s->heap().release(ref)) {
                verdict = -3;
            }
        }
        bad += verdict == 1 ? 0 : 1;
        schema::cmd::Bridge_Response ans;
        ans.result = verdict;
        if (s->send(ans, in.header.handle) != Result::Success) {
            return 4;
        }
    }
    return bad == 0 ? 0 : 5;
}

int runHeap(uint64_t count) {
    ipc::SessionConfig cfg = fuzzConfig(ipc::QueueKind::Atomic);
    cfg.heap.chunkBytes = 4096;
    cfg.heap.segmentBytes = 256 * 1024;
    cfg.heap.maxBytes = 2u << 20;  // small: allocation waits on the host's releases
    const std::string name = uniqueName("heapx");
    std::unique_ptr<ipc::Session> s;
    CHECK_RESULT(ipc::Session::create(name, cfg, s), Result::Success);
    if (!s) {
        return 1;
    }
    ipc::ChildProcess child;
    CHECK_RESULT(spawnChild(child, {"heap", name}), Result::Success);
    CHECK_RESULT(s->handshake(20000), Result::Success);
    std::atomic<uint64_t> answered {0};
    std::atomic<uint64_t> badAnswers {0};
    std::thread answers([&] {
        for (uint64_t i = 0; i < count; ++i) {
            const Result r = s->receiveAndDispatch(
                [&](const auto& c, const ipc::MessageHeader& h) {
                    using C = std::decay_t<decltype(c)>;
                    if constexpr (std::is_same_v<C, schema::cmd::Bridge_Response>) {
                        if (c.result != 1 || h.handle != uint32_t(i)) {
                            if (badAnswers.load() < 5) {
                                std::fprintf(stderr, "heap: block %" PRIu64 " answer %d (handle %u)\n", i, int(c.result),
                                             unsigned(h.handle));
                            }
                            ++badAnswers;
                        }
                    } else {
                        ++badAnswers;
                    }
                },
                30000);
            if (r != Result::Success) {
                ++badAnswers;
                return;
            }
            ++answered;
        }
    });
    Rng rng(0x4EA9);
    uint64_t allocFailures = 0;
    uint64_t totalBytes = 0;
    const uint64_t t0 = ipc::nowMs();
    for (uint64_t i = 0; i < count; ++i) {
        // Up to the default segment size, so every request can be met once the host releases.
        const uint32_t bytes = 4 + static_cast<uint32_t>(rng.next() % (cfg.heap.segmentBytes - 4));
        ipc::HeapRef ref;
        const Result r = s->heap().allocate(bytes, ref, 30000);
        if (r != Result::Success) {
            ++allocFailures;
            break;
        }
        totalBytes += bytes;
        uint8_t* p = s->heap().data(ref);
        for (uint32_t k = 0; k + 4 <= bytes; k += 4) {
            const uint32_t v = pattern(uint32_t(i), k);
            std::memcpy(p + k, &v, 4);
        }
        schema::cmd::Bridge_Response msg;
        msg.payload.resize(8);
        std::memcpy(msg.payload.data(), &ref.firstChunk, 4);
        std::memcpy(msg.payload.data() + 4, &ref.bytes, 4);
        if (s->send(msg, uint32_t(i), ipc::kDataInSharedHeap, 30000) != Result::Success) {
            s->heap().freeLocal(ref);
            ++allocFailures;
            break;
        }
    }
    answers.join();
    const ipc::HeapStats st = s->heap().stats();
    s->close();
    int code = -1;
    CHECK_RESULT(child.wait(30000, &code), Result::Success);
    CHECK(code == 0);
    CHECK(allocFailures == 0);
    CHECK(badAnswers.load() == 0);
    CHECK(answered.load() == count);
    CHECK(st.segments >= 2);  // grew beyond the first segment
    std::printf("rl_bridge_ipc_heap: %" PRIu64 " blocks (%" PRIu64 " MiB) through a %u KiB heap (%u segments) in %" PRIu64
                " ms, %d failures\n",
                answered.load(), totalBytes >> 20, cfg.heap.maxBytes >> 10, st.segments, ipc::nowMs() - t0, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ---- peer death -------------------------------------------------------------------------------

// Host role that handshakes and then never reads (so the client's queues fill up).
int childIdleHost(const std::string& name) {
    std::unique_ptr<ipc::Session> s;
    Result r = ipc::Session::open(name, 10000, s);
    if (r == Result::Success) {
        r = s->handshake(10000);
    }
    if (r != Result::Success) {
        return 2;
    }
    for (;;) {
        ipc::sleepMs(1000);
    }
}

// Client role that creates the session, handshakes and idles.
int childIdleClient(const std::string& name) {
    ipc::SessionConfig cfg = fuzzConfig(ipc::QueueKind::Atomic);
    std::unique_ptr<ipc::Session> s;
    if (ipc::Session::create(name, cfg, s) != Result::Success || s->handshake(10000) != Result::Success) {
        return 2;
    }
    for (;;) {
        ipc::sleepMs(1000);
    }
}

struct Blocked {
    std::atomic<bool> started {false};
    std::atomic<bool> done {false};
    Result result = Result::Success;
    uint64_t returnedAt = 0;
};

template <class Fn>
std::thread block(Blocked& b, Fn&& fn) {
    return std::thread([&b, fn] {
        b.started = true;
        b.result = fn();
        b.returnedAt = ipc::nowMs();
        b.done = true;
    });
}

void checkDetection(const char* what, Blocked& b, uint64_t killedAt) {
    CHECK(b.done.load());
    CHECK_RESULT(b.result, Result::PeerDead);
    const uint64_t latency = b.returnedAt >= killedAt ? b.returnedAt - killedAt : 0;
    CHECK(latency < 1000);
    std::printf("  %-44s -> %-8s in %3" PRIu64 " ms\n", what, ipc::toString(b.result), latency);
}

void peerDeathAsClient(ipc::QueueKind kind) {
    ipc::SessionConfig cfg = fuzzConfig(kind);
    cfg.clientToHostSlots = 8;
    cfg.heap.chunkBytes = 4096;
    cfg.heap.segmentBytes = 64 * 1024;
    cfg.heap.maxBytes = 64 * 1024;
    const std::string name = uniqueName("death-c");
    std::unique_ptr<ipc::Session> s;
    CHECK_RESULT(ipc::Session::create(name, cfg, s), Result::Success);
    if (!s) {
        return;
    }
    ipc::ChildProcess child;
    CHECK_RESULT(spawnChild(child, {"idle-host", name}), Result::Success);
    CHECK_RESULT(s->handshake(20000), Result::Success);
    // The host may still be consuming Continue: wait until it is Running and has stopped reading.
    for (int i = 0; i < 5000 && s->peerState() != ipc::PeerState::Running; ++i) {
        ipc::sleepMs(1);
    }
    CHECK(s->peerState() == ipc::PeerState::Running);
    // Fill the command queue and the heap so the blocked calls below really wait.
    while (s->send(schema::cmd::IDirect3DDevice9Ex_EndScene {}, 0, 0, ipc::kNoWait) == Result::Success) {
    }
    ipc::HeapRef ref;
    while (s->heap().allocate(4096, ref, ipc::kNoWait) == Result::Success) {
    }
    // Two sessions are single-producer/single-consumer: one thread receives, one sends, and the
    // heap wait runs on a third (the heap is only touched by that thread from here on).
    Blocked rx, tx, heap;
    std::thread t1 = block(rx, [&] {
        ipc::Session::Incoming in;
        return s->receive(in, ipc::kInfinite);
    });
    std::thread t2 = block(tx, [&] { return s->send(schema::cmd::IDirect3DDevice9Ex_EndScene {}, 0, 0, ipc::kInfinite); });
    std::thread t3 = block(heap, [&] {
        ipc::HeapRef r;
        return s->heap().allocate(4096, r, ipc::kInfinite);
    });
    ipc::sleepMs(150);
    CHECK(!rx.done.load() && !tx.done.load() && !heap.done.load());
    const uint64_t killedAt = ipc::nowMs();
    child.kill();
    t1.join();
    t2.join();
    t3.join();
    const char* k = kind == ipc::QueueKind::Atomic ? "atomic" : "blocking";
    checkDetection((std::string("client/") + k + ": blocked receive").c_str(), rx, killedAt);
    checkDetection((std::string("client/") + k + ": blocked send (queue full)").c_str(), tx, killedAt);
    checkDetection((std::string("client/") + k + ": blocked heap allocate (heap full)").c_str(), heap, killedAt);
    CHECK(!s->peerAlive());
    int code = 0;
    CHECK_RESULT(child.wait(5000, &code), Result::Success);
}

void peerDeathAsHost() {
    const std::string name = uniqueName("death-h");
    ipc::ChildProcess child;
    CHECK_RESULT(spawnChild(child, {"idle-client", name}), Result::Success);
    std::unique_ptr<ipc::Session> s;
    CHECK_RESULT(ipc::Session::open(name, 20000, s), Result::Success);
    if (!s) {
        return;
    }
    CHECK_RESULT(s->handshake(20000), Result::Success);
    Blocked rx;
    std::thread t1 = block(rx, [&] {
        ipc::Session::Incoming in;
        return s->receive(in, ipc::kInfinite);
    });
    ipc::sleepMs(150);
    CHECK(!rx.done.load());
    const uint64_t killedAt = ipc::nowMs();
    child.kill();
    t1.join();
    checkDetection("host: blocked receive", rx, killedAt);
    CHECK(!s->peerAlive());
    int code = 0;
    CHECK_RESULT(child.wait(5000, &code), Result::Success);
}

int runPeerDeath() {
    std::printf("rl_bridge_ipc_peer_death: detection latency after the peer was killed\n");
    peerDeathAsClient(ipc::QueueKind::Atomic);
    peerDeathAsClient(ipc::QueueKind::Blocking);
    peerDeathAsHost();
    std::printf("rl_bridge_ipc_peer_death: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

uint64_t argCount(int argc, char** argv, const char* flag, uint64_t fallback) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return fallback;
}

bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_self = ipc::selfExecutablePath();
    const std::string mode = argc > 1 ? argv[1] : "unit";
    if (mode == "unit") {
        return runUnit();
    }
    if (mode == "fuzz") {
        const uint64_t count = argCount(argc, argv, "--count", 1000000);
        return runFuzz(count, hasFlag(argc, argv, "--blocking") ? ipc::QueueKind::Blocking : ipc::QueueKind::Atomic);
    }
    if (mode == "heap") {
        return runHeap(argCount(argc, argv, "--count", 2000));
    }
    if (mode == "peer-death") {
        return runPeerDeath();
    }
    if (mode == "child" && argc >= 4) {
        const std::string role = argv[2];
        const std::string name = argv[3];
        if (role == "echo") {
            return childEcho(name);
        }
        if (role == "heap") {
            return childHeap(name);
        }
        if (role == "idle-host") {
            return childIdleHost(name);
        }
        if (role == "idle-client") {
            return childIdleClient(name);
        }
    }
    std::fprintf(stderr, "usage: %s unit | fuzz [--count N] [--blocking] | heap [--count N] | peer-death\n", argv[0]);
    return 2;
}
