// FUSE Relight options: map an options-menu exit reason to a process status code.
//
// Ok and Saved both succeed. Cancelled and Invalid are distinct non-zero statuses.
#pragma once

namespace fuse::relight::options {

enum class OptionsExit { Ok, Saved, Cancelled, Invalid };

/// Process status for an options session. Ok and Saved are 0, Cancelled is 1, Invalid is 2.
[[nodiscard]] inline int options_exit_code(OptionsExit reason) {
    switch (reason) {
    case OptionsExit::Ok:
        return 0;
    case OptionsExit::Saved:
        return 0;
    case OptionsExit::Cancelled:
        return 1;
    case OptionsExit::Invalid:
        return 2;
    }
    return 2;
}

} // namespace fuse::relight::options
