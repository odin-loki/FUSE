#pragma once

#include <cstddef>
#include <string_view>

namespace fuse::editor {

enum class SceneFileKind { Unknown, Scene, Level, Prefab };

/// Classify a path by its file extension.
/// Matching is case-insensitive: `.fuse` is a scene, `.flevel` is a level, and `.prefab` is a prefab.
/// Any other extension, or a path with no extension, is unknown.
[[nodiscard]] inline SceneFileKind scene_file_kind(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) {
        return SceneFileKind::Unknown;
    }

    const std::string_view ext = name.substr(dot + 1);
    const auto sameExtension = [](std::string_view value, std::string_view expected) {
        if (value.size() != expected.size()) {
            return false;
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(value[i]);
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<unsigned char>(c - 'A' + 'a');
            }
            if (c != static_cast<unsigned char>(expected[i])) {
                return false;
            }
        }
        return true;
    };

    if (sameExtension(ext, "fuse")) {
        return SceneFileKind::Scene;
    }
    if (sameExtension(ext, "flevel")) {
        return SceneFileKind::Level;
    }
    if (sameExtension(ext, "prefab")) {
        return SceneFileKind::Prefab;
    }
    return SceneFileKind::Unknown;
}

} // namespace fuse::editor
