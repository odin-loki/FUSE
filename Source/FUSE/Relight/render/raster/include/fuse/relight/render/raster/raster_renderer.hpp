// FUSE Relight RL-4.2: the raster remaster (T0-T2) as the frame orchestration's renderer
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8, §5.9; relight.frame.mode = raster).
//
// RasterRenderer implements frame::IFrameRenderer on the renderer RL-4.1 adopted from DXVK's device (RendererContext:
// the WP-0.4 bindless heap, the WP-1.1 GPU scene). At the injection point prepare() builds the frame on the CPU
// (raster_scene.hpp) and uploads it (host-visible buffers reached by device address); declare() / record() add its
// passes to FUSE's frame graph (RG v2: FrameGpu plans and records every barrier) and record them with dynamic
// rendering:
//
//   raster.shadow    T1+: directional shadow map (depth only) of the opaque casters
//   raster.gbuffer   legacy-material G-buffer (albedo + metallic, normal + roughness, emissive + class, position):
//                    opaque, alpha-tested and sky draws with their D3D depth / cull / viewport state; game textures
//                    sampled through the bindless heap (DXVK images, imported into the graph in their host layout)
//   raster.decal     Decal-category draws blended into the albedo (depth tested against the G-buffer)
//   raster.light     deferred lighting into FUSE's output image: RL-4.3 BSDF over the GPU scene's lights (directional
//                    list + clustered point / spot lights + fallback light), shadows, D3D fog; sky unlit; background =
//                    the game's clear colour
//   raster.forward   alpha-blended draws in submission order with their D3D blend state over the lit image
//
// The output is composited below the UI by the orchestrator (RL-4.1). Shaders: Relight/shaders/raster (GLSL,
// compiled on the build host, embedded). Everything a frame uses is retired with the orchestrator's serials.
#pragma once

#include <fuse/relight/render/frame/frame_renderer.hpp>

#include <memory>

namespace fuse::relight::render::raster {

std::unique_ptr<frame::IFrameRenderer> createRasterRenderer(const frame::FrameConfig& config);

} // namespace fuse::relight::render::raster
