#pragma once

namespace fuse::renderer {

/// True only for M values 1 through 32 inclusive.
[[nodiscard]] inline bool restir_m_ok(int m) {
    return m >= 1 && m <= 32;
}

} // namespace fuse::renderer
