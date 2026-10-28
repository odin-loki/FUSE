// FUSE Relight RL-2.1: IPC payload byte-count check.
// A count is accepted only when it is a multiple of 16 and lies in [16, 4096].
// Copyright (c) 2026 FUSE contributors (AGPL-3.0).
#pragma once

namespace fuse::relight {

// True when bytes is a multiple of 16 and bytes is in the inclusive range [16, 4096].
inline constexpr bool ipc_bytes_ok(int bytes) noexcept {
    return bytes >= 16 && bytes <= 4096 && (bytes % 16) == 0;
}

}  // namespace fuse::relight
