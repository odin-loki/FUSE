#pragma once

namespace fuse::renderer {

[[nodiscard]] inline bool pass_budget_ok(int passes) {
    return passes >= 1 && passes <= 64;
}

} // namespace fuse::renderer
