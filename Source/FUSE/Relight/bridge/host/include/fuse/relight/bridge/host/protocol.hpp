/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (response rules, getPresParamFromRaw,
// ReturnSurfaceDataToClient payload)

// FUSE Relight RL-2.3: the host side of the bridge protocol, on top of the RL-2.1 generated schema and
// the RL-2.2 client contract (bridge/client/include/fuse/relight/bridge/client/protocol.hpp and the
// conventions at the top of commands.table). Plain data + pure functions (no D3D9 or OS headers),
// so the x86 client, the x64 host and the Linux-native unit tests share it.
//
// Replies (upstream: Bridge_Response for create/get calls, optional responses for the rest via
// bridge.conf sendAllServerResponses / sendCreateFunctionServerResponses). The RL-2.2 contract:
// - a command carrying kWantReply (== client::kFlagWantsReply, bit 8) is answered with
//   Reply_Result{requestUid, hresult};
// - the query commands are always answered with their typed reply: the table's "-> Reply_X" row
//   suffix (generated schema::replyOf);
// - read-backs (GetRenderTargetData, GetFrontBufferData, Lock*/LockRect/LockBox, Query GetData)
//   answer with Reply_Data chunks of at most kReplyChunkBytes, tightly packed rows;
// - every reply echoes the request's MessageHeader::uid in requestUid; replies come in request order.
// Resource uploads carry `u32 heapChunk, u32 heapBytes, bytes data`: heapBytes == 0 means inline,
// otherwise the shared-heap run HeapRef{heapChunk, heapBytes} that the host releases after the copy.
#pragma once

#include <fuse/relight/bridge/schema/commands.gen.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fuse::relight::bridge::host {

// MessageHeader::flags bit 8: the value of RL-2.2's client::kFlagWantsReply (RL-2.1 owns bits 0..7).
enum HostFlag : uint16_t {
    kWantReply = 1u << 8,
};

// HRESULTs the bridge produces itself (numeric values of the D3D9/Win32 constants; no windows.h).
inline constexpr int32_t kHrOk = 0;
inline constexpr int32_t kHrNotImpl = static_cast<int32_t>(0x80004001u);      // E_NOTIMPL
inline constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);         // E_FAIL
inline constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);  // E_OUTOFMEMORY
inline constexpr int32_t kHrInvalidCall = static_cast<int32_t>(0x8876086Cu);  // D3DERR_INVALIDCALL
inline constexpr int32_t kHrDeviceLost = static_cast<int32_t>(0x88760868u);   // D3DERR_DEVICELOST

// Reply_Data chunk size (== RL-2.2 client::kReplyChunkMax).
inline constexpr uint32_t kReplyChunkBytes = 256u << 10;

// Method part of a generated command name: "IDirect3DDevice9Ex_Present" -> "Present".
std::string_view methodName(uint16_t command) noexcept;
std::string_view interfaceName(uint16_t command) noexcept;

enum class ReplyKind : uint8_t { None, Result, Value, Caps, DisplayMode, AdapterIdentifier, RasterStatus, Luid, Data };

// How the host answers `command` sent with `flags`.
ReplyKind replyKind(uint16_t command, uint16_t flags) noexcept;
inline ReplyKind replyKind(schema::CommandId id, uint16_t flags = 0) noexcept {
    return replyKind(static_cast<uint16_t>(id), flags);
}
// The Reply_* command id of a kind (Invalid for None).
uint16_t replyCommand(ReplyKind kind) noexcept;
// True for Reply_* ids (every one starts with u32 requestUid, i32 hresult).
bool isReplyCommand(uint16_t command) noexcept;
// Encodes a Reply_* of `kind` with requestUid 0 and the given hresult, all other fields zero (the
// answer to a query the executor could not serve). Data: an empty Reply_Data.
std::vector<uint8_t> encodeDefaultReply(ReplyKind kind, int32_t hresult);
// Overwrites requestUid (the first field of every Reply_*).
void patchReplyUid(std::vector<uint8_t>& encodedReply, uint32_t uid) noexcept;

// Present / PresentEx / IDirect3DSwapChain9_Present: frame boundaries (journal compaction, the
// asynchronous reply window, the host's fault hooks).
bool isPresentCommand(uint16_t command) noexcept;

// Host command line (the RL-2.2 client launches the host):
//   fuse_relight_host.exe --bridge-session <name> [--client-pid <pid>] [--d3d9 <path>]
//                         [--open-timeout <ms>] [--relight 0|1] [--test-fault crash@N|hang@N|exit@N]
//                         [--verbose]            (--session is accepted as an alias)
inline constexpr const char* kArgSession = "--bridge-session";
// Exit codes:
inline constexpr int kHostExitOk = 0;             // Bridge_Terminate or a graceful client close
inline constexpr int kHostExitUsage = 64;
inline constexpr int kHostExitClientDied = 65;    // the client process vanished (upstream OnClientExited)
inline constexpr int kHostExitSession = 66;       // session open / handshake failed
inline constexpr int kHostExitD3D9 = 67;          // d3d9.dll could not be loaded or created no IDirect3D9
inline constexpr int kHostExitProtocol = 68;      // a malformed message
inline constexpr int kHostExitTestFault = 70;     // --test-fault exit@N

}  // namespace fuse::relight::bridge::host
