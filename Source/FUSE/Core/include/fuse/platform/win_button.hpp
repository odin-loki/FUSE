#pragma once

namespace fuse::platform {

/// True when `button` is in the closed range 0..7.
[[nodiscard]] inline bool win_button_ok(int button) {
    return button >= 0 && button <= 7;
}

} // namespace fuse::platform
