// FUSE Relight RL-2.3: bridge host tests on the model backend (any platform; Wine in the MinGW tree).
// Copyright (c) 2026 FUSE contributors (MIT).
//
//   unit                 protocol rules, fault parsing, journal compaction (bounded, and replaying
//                        the compacted journal reproduces the model state), heap-payload path
//   crash <scenario>     a test-local client drives this executable, started as the host
//                        ("--bridge-session <name>", i.e. the fuse_relight_host command line) with a model
//                        backend, through BridgeLink; every frame's digest is checked against a
//                        reference model fed the same commands. Scenarios:
//                          none         no failure; graceful Terminate, host exits 0
//                          kill         the client kills the host between frames (mid-run)
//                          kill-async   another thread kills the host at a random moment
//                          crash        the host aborts at its 12th Present (--test-fault crash@12)
//                          hang         the host stops responding at Present 12 (hang detection)
//                          exit         the host exits (closes the session) at Present 12
//                          nohost       the host executable does not exist (spawn failure)
//                          mismatch     the host runs a different schema hash (handshake refusal)
//                        Every scenario must end with all frames verified and, except `none`,
//                        exactly one fallback to passthrough.
//   --bridge-session <name> ... host mode (the fuse_relight_host command line, model backend)

#include "model_backend.hpp"

#include <fuse/relight/bridge/host/host_loop.hpp>
#include <fuse/relight/bridge/host/journal.hpp>
#include <fuse/relight/bridge/host/link.hpp>
#include <fuse/relight/bridge/host/protocol.hpp>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>

using namespace fuse::relight::bridge;
using rltest::ModelBackend;

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_self;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                \
    } while (0)

template <class Cmd>
std::vector<uint8_t> encode(const Cmd& c) {
    std::vector<uint8_t> b(schema::cmd::encodedSize(c));
    ipc::WireWriter w(b.data(), b.size());
    schema::cmd::encode(w, c);
    return b;
}

// ---- a deterministic command stream (the "game") -----------------------------------------------

constexpr uint32_t kD3D = 1;
constexpr uint32_t kDevice = 2;

// Sends each command to a sink (the link, a journal, a model) and to the reference model.
class Game {
public:
    explicit Game(uint32_t seed) : rng_(seed) {}

    template <class Sink>
    void setup(Sink&& sink) {
        schema::cmd::IDirect3D9Ex_CreateDevice cd;
        cd.result = kDevice;
        sink(cd, kD3D);
        for (int i = 0; i < 4; ++i) {
            createTexture(sink);
        }
        for (uint32_t i = 0; i < 2; ++i) {
            schema::cmd::IDirect3DDevice9Ex_CreateVertexBuffer vb;
            vb.length = 64;
            vb.result = nextHandle_++;
            buffers_.push_back(vb.result);
            sink(vb, kDevice);
        }
        schema::cmd::IDirect3DDevice9Ex_SetStreamSource ss;
        ss.vertexBuffer = buffers_[0];
        ss.stride = 16;
        sink(ss, kDevice);
    }

    // One frame: state, uploads, churn, state blocks, draws; digest query; Present.
    template <class Sink, class Query>
    void frame(Sink&& sink, Query&& query) {
        const int ops = 4 + static_cast<int>(rng_() % 8);
        for (int i = 0; i < ops; ++i) {
            switch (rng_() % 9) {
            case 0:
            case 1: {
                schema::cmd::IDirect3DDevice9Ex_SetRenderState c;
                c.state = rng_() % 6;
                c.value = rng_() % 4;
                sink(c, kDevice);
                break;
            }
            case 2: {
                schema::cmd::IDirect3DDevice9Ex_SetTexture c;
                c.stage = rng_() % 2;
                c.texture = rng_() % 4 == 0 ? 0 : textures_[rng_() % textures_.size()];
                sink(c, kDevice);
                break;
            }
            case 3: {
                const uint32_t t = textures_[rng_() % textures_.size()];
                const bool whole = rng_() % 3 == 0;
                int32_t r[4] = {int32_t(rng_() % 2), int32_t(rng_() % 2), 0, 0};
                r[2] = r[0] + 1 + int32_t(rng_() % 3);
                r[3] = r[1] + 1 + int32_t(rng_() % 3);
                const uint32_t w = whole ? 4 : uint32_t(r[2] - r[0]), h = whole ? 4 : uint32_t(r[3] - r[1]);
                schema::cmd::IDirect3DTexture9_UnlockRect un;
                if (!whole) {
                    un.rect.assign(r, r + 4);
                }
                un.rowBytes = w * 4;
                un.rows = h;
                un.data.resize(size_t(w) * h * 4);
                for (auto& b : un.data) {
                    b = uint8_t(rng_());
                }
                sink(un, t);
                break;
            }
            case 4: {
                schema::cmd::IDirect3DVertexBuffer9_Unlock c;
                const bool discard = rng_() % 4 == 0;
                c.offset = discard ? 0 : uint32_t(rng_() % 4) * 16;
                c.flags = discard ? 0x2000u : 0u;
                c.data.resize(16);
                for (auto& b : c.data) {
                    b = uint8_t(rng_());
                }
                sink(c, buffers_[rng_() % buffers_.size()]);
                break;
            }
            case 5: {
                // Churn: a new texture replaces an old one (which may still be bound).
                createTexture(sink);
                const size_t victim = rng_() % (textures_.size() - 1);
                schema::cmd::IDirect3DResource9_Destroy d;
                sink(d, textures_[victim]);
                textures_.erase(textures_.begin() + static_cast<std::ptrdiff_t>(victim));
                break;
            }
            case 6: {
                if (blocks_.size() < 3) {
                    sink(schema::cmd::IDirect3DDevice9Ex_BeginStateBlock {}, kDevice);
                    schema::cmd::IDirect3DDevice9Ex_SetRenderState c;
                    c.state = rng_() % 6;
                    c.value = 10 + rng_() % 4;
                    sink(c, kDevice);
                    schema::cmd::IDirect3DDevice9Ex_SetTexture t;
                    t.stage = 1;
                    t.texture = textures_[rng_() % textures_.size()];
                    sink(t, kDevice);
                    schema::cmd::IDirect3DDevice9Ex_EndStateBlock e;
                    e.result = nextHandle_++;
                    blocks_.push_back(e.result);
                    sink(e, kDevice);
                } else {
                    const size_t victim = rng_() % blocks_.size();
                    sink(schema::cmd::IDirect3DStateBlock9_Destroy {}, blocks_[victim]);
                    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(victim));
                }
                break;
            }
            case 7:
                if (!blocks_.empty()) {
                    sink(schema::cmd::IDirect3DStateBlock9_Apply {}, blocks_[rng_() % blocks_.size()]);
                }
                break;
            default:
                if (!blocks_.empty()) {
                    sink(schema::cmd::IDirect3DStateBlock9_Capture {}, blocks_[rng_() % blocks_.size()]);
                }
                break;
            }
        }
        sink(schema::cmd::IDirect3DDevice9Ex_BeginScene {}, kDevice);
        schema::cmd::IDirect3DDevice9Ex_Clear clear;
        clear.color = uint32_t(rng_());
        sink(clear, kDevice);
        for (int d = 0; d < 2; ++d) {
            schema::cmd::IDirect3DDevice9Ex_DrawPrimitive draw;
            draw.primitiveCount = 1 + d;
            sink(draw, kDevice);
            schema::cmd::IDirect3DDevice9Ex_SetRenderState c;
            c.state = rng_() % 6;
            c.value = rng_() % 4;
            sink(c, kDevice);
        }
        sink(schema::cmd::IDirect3DDevice9Ex_EndScene {}, kDevice);
        query();
        sink(schema::cmd::IDirect3DDevice9Ex_Present {}, kDevice);
    }

private:
    template <class Sink>
    void createTexture(Sink&& sink) {
        schema::cmd::IDirect3DDevice9Ex_CreateTexture c;
        c.width = 4;
        c.height = 4;
        c.levels = 1;
        c.result = nextHandle_++;
        textures_.push_back(c.result);
        sink(c, kDevice);
    }

    std::mt19937 rng_;
    uint32_t nextHandle_ = 100;
    std::vector<uint32_t> textures_;
    std::vector<uint32_t> buffers_;
    std::vector<uint32_t> blocks_;
};

uint64_t modelDigestQuery(ModelBackend& m) {
    host::Response r;
    const auto b = encode(schema::cmd::IDirect3DDevice9Ex_GetRenderTargetData {});
    m.execute(static_cast<uint16_t>(schema::CommandId::IDirect3DDevice9Ex_GetRenderTargetData), 0, kDevice, b.data(),
              b.size(), &r);
    uint64_t d = 0;
    if (r.payload.size() == 8) {
        std::memcpy(&d, r.payload.data(), 8);
    }
    return d;
}

// ---- unit ---------------------------------------------------------------------------------------

void testProtocol() {
    using schema::CommandId;
    using host::ReplyKind;
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_CreateDevice) == ReplyKind::None);
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_CreateDevice, host::kWantReply) == ReplyKind::Result);
    CHECK(host::replyKind(CommandId::Direct3DCreate9) == ReplyKind::Result);
    CHECK(host::replyKind(CommandId::Bridge_Sync) == ReplyKind::Result);
    CHECK(host::replyKind(CommandId::IDirect3DDevice9Ex_GetRenderTargetData) == ReplyKind::Data);
    CHECK(host::replyKind(CommandId::IDirect3DTexture9_LockRect) == ReplyKind::Data);
    CHECK(host::replyKind(CommandId::IDirect3DVertexBuffer9_Lock) == ReplyKind::Data);
    CHECK(host::replyKind(CommandId::IDirect3DQuery9_GetData) == ReplyKind::Data);
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_GetDeviceCaps) == ReplyKind::Caps);
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_GetAdapterIdentifier) == ReplyKind::AdapterIdentifier);
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_GetAdapterLUID) == ReplyKind::Luid);
    CHECK(host::replyKind(CommandId::IDirect3DSwapChain9_GetRasterStatus) == ReplyKind::RasterStatus);
    CHECK(host::replyKind(CommandId::IDirect3DSwapChain9_GetDisplayMode) == ReplyKind::DisplayMode);
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_GetAdapterCount) == ReplyKind::Value);
    CHECK(host::replyKind(CommandId::IDirect3DDevice9Ex_IsSupportedSurfaceFormat) == ReplyKind::Value);
    CHECK(host::replyKind(CommandId::IDirect3D9Ex_CheckDeviceFormat) == ReplyKind::Result);
    CHECK(host::replyKind(CommandId::IDirect3DDevice9Ex_Present) == ReplyKind::None);
    CHECK(host::replyKind(CommandId::IDirect3DDevice9Ex_Present, host::kWantReply) == ReplyKind::Result);
    CHECK(host::replyKind(CommandId::IDirect3DDevice9Ex_SetRenderState) == ReplyKind::None);
    CHECK(host::replyKind(CommandId::Bridge_Terminate) == ReplyKind::None);
    CHECK(host::replyKind(CommandId::RemixApi_CreateMesh) == ReplyKind::None);
    CHECK(host::replyKind(CommandId::Reply_Result, host::kWantReply) == ReplyKind::None);
    CHECK(host::isReplyCommand(static_cast<uint16_t>(CommandId::Reply_Data)));
    CHECK(!host::isReplyCommand(static_cast<uint16_t>(CommandId::Bridge_Sync)));
    CHECK(host::replyCommand(ReplyKind::Caps) == static_cast<uint16_t>(CommandId::Reply_Caps));
    CHECK(host::isPresentCommand(static_cast<uint16_t>(CommandId::IDirect3DDevice9Ex_PresentEx)));
    CHECK(host::isPresentCommand(static_cast<uint16_t>(CommandId::IDirect3DSwapChain9_Present)));
    CHECK(!host::isPresentCommand(static_cast<uint16_t>(CommandId::IDirect3DDevice9Ex_EndScene)));
    CHECK(host::methodName(static_cast<uint16_t>(CommandId::IDirect3DDevice9Ex_Clear)) == "Clear");
    CHECK(host::interfaceName(static_cast<uint16_t>(CommandId::IDirect3DDevice9Ex_Clear)) == "IDirect3DDevice9Ex");
    CHECK(host::replyKind(uint16_t(0), 0) == ReplyKind::None && host::replyKind(uint16_t(0xFFFF), host::kWantReply) == ReplyKind::None);
    // The flag bit must not collide with the IPC core's.
    CHECK((uint32_t(host::kWantReply) & uint32_t(ipc::kDataInSharedHeap)) == 0);
    // Default replies decode as their struct, with the patched uid.
    {
        std::vector<uint8_t> b = host::encodeDefaultReply(ReplyKind::DisplayMode, host::kHrInvalidCall);
        host::patchReplyUid(b, 77);
        schema::cmd::Reply_DisplayMode r;
        ipc::WireReader rd(b.data(), b.size());
        CHECK(schema::cmd::decode(rd, r) && rd.atEnd() && r.requestUid == 77 && r.hresult == host::kHrInvalidCall);
    }

    host::HostFault f = host::HostFault::None;
    uint32_t n = 0;
    CHECK(host::parseHostFault("crash@3", f, n) && f == host::HostFault::Crash && n == 3);
    CHECK(host::parseHostFault("hang@12", f, n) && f == host::HostFault::Hang && n == 12);
    CHECK(host::parseHostFault("exit@1", f, n) && f == host::HostFault::Exit && n == 1);
    CHECK(!host::parseHostFault("crash@0", f, n));
    CHECK(!host::parseHostFault("boom@3", f, n));
    CHECK(!host::parseHostFault("crash@3x", f, n));
    CHECK(!host::parseHostFault("crash", f, n));
}

// Replaying the compacted journal into a fresh model reproduces the model fed every command, at every
// frame boundary and mid-frame, and compaction keeps the journal bounded.
void testJournalReplay() {
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        Game game(seed);
        ModelBackend reference;
        host::CommandJournal journal;
        auto sink = [&](const auto& c, uint32_t handle) {
            const auto b = encode(c);
            const uint16_t id = static_cast<uint16_t>(std::decay_t<decltype(c)>::kId);
            reference.execute(id, 0, handle, b.data(), b.size(), nullptr);
            journal.append(id, handle, b.data(), b.size());
        };
        game.setup(sink);
        size_t maxEntries = 0;
        bool allEqual = true;
        for (int f = 0; f < 120; ++f) {
            game.frame(sink, [&] {
                // Mid-frame (after the draws, before Present): replay what the journal holds now.
                ModelBackend replayed;
                journal.replay([&](const host::JournalEntry& e) {
                    replayed.execute(e.command, 0, e.handle, e.payload.data(), e.payload.size(), nullptr);
                });
                const uint64_t a = modelDigestQuery(reference);
                const uint64_t b = modelDigestQuery(replayed);
                if (a != b && allEqual) {
                    std::fprintf(stderr, "journal replay differs: seed %u frame %d (%zu entries)\n", seed, f, journal.size());
                }
                allEqual = allEqual && a == b && replayed.unknownHandles == 0;
            });
            maxEntries = std::max(maxEntries, journal.size());
        }
        CHECK(allEqual);
        CHECK(journal.enabled());
        // Bounded: 120 frames of churn, but live state is a few dozen objects + keys.
        CHECK(maxEntries < 400);
        if (seed == 1) {
            const host::JournalStats& s = journal.stats();
            std::printf("journal (seed 1): %zu entries, %zu bytes after 120 frames; dropped %" PRIu64 " transient, %" PRIu64
                        " superseded, %" PRIu64 " destroyed; %" PRIu64 " queries skipped\n",
                        journal.size(), journal.bytes(), s.droppedTransient, s.droppedSuperseded, s.droppedDestroyed,
                        s.skipped);
        }
    }
}

void testJournalLimit() {
    host::CommandJournal journal(4096);
    schema::cmd::IDirect3DVertexBuffer9_Unlock c;
    c.data.resize(1000);
    bool ok = true;
    for (uint32_t i = 0; i < 10 && ok; ++i) {
        c.offset = i * 1000;  // distinct keys: nothing supersedes
        const auto b = encode(c);
        ok = journal.append(static_cast<uint16_t>(c.kId), 7, b.data(), b.size());
    }
    CHECK(!ok);
    CHECK(!journal.enabled());
    CHECK(journal.size() == 0);
}

// The host loop answers with the request's uid in the header handle, resolves shared-heap payloads,
// and returns on Terminate. In-process: the host loop runs on a thread.
void testHostLoopInProcess() {
    const std::string name = "rl-host-unit-" + std::to_string(ipc::currentPid());
    ipc::SessionConfig cfg;
    cfg.heap.maxBytes = 8u << 20;
    cfg.heap.segmentBytes = 1u << 20;
    std::unique_ptr<ipc::Session> client;
    CHECK(ipc::Session::create(name, cfg, client) == ipc::Result::Success);
    if (!client) {
        return;
    }
    int hostCode = -1;
    host::HostLoopStats hostStats;
    std::thread hostThread([&] {
        std::unique_ptr<ipc::Session> s;
        if (ipc::Session::open(name, 5000, s) != ipc::Result::Success || s->handshake(5000) != ipc::Result::Success) {
            return;
        }
        ModelBackend model;
        hostCode = host::runHostLoop(*s, model, host::HostLoopOptions {}, &hostStats);
    });
    CHECK(client->handshake(5000) == ipc::Result::Success);

    host::LinkConfig lc;
    lc.hangTimeoutMs = 5000;
    lc.heapThresholdBytes = 256;  // push the big uploads through the shared heap (per-field heapChunk/heapBytes)
    host::BridgeLink link(lc, [](std::string&) { return std::unique_ptr<host::ILocalBackend>(new ModelBackend); });
    link.attach(std::move(client), {});
    ModelBackend reference;
    Game game(7);
    auto sink = [&](const auto& c, uint32_t handle) {
        const auto b = encode(c);
        reference.execute(static_cast<uint16_t>(std::decay_t<decltype(c)>::kId), 0, handle, b.data(), b.size(), nullptr);
        link.call(c, handle);
    };
    game.setup(sink);
    // A big payload (> threshold) through the heap.
    schema::cmd::IDirect3DVertexBuffer9_Unlock big;
    big.data.assign(4096, 0x5A);
    big.offset = 0;
    sink(big, 100 + 4);  // handle of the first vertex buffer (after 4 textures)
    bool same = true;
    for (int f = 0; f < 20; ++f) {
        game.frame(sink, [&] {
            host::Response r;
            link.call(schema::cmd::IDirect3DDevice9Ex_GetRenderTargetData {}, kDevice, &r);
            uint64_t d = 0;
            if (r.payload.size() == 8) {
                std::memcpy(&d, r.payload.data(), 8);
            }
            same = same && d == modelDigestQuery(reference);
        });
    }
    CHECK(same);
    CHECK(link.mode() == host::LinkMode::Bridged);
    link.shutdown();
    hostThread.join();
    CHECK(hostCode == host::kHostExitOk);
    CHECK(hostStats.presents == 20);
    CHECK(hostStats.responses >= 40);  // 20 digests + 20 Presents (kWantReply)
}

int runUnit() {
    testProtocol();
    testJournalReplay();
    testJournalLimit();
    testHostLoopInProcess();
    std::printf("rl_bridge_host_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ---- crash scenarios ----------------------------------------------------------------------------

int runCrash(const std::string& scenario) {
    constexpr int kFrames = 40;
    constexpr int kKillFrame = 15;
    host::LinkConfig lc;
    lc.hangTimeoutMs = scenario == "hang" ? 1500 : 10000;
    lc.startupTimeoutMs = scenario == "nohost" ? 3000 : 10000;
    std::atomic<int> backendsBuilt {0};
    host::BridgeLink link(lc, [&](std::string&) {
        ++backendsBuilt;
        return std::unique_ptr<host::ILocalBackend>(new ModelBackend);
    });
    host::HostLaunch hl;
    hl.hostExe = scenario == "nohost" ? g_self + ".missing" : g_self;
    hl.session.heap.maxBytes = 0;
    if (scenario == "crash") {
        hl.extraArgs = {"--test-fault", "crash@12"};
    } else if (scenario == "hang") {
        hl.extraArgs = {"--test-fault", "hang@12"};
    } else if (scenario == "exit") {
        hl.extraArgs = {"--test-fault", "exit@12"};
    } else if (scenario == "mismatch") {
        hl.extraArgs = {"--advertise-hash", "1"};
    }
    host::FallbackReason seenReason = host::FallbackReason::None;
    link.onFallback = [&](host::FallbackReason r) { seenReason = r; };
    const uint64_t t0 = ipc::nowMs();
    const ipc::Result lr = link.launch(hl);
    std::printf("launch: %s (%s)\n", ipc::toString(lr), host::toString(link.mode()));

    std::thread killer;
    std::atomic<bool> stop {false};
    std::atomic<int> frameNo {0};
    std::atomic<bool> killed {false};
    if (scenario == "kill-async") {
        // Kill from another thread at an arbitrary point after frame 10 starts (mid-command stream);
        // the client does not wait for it, only frame 14 waits until the kill was issued.
        killer = std::thread([&] {
            while (!stop && frameNo.load() < 10) {
                ipc::sleepMs(0);
            }
            if (!stop && link.hostProcess() != nullptr) {
                link.hostProcess()->kill();
            }
            killed = true;
        });
    }

    ModelBackend reference;
    Game game(1234);
    auto sink = [&](const auto& c, uint32_t handle) {
        const auto b = encode(c);
        reference.execute(static_cast<uint16_t>(std::decay_t<decltype(c)>::kId), 0, handle, b.data(), b.size(), nullptr);
        link.call(c, handle);
    };
    game.setup(sink);
    int verified = 0;
    for (int f = 0; f < kFrames; ++f) {
        frameNo = f;
        for (int w = 0; scenario == "kill-async" && f == 14 && !killed && w < 1000; ++w) {
            ipc::sleepMs(1);
        }
        if (scenario == "kill" && f == kKillFrame && link.hostProcess() != nullptr) {
            link.hostProcess()->kill();  // between frames, with commands possibly still queued
        }
        game.frame(sink, [&] {
            host::Response r;
            const int32_t hr = link.call(schema::cmd::IDirect3DDevice9Ex_GetRenderTargetData {}, kDevice, &r);
            uint64_t d = 0;
            if (hr == host::kHrOk && r.payload.size() == 8) {
                std::memcpy(&d, r.payload.data(), 8);
            }
            const uint64_t want = modelDigestQuery(reference);
            if (d == want) {
                ++verified;
            } else {
                std::fprintf(stderr, "frame %d: digest mismatch (%s, hr 0x%08x)\n", f, host::toString(link.mode()),
                             static_cast<uint32_t>(hr));
            }
        });
    }
    stop = true;
    if (killer.joinable()) {
        killer.join();
    }
    const host::LinkMode endMode = link.mode();
    link.shutdown();
    int hostExit = -1;
    if (link.hostProcess() != nullptr) {
        link.hostProcess()->wait(5000, &hostExit);
    }
    std::printf("scenario %s: %d/%d frames verified, mode %s, %u fallback(s) (%s), %" PRIu64 " commands replayed in %" PRIu64
                " ms, host exit %d, %" PRIu64 " ms total\n",
                scenario.c_str(), verified, kFrames, host::toString(endMode), link.fallbacks(), host::toString(link.reason()),
                link.replayedCommands(), link.fallbackMs(), hostExit, ipc::nowMs() - t0);
    CHECK(verified == kFrames);
    if (scenario == "none") {
        CHECK(endMode == host::LinkMode::Bridged);
        CHECK(link.fallbacks() == 0);
        CHECK(hostExit == host::kHostExitOk);
        CHECK(backendsBuilt == 0);
    } else {
        CHECK(endMode == host::LinkMode::Passthrough);
        CHECK(link.fallbacks() == 1);
        CHECK(backendsBuilt == 1);
        const host::FallbackReason want = scenario == "kill" || scenario == "kill-async" || scenario == "crash"
                                              ? host::FallbackReason::HostDied
                                          : scenario == "hang"     ? host::FallbackReason::HostHung
                                          : scenario == "exit"     ? host::FallbackReason::HostClosed
                                          : scenario == "nohost"   ? host::FallbackReason::SpawnFailed
                                          : scenario == "mismatch" ? host::FallbackReason::VersionMismatch
                                                                   : host::FallbackReason::None;
        // A kill racing a response wait can surface as a closed session on some backends.
        // A kill racing a reply wait can surface as a closed session; a missing host fails at exec
        // (handshake) on POSIX and at CreateProcess (spawn) on Windows.
        const bool reasonOk = link.reason() == want ||
                              (want == host::FallbackReason::HostDied && link.reason() == host::FallbackReason::HostClosed) ||
                              (want == host::FallbackReason::SpawnFailed && link.reason() == host::FallbackReason::HandshakeFailed);
        CHECK(reasonOk);
        CHECK(seenReason == link.reason());
        if (scenario != "nohost" && scenario != "mismatch") {
            CHECK(link.replayedCommands() > 0);
        }
    }
    std::printf("rl_bridge_host_crash_%s: %d checks, %d failures\n", scenario.c_str(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// ---- host mode: the same command line as fuse_relight_host.exe, on the model backend ----------

int runModelHost(int argc, char** argv) {
    std::string name;
    host::HostLoopOptions opts;
    uint64_t advertiseHash = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == host::kArgSession) {
            name = argv[++i];
        } else if (a == "--test-fault") {
            host::parseHostFault(argv[++i], opts.fault, opts.faultAtPresent);
        } else if (a == "--advertise-hash") {
            advertiseHash = std::strtoull(argv[++i], nullptr, 10);
        }
    }
    std::unique_ptr<ipc::Session> s;
    if (ipc::Session::open(name, 10000, s) != ipc::Result::Success) {
        return host::kHostExitSession;
    }
    if (advertiseHash != 0) {
        s->setAdvertised(schema::kProtocolMajor, schema::kProtocolMinor, advertiseHash);
    }
    if (s->handshake(10000) != ipc::Result::Success) {
        return host::kHostExitSession;
    }
    ModelBackend model;
    return host::runHostLoop(*s, model, opts);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_self = ipc::selfExecutablePath();
    const std::string mode = argc > 1 ? argv[1] : "unit";
    if (mode == host::kArgSession) {
        return runModelHost(argc, argv);
    }
    if (mode == "unit") {
        return runUnit();
    }
    if (mode == "crash" && argc > 2) {
        return runCrash(argv[2]);
    }
    std::fprintf(stderr, "usage: %s unit | crash none|kill|kill-async|crash|hang|exit|nohost|mismatch\n", argv[0]);
    return 2;
}
