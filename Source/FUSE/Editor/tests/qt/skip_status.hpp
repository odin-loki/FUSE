#pragma once

namespace fuse::editor {

/// True only for the status code 77.
[[nodiscard]] inline bool skip_status_ok(int status) {
    return status == 77;
}

} // namespace fuse::editor
