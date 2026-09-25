// FUSE Relight RL-2.2: bridge client <-> host conventions on top of the RL-2.1 session.
// Copyright (c) 2026 FUSE contributors (AGPL-3.0). New code; the semantics it replaces are named inline
// (dxvk-remix bridge/src/client/d3d9_lss.cpp InitServer, util_bridgecommand.h Flags, client_options.h).
//
// This header is the contract the RL-2.3 host (bridge/host) implements; the RL-2.2 tests carry a
// test-local stub host that implements it too (bridge/client/tests/stub_host.cpp).
//
// Launch. The client creates the session (RL-2.1 Session::create) on the first Direct3DCreate9* and
// starts the host (through RL-2.3's BridgeLink::launch) as
//     <host.exe> --bridge-session <name> --client-pid <pid> [$FUSE_RELIGHT_BRIDGE_HOST_ARGS...]
// where <host.exe> is $FUSE_RELIGHT_BRIDGE_HOST, else fuse_relight_host.exe next to the client DLL.
// If the host cannot be started or dies later, the client continues in-process on the passthrough
// d3d9.dll ($FUSE_RELIGHT_PASSTHROUGH_D3D9, else <client dir>/fuse_relight_passthrough/d3d9.dll).
// Upstream passed "<guid> <version> <game command line>" and compared a version string; the session
// handshake (protocol major + schema hash) replaces that.
//
// Replies. A command sent with kFlagWantsReply gets Reply_Result{requestUid = header.uid}; the query
// commands listed in commands.table always get their typed Reply_*. Replies arrive in request order
// (the client serializes calls), so the client can require requestUid == the uid it sent.
#pragma once

#include <cstdint>

namespace fuse::relight::bridge::client {

// MessageHeader::flags bits used by RL-2.2 (ipc::kDataInSharedHeap is bit 0).
enum ClientFlag : uint16_t {
    kFlagWantsReply = 1u << 8,  // the host answers with Reply_Result (upstream: WAIT_FOR_*_SERVER_RESPONSE)
};

// Uploads above this size go through the shared heap instead of the command ring (LinkConfig).
inline constexpr uint32_t kInlineDataMax = 256u << 10;
// Host -> client readback chunk size (Reply_Data); well under half of the default host->client ring.
inline constexpr uint32_t kReplyChunkMax = 256u << 10;

// Command-line switches of the host.
inline constexpr const char* kArgSession = "--bridge-session";
inline constexpr const char* kArgClientPid = "--client-pid";

// Environment knobs read by the client.
inline constexpr const char* kEnvHostPath = "FUSE_RELIGHT_BRIDGE_HOST";        // host executable
inline constexpr const char* kEnvHostArgs = "FUSE_RELIGHT_BRIDGE_HOST_ARGS";   // extra host arguments (space-separated)
inline constexpr const char* kEnvSyncAll = "FUSE_RELIGHT_BRIDGE_SYNC";        // 1: every call waits (debug)
inline constexpr const char* kEnvTimeoutMs = "FUSE_RELIGHT_BRIDGE_TIMEOUT_MS";  // startup / hang timeout
inline constexpr const char* kEnvLog = "FUSE_RELIGHT_BRIDGE_LOG";              // client log file path

// Sampler index mapping of the shadow state and the wire (D3DDMAPSAMPLER = 256,
// D3DVERTEXTEXTURESAMPLER0..3 = 257..260 map to 16..20).
inline constexpr uint32_t kSamplerCount = 21;
inline constexpr bool samplerSlot(uint32_t sampler, uint32_t& slot) noexcept {
    if (sampler < 16) {
        slot = sampler;
        return true;
    }
    if (sampler >= 256 && sampler <= 260) {
        slot = 16 + (sampler - 256);
        return true;
    }
    return false;
}

}  // namespace fuse::relight::bridge::client
