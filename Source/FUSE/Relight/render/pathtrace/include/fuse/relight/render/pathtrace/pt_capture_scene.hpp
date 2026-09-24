// FUSE Relight RL-5.1: a PtScene from an RL-1.8 capture (the USDA a Relight capture writes: capture_<app>.usda with its
// meshes / lights / materials layers), so the path-tracer parity gates run on the test apps' captured scenes (plan
// §5.8 "Scenes come from the Relight test-app captures themselves ... parity also covers capture -> scene -> render").
//
// The stage is composed by RL-3.1 (mods/usd readStage); meshes, transforms and lights are read with RL-3.2's
// importer helpers (importMesh, relativeTransform, readLightParams) and RL-4.4's lightFromUsd, at the earliest
// authored time (the capture's first frame):
//   meshes     every visible Mesh prim outside /RootNode/meshes (the prototypes are invisible), world transform of
//              the prim, COLOR0 from displayColor / displayOpacity when authored;
//   materials  the captures carry albedo textures only (no constants): every draw gets `options.material` (a grey
//              Lambertian by default); texture references are counted in the report, not loaded (the parity gates
//              compare the GPU and the CPU on the same untextured scene);
//   lights     SphereLight / RectLight / DiskLight / CylinderLight / DistantLight -> RlLight (lightFromUsd);
//   camera     the bound camera: origin, -Z forward, +Y up of its world transform; vertical field of view from
//              focalLength and horizontalAperture / aspect (the capture writes GfCamera's aspect-ratio form).
#pragma once

#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <string>
#include <vector>

namespace fuse::relight::render::pathtrace {

struct PtCaptureOptions {
    float aspect = 4.f / 3.f; ///< the captured viewport's width / height (the USDA does not carry it)
    PtMaterial material{};    ///< every captured draw's material
    float sky[3] = {0.f, 0.f, 0.f};
    bool lights = true;
};

struct PtCaptureReport {
    u32 meshes = 0;
    u32 triangles = 0;
    u32 lights = 0;
    u32 skippedLights = 0;
    u32 textures = 0; ///< texture references not loaded
    bool camera = false;
    std::vector<std::string> warnings;
};

/// Loads `usdaPath` into `out` (replaced). False with `error` when the stage cannot be read or has no geometry.
bool loadCaptureScene(const std::string& usdaPath, const PtCaptureOptions& options, PtScene& out,
                      PtCaptureReport* report = nullptr, std::string* error = nullptr);

} // namespace fuse::relight::render::pathtrace
