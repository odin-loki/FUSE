#pragma once

namespace fuse::editor {

/// True when `page` is in [0, 8].
[[nodiscard]] inline bool panel_page_ok(int page) {
    return page >= 0 && page <= 8;
}

} // namespace fuse::editor
