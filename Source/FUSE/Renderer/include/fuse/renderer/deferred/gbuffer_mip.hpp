#pragma once

namespace fuse::renderer {

/// True only for mip counts 1 through 12 inclusive.
[[nodiscard]] inline bool gbuffer_mip_ok(int mips) {
    return mips >= 1 && mips <= 12;
}

} // namespace fuse::renderer
