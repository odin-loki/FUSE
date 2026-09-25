// FUSE Relight RL-5.1: the path tracer as the frame orchestration's renderer (relight.frame.mode = pathtrace;
// docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8, §5.1).
//
// PathTraceFrameRenderer implements frame::IFrameRenderer (RL-4.1) on the renderer RL-4.1 adopted from DXVK's device
// (RendererContext: the device, allocator and WP-0.4 bindless heap). At the injection point prepare():
//
//   scene      builds RL-4.2's CPU frame (raster::buildRasterFrame over the frame's draws so far: the same draw
//              selection, legacy material mapping, positions and main camera as the raster remaster) and converts it
//              (ptSceneFromRaster): one PtMesh + PtInstance per surviving opaque / decal / blended draw with object-
//              space positions (FixedFunction / VertexCapture; clip-space POSITIONT / CaptureClip draws are skipped),
//              linear albedo / emission from the legacy material, COLOR0 (linearised) as vertex colour, the frame's
//              game lights (RL-1.5 records -> RL-4.4 sphere / spot / distant lights; the RL-3.4 replaced list as
//              GPU-scene lights when the scene was fed at the injection) and RL-4.2's fallback distant light;
//              untextured (game textures are not sampled by this first cut);
//   trace      PathTracerGpu ("relight.pt.trace" on its own GpuScene + WP-6.0 acceleration structures) in a render
//              graph of its own, submitted under the host's queue lock and waited for; accumulation: a frame whose
//              scene (vertices, draws, camera, lights) hashes like the previous one continues the sample sequence
//              (sampleBase += samplesPerFrame, accumulate), any change starts over;
//   output     the accumulated mean (x exposure, clamped, sRGB-encoded) packed into the output image's 8-bit layout
//              (B8G8R8A8 / R8G8B8A8, UNORM or SRGB) in a host-visible ring slot; declare() adds one pass,
//              "relight.pt.present", which copies it into FUSE's output image (TransferDst) - the orchestrator then
//              composites it below the UI as for the raster remaster.
//
// relight.pathtrace.referenceSpp > 0 (tests): the CPU reference path tracer renders the same compiled scene once per
// scene (another seed, that many samples) and every frame record carries the GPU accumulation's distance to it (RMSE
// and 8x8 block means within 4 sigma), so the Wine gate checks convergence on the captured scene.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/render/frame/frame_renderer.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>
#include <fuse/relight/render/raster/raster_options.hpp>
#include <fuse/relight/render/raster/raster_scene.hpp>

#include <memory>
#include <vector>

namespace fuse::relight::render::pathtrace {

struct PtFrameOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.pathtrace", int, samplesPerFrame, 4, "FUSE_RELIGHT_PT_SPP",
                            "Path-traced samples per pixel and frame (relight.frame.mode = pathtrace).");
    FUSE_RELIGHT_OPTION("relight.pathtrace", int, maxBounces, 4,
                        "Scattering vertices per path of the frame path tracer (1: direct lighting only).");
    FUSE_RELIGHT_OPTION("relight.pathtrace", float, exposure, 1.f,
                        "Scale of the path-traced radiance before the sRGB encoding of the frame output.");
    FUSE_RELIGHT_OPTION("relight.pathtrace", bool, accumulate, true,
                        "Accumulate samples over frames while the scene does not change.");
    FUSE_RELIGHT_OPTION_ENV("relight.pathtrace", int, referenceSpp, 0, "FUSE_RELIGHT_PT_REFERENCE_SPP",
                            "Tests: samples per pixel of an in-process CPU reference of each scene; the frame record "
                            "then carries the GPU accumulation's distance to it. 0: off.");
};

struct PtFrameConfig {
    u32 samplesPerFrame = 4;
    u32 maxBounces = 4;
    float exposure = 1.f;
    bool accumulate = true;
    u32 referenceSpp = 0;
    raster::RasterConfig raster{}; ///< ambient / fallback light / tier options RL-4.2 reads

    static PtFrameConfig fromOptions();
};

struct PtFromRasterStats {
    u32 draws = 0;     ///< raster draws considered
    u32 meshes = 0;    ///< converted draws
    u32 triangles = 0;
    u32 skipped = 0;   ///< skipped (bucket Skipped, clip-space positions, empty)
    u32 lights = 0;
    bool fallbackLight = false;
};

/// RL-4.2's CPU frame -> a PtScene (see the header comment). `gameLights`: the RL-1.5 records (preferred), else
/// `sceneLights` (GPU-scene lights). False when no draw survived or the frame has no perspective camera.
bool ptSceneFromRaster(const raster::RasterFrame& frame, const std::vector<scene::LightRecord>* gameLights,
                       const std::vector<frame::AdapterLight>* sceneLights, const PtFrameConfig& config, PtScene& out,
                       PtFromRasterStats* stats = nullptr);

/// relight.frame.mode = pathtrace.
std::unique_ptr<frame::IFrameRenderer> createPathTraceRenderer(const frame::FrameConfig& config);

} // namespace fuse::relight::render::pathtrace
