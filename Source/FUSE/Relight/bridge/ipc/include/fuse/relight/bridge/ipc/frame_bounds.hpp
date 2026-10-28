// FUSE Relight RL-2.1: IPC frame size check.
// A frame is 4 bytes of size, 4 bytes of type, then the payload. An empty payload or a zero max is
// rejected; a payload is accepted only when it fits in the caller's maximum.
// Copyright (c) 2026 FUSE contributors (AGPL-3.0).
#pragma once

#include <cstdint>

namespace fuse::relight::bridge::ipc {

// True when payloadBytes <= maxPayload, maxPayload > 0, and payloadBytes > 0.
inline constexpr bool ipc_frame_ok(uint32_t payloadBytes, uint32_t maxPayload) noexcept {
    return payloadBytes <= maxPayload && maxPayload > 0 && payloadBytes > 0;
}

// payloadBytes + 8 (4 byte size + 4 byte type). Saturates at UINT32_MAX if the add would wrap.
inline constexpr uint32_t ipc_frame_total(uint32_t payloadBytes) noexcept {
    constexpr uint32_t kHeaderBytes = 8u;
    constexpr uint32_t kMax = 0xFFFFFFFFu;
    if (payloadBytes > kMax - kHeaderBytes) {
        return kMax;
    }
    return payloadBytes + kHeaderBytes;
}

}  // namespace fuse::relight::bridge::ipc
