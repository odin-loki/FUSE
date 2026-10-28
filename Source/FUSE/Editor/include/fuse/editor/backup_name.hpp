#pragma once

#include <string>
#include <string_view>

namespace fuse::editor {

/// Backup path for a scene file.
/// An empty path stays empty. Otherwise ".bak" is appended and the existing
/// extension is left in place (`Town.fuse` becomes `Town.fuse.bak`).
[[nodiscard]] inline std::string backup_name(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    std::string result(path);
    result.append(".bak");
    return result;
}

} // namespace fuse::editor
