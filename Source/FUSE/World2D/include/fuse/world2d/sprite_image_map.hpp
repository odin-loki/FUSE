#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::world2d {

/// Search roots for T2D `imageMap` filenames (project `assets/`, `worlds/`, bundled parity stubs).
struct SpriteImageMapSearchRoots {
    std::string projectRoot;
    std::string repositoryRoot;
};

/// Sample RGB at the centre of `frameIndex` in a horizontal sprite sheet (`frameCount` columns).
/// Tries PNG files on disk when stb is available; otherwise a deterministic procedural strip.
[[nodiscard]] bool sampleSpriteImageMapFrame(const SpriteImageMapSearchRoots& roots, const std::string& imageMap,
                                             u32 frameIndex, u32 frameCount, u8& r, u8& g, u8& b);

} // namespace fuse::world2d
