#pragma once

#include <fuse/ecs/entity.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Shared editor chrome state — Qt-free; mirrors P6 `EditorState` selection/edit flags.
struct EditorState {
    std::vector<ecs::EntityID> selectedEntities;
    ecs::EntityID primarySelection = ecs::EntityID::null();

    bool sceneModified = false;
    bool playing = false;
    bool paused = false;

    std::string layoutName = "default";
};

} // namespace fuse::editor
