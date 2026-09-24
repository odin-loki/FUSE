// WP-0.7 renderer test harness: deterministic procedural mini scenes.
//
// A Scene is a camera plus flat-shaded triangle batches. Each batch has one material and one
// world normal, which matches the per-draw push-constant surface of the existing G-buffer raster
// path (GBufferRasterPass / shaders/raster/gbuffer.{vert,frag}): one batch = one draw. Builders
// merge faces that share (material, normal), so the draw count stays small even for the
// 100k-instance grid.
//
// Everything is generated from integer seeds with a fixed hash (no <random>, no time, no
// unordered containers): the same build produces bit-identical vertex data on every platform
// that follows IEEE-754 single precision without FMA contraction in this file (see rp_harness.cmake).
//
// No Vulkan types: the builders run in the stub backend (rp_harness_image_io checks them).
#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::harness {

/// Shading model value the resolve treats as unlit HUD (colour = emissive). Stored in RT2.w as
/// shadingModel / 255 by write_gbuffer, so a HUD mask can be rebuilt from the G-buffer.
inline constexpr u32 kHudShadingModel = 15u;

struct SurfaceMaterial {
    math::Vec3 albedo{0.75f, 0.75f, 0.75f};
    f32 roughness = 0.5f;
    f32 metallic = 0.f;
    f32 ao = 1.f;
    math::Vec3 emissive{0.f, 0.f, 0.f};
    u32 shadingModel = 1u;
};

struct Camera {
    math::Vec3 eye{0.f, 0.f, 3.f};
    math::Vec3 target{0.f, 0.f, 0.f};
    math::Vec3 up{0.f, 1.f, 0.f};
    f32 fovYDegrees = 45.f;
    f32 nearZ = 0.05f;
    f32 farZ = 100.f;
};

/// One draw: a triangle list with a single material and world-space normal.
struct Batch {
    u32 material = 0;
    math::Vec3 normal{0.f, 0.f, 1.f};
    /// Screen-space batches (HUD) hold NDC positions (x, y in [-1, 1], y down) and skip the camera;
    /// `screenDepth` is their NDC depth (0 = nearest, always in front of the 3D scene).
    bool screenSpace = false;
    f32 screenDepth = 0.f;
    std::vector<math::Vec3> positions; ///< 3 per triangle
};

/// Named point of interest (e.g. the dark side of the thin wall for later GI leak gates).
struct SceneMarker {
    std::string name;
    math::Vec3 position{};
};

struct Scene {
    std::string name;
    Camera camera{};
    std::vector<SurfaceMaterial> materials;
    std::vector<Batch> batches;
    std::vector<SceneMarker> markers;
    /// Instance count for grid scenes (0 otherwise); informational.
    u32 instances = 0;

    u64 triangleCount() const;
    /// FNV-1a over names, materials and every vertex bit pattern (determinism checks).
    u64 contentHash() const;
};

// ---- builders ----------------------------------------------------------------------------------

/// Two overlapping screen-space quads at NDC depth 0.25 (near) and 0.75 (far), the layout of
/// fuse_b5_rhi_gbuffer_pass. Renders with the stock gbuffer.vert (per-draw depth).
Scene buildGBufferQuads();
/// Classic Cornell box: red / green side walls, white floor, ceiling and back wall, emissive
/// ceiling light, a tall and a short rotated block.
Scene buildCornellBox();
/// Ground plane plus a `side` x `side` grid of faceted UV spheres (materials vary by roughness,
/// metallic and albedo across the grid).
Scene buildSphereField(u32 side = 4u, u32 stacks = 6u, u32 slices = 10u);
/// Two rooms separated by a wall of `wallThickness` (world units); a bright emissive panel lights
/// one room only. Markers "lit_side" and "dark_side" sit 0.25 from the wall on either side.
Scene buildThinWall(f32 wallThickness = 0.02f);
/// `count` cubes (5 visible faces each) on a ground plane, laid out on a ceil(sqrt(count)) grid
/// that always spans the same area; 4 materials, hashed heights. 1k to 100k instances render in
/// at most 21 draws (4 materials x 5 face normals + ground).
Scene buildInstanceGrid(u32 count);
/// Appends a HUD overlay layer to `scene`: top bar, health bar, crosshair and minimap frame in
/// screen space (shading model kHudShadingModel, emissive colour, NDC depth 0).
void addHudOverlay(Scene& scene);
/// Cornell box with the HUD overlay layer.
Scene buildHudOverlay();

/// Builds a scene by name: gbuffer_quads, cornell_box, sphere_field, thin_wall, instance_grid_<N>
/// (N = 1k, 10k, 100k or a plain integer) and hud_overlay. Returns false for an unknown name.
bool buildSceneByName(const std::string& name, Scene& out);
/// The names above (instance grid listed as 1k, 10k and 100k).
std::vector<std::string> sceneNames();

// ---- camera projection -------------------------------------------------------------------------

/// Projects every batch to Vulkan NDC (x right, y down, z in [0, 1], 0 at the near plane),
/// clipping triangles against the near plane. Output is one xyz triple per vertex, batches in
/// order, plus each batch's first vertex and vertex count (empty batches keep count 0).
struct ProjectedScene {
    std::vector<f32> vertices; ///< xyz per vertex
    std::vector<u32> firstVertex;
    std::vector<u32> vertexCount;
};
ProjectedScene projectScene(const Scene& scene, f32 aspect);

} // namespace fuse::renderer::harness
