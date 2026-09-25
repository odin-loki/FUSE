// FUSE Relight RL-2.1: bridge IPC core — result codes and wait constants.
// Copyright (c) 2026 FUSE contributors (MIT). Semantics follow dxvk-remix bridge/src/util/util_common.h
// (Result::{Success,Timeout,Failure}); FUSE adds the peer, version and validation outcomes.
#pragma once

#include <cstdint>

namespace fuse::relight::bridge::ipc {

enum class Result : uint32_t {
    Success = 0,
    Timeout,          // the wait ran out (queue full/empty, heap exhausted, no handshake)
    Failure,          // an OS call failed
    PeerDead,         // the process at the other end is gone (crash, kill)
    PeerClosed,       // the other end closed the session gracefully
    VersionMismatch,  // handshake: protocol major, schema hash or layout version differ
    TooLarge,         // a payload exceeds the data ring's record limit (use the shared heap)
    Exists,           // create: the named object already exists
    NotFound,         // open: the named object does not exist (yet)
    Malformed,        // shared state or a message failed validation
};

const char* toString(Result r) noexcept;

inline constexpr bool succeeded(Result r) noexcept { return r == Result::Success; }

inline constexpr bool isPowerOfTwo(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

// Timeouts are in milliseconds. kNoWait tries once; kInfinite waits until success or peer loss.
inline constexpr uint32_t kNoWait = 0;
inline constexpr uint32_t kInfinite = 0xFFFFFFFFu;

}  // namespace fuse::relight::bridge::ipc
