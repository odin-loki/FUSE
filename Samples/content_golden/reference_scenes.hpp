// FUSE asset plan W0.8 (docs/plans/FUSE_ASSET_PLAN.md §5.4): deterministic reference scenes for content
// golden renders.
//
// Scene (1) of §5.4, the material-ball grid, in its Wave-0 form: a 5 x 5 grid of faceted balls on a
// ground plane (roughness across columns, metallic down rows, hashed albedo in 0.2..0.9 linear),
// rendered through the existing G-buffer raster path (WP-0.7 HeadlessFrameRunner, Lavapipe) at
// 960 x 540 and resolved on the CPU under three fixed lighting setups. The setups only move the resolve's
// directional light and background (sun: high key under a blue sky, overcast: zenith key on a grey sky, interior:
// grazing key on a dark room); the W0.7 material system and the FrameComposer replace the resolve when
// they land, at which point the goldens are regenerated in the same change (update policy, §5.4).
//
// Everything is generated from constants and integer hashes (no time, no <random>): the scene is
// bit-identical on every build, and Lavapipe renders it bit-identically.
#pragma once

#include "scene.hpp"

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::content_golden {

inline constexpr u32 kGoldenWidth = 960u;
inline constexpr u32 kGoldenHeight = 540u;
/// Mean-FLIP pass threshold against a committed golden (§5.4).
inline constexpr double kGoldenMaxMeanFlip = 0.05;

struct LightingSetup {
    std::string name;
    math::Vec3 lightDir;
    math::Vec3 background;
};

/// sun, overcast, interior (fixed order: golden names are <scene>_<setup>.png).
std::vector<LightingSetup> lightingSetups();

/// The Wave-0 material-ball grid ("material_balls").
renderer::harness::Scene buildMaterialBallGrid();

/// Every reference scene of this sample (currently the material-ball grid).
std::vector<std::string> referenceSceneNames();

} // namespace fuse::content_golden
