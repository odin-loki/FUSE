// FUSE Relight options: system slot index.
//
// A slot is accepted only when it is in the inclusive range 0..15.
#pragma once

namespace fuse::relight::options {

/// True only for slots 0 through 15.
[[nodiscard]] inline bool system_slot_ok(int slot) {
    return slot >= 0 && slot <= 15;
}

} // namespace fuse::relight::options
