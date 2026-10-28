#pragma once

namespace fuse::script {

/// True only for budgets from 1 through 100000 microseconds, inclusive.
[[nodiscard]] inline bool script_budget_us_ok(int us) {
    return us >= 1 && us <= 100000;
}

} // namespace fuse::script
