#pragma once

namespace fuse::script {

/// True only for stack slots 1 through 256 inclusive.
[[nodiscard]] inline bool lua_stack_ok(int slots) {
    return slots >= 1 && slots <= 256;
}

} // namespace fuse::script
