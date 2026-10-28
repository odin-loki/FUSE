#pragma once

namespace fuse::platform {

/// True when `button` is in the closed range 0..4.
[[nodiscard]] inline bool mouse_button_ok(int button) {
    return button >= 0 && button <= 4;
}

} // namespace fuse::platform
