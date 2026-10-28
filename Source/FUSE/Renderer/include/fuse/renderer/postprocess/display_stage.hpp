#pragma once

#include <cstdint>

namespace fuse::renderer {

/// Display-chain order, separate from PostPass (bloom / dof / tonemap / grade).
enum class DisplayStage : std::uint8_t {
    Depth = 0,
    Motion = 1,
    Exposure = 2,
    Color = 3,
    Tonemap = 4,
    Ui = 5,
};

inline bool display_stage_before(DisplayStage a, DisplayStage b) {
    return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
}

inline int display_stage_index(DisplayStage stage) {
    return static_cast<int>(stage);
}

} // namespace fuse::renderer
