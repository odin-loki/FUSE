/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_game_capturer.{h,cpp}@0867d3c (GameCapturer: captureFrame,
// captureCamera, captureLights / captureSphereLight / captureDistantLight, captureInstances, newInstance,
// captureMaterial, captureMesh and its buffer readers, createDrawCallMetadata, prepExport*),
// src/lssusd/game_exporter.cpp@0867d3c (generateSkeleton, sanitizeBoneXforms, reduceIdxBufferSet /
// reduceBufferSet), src/dxvk/shaders/rtx/pass/gen_tri_list_index_buffer.h@0867d3c (generateIndices:
// strip / fan -> triangle list, degenerate triangles collapsed to vertex 0), src/lssusd/mdl_helpers.h@0867d3c
// (Filter / WrapMode vkToMdl) and src/dxvk/rtx_render/rtx_lights.cpp@0867d3c (safeColorAndIntensity).
//
// FUSE Relight RL-1.8: GameCapturer turns the Wave R1 capture packages' per-frame output into a CaptureData
// (capture_model.hpp):
//   RL-1.3 CapturedDraw     geometry: the sliced vertex streams, rebased indices, hash components, skinning;
//   RL-1.5 TranslatedDraw   the legacy material (hash = colour texture 0), camera type, and per frame the game
//                           lights (LightRecord) and the main camera (CameraState);
//   RL-1.7 SceneDrawResult  the instance a draw became (stable id) and whether its transform changed;
//   RL-1.4 TextureTracker   the albedo texture bytes (through a TextureSource callback).
// Feed it every committed draw of a frame with captureFrame (the CaptureTap frame sink, or a replay tool),
// then finish().
//
// Differences from upstream (deterministic CPU capture; no GPU read-back):
//   * time codes are captured-frame indices (0, 1, ...), not frameTime x fps;
//   * dynamic geometry: a later draw of an instance whose vertex data changed (its asset hash differs from the
//     instance's mesh while the topology - index and descriptor hashes, vertex count - is the same) adds
//     position / normal time samples to the instance's mesh when they moved more than rtx.captureMesh*Delta
//     (evalNewBufferAndCache); upstream decides from the instance update flags and also re-reads indices,
//     texcoords and colours; a changed topology is counted, not captured;
//   * instances are the ones drawn in the frame (upstream walks the whole instance table, including
//     instances kept alive but not drawn);
//   * RtInstance::isFrontFaceFlipped is not tracked by RL-1.7 yet: meshes are captured as not LHS;
//   * the sky probe bake and the window icon are not produced (no renderer yet);
//   * captureDistantLight tests its own table (upstream tests sphereLights, re-initialising every frame).
#pragma once

#include <fuse/relight/capture/export/capture_model.hpp>
#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/scene/camera/camera_manager.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>
#include <fuse/relight/scene/translate/translate_tap.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fuse::relight::capture::exporter {

struct CaptureOptions {
    CaptureMeta meta;            ///< gameId, windowTitle, exeName, stageName, timeCodesPerSecond, metersPerUnit, isZUp
    hash::HashRule assetRule = hash::parseHashRule(hash::rules::kDefaultAssetRuleString);
    std::string assetRuleString = std::string(hash::rules::kDefaultAssetRuleString);
    hash::HashRule generationRule = hash::parseHashRule(hash::rules::kDefaultGenerationRuleString);
    std::uint32_t maxFrames = 0;   ///< rtx.captureMaxFrames (0: no limit)
    bool captureInstances = true;  ///< instance stage (camera, lights, instances)
    bool reduceMeshBuffers = true; ///< Export::Meta::bReduceMeshBuffers (always true upstream)
    float meshPositionDelta = 0.3f; ///< rtx.captureMeshPositionDelta
    float meshNormalDelta = 0.3f;   ///< rtx.captureMeshNormalDelta
};

/// The D3D sampler state of colour texture 0 (D3DSAMP_ADDRESSU / ADDRESSV / MAGFILTER values).
struct SamplerState {
    std::uint32_t addressU = 1; ///< D3DTADDRESS_WRAP
    std::uint32_t addressV = 1;
    std::uint32_t magFilter = 2; ///< D3DTEXF_LINEAR
};

/// DXVK DecodeAddressMode + lss::Mdl::WrapMode::vkToMdl.
std::uint32_t mdlWrapMode(std::uint32_t d3dAddressMode);
/// DXVK DecodeFilter + lss::Mdl::Filter::vkToMdl.
std::uint32_t mdlFilter(std::uint32_t d3dTextureFilter);

/// One committed draw of the frame.
struct CaptureDraw {
    const geometry::CapturedDraw* geometry = nullptr;
    const scene::TranslatedDraw* translation = nullptr;
    scene::CategoryFlags categories;     ///< the draw's final categories (texture + geometry lists)
    scene::instances::SceneDrawResult instance;
    SamplerState colorSampler;
    std::uint32_t cullMode = 2;          ///< D3DRS_CULLMODE (1 = D3DCULL_NONE: double sided)
    tap::ResourceId colorTexture = tap::kNoResource; ///< the texture bound as colour texture 0
};

struct CaptureFrame {
    std::vector<CaptureDraw> draws;
    std::vector<scene::LightRecord> lights;       ///< the frame's game lights (TranslatedFrame::lights)
    std::optional<scene::CameraState> mainCamera; ///< nullopt: no valid main camera this frame
};

class GameCapturer {
public:
    /// The captured bytes of the texture with Remix hash `hash`, bound as `texture` (nullopt: not available;
    /// the material is still written, pointing at a texture file that is then missing).
    using TextureSource = std::function<std::optional<CaptureTexture>(Hash64 hash, tap::ResourceId texture)>;

    explicit GameCapturer(CaptureOptions options, TextureSource textures = {});

    /// GameCapturer::capture + captureFrame. Returns false (and captures nothing) once maxFrames frames
    /// have been captured.
    bool captureFrame(const CaptureFrame& frame);
    std::size_t framesCaptured() const { return m_frames; }

    /// prepExport: the CaptureData to write (skeletons generated, material links resolved, the lights'
    /// global correction transform).
    CaptureData finish() const;

    /// Draws skipped so far and why (not captured geometry, no instance, zero mesh hash).
    struct Stats {
        std::uint64_t draws = 0;
        std::uint64_t skippedNoGeometry = 0;
        std::uint64_t skippedNoInstance = 0;
        std::uint64_t skippedZeroHash = 0;
        std::uint64_t duplicateInstanceDraws = 0;
        std::uint64_t texturesMissing = 0;      ///< material textures without captured bytes
        std::uint64_t meshUpdates = 0;          ///< later draws of an instance with new vertex data
        std::uint64_t meshSamples = 0;          ///< of which kept as time samples (moved more than the delta)
        std::uint64_t meshTopologyChanges = 0;  ///< later draws with a different topology (not captured)
    };
    const Stats& stats() const { return m_stats; }

private:
    void captureCamera(const scene::CameraState& camera);
    void captureLights(const std::vector<scene::LightRecord>& lights);
    void captureInstance(const CaptureDraw& draw, std::unordered_set<std::uint64_t>& seen);
    void captureMaterial(Hash64 matHash, const CaptureDraw& draw);
    void captureMesh(Hash64 meshHash, Hash64 matHash, const CaptureDraw& draw);
    void captureMeshUpdate(CaptureMesh& mesh, const CaptureDraw& draw);
    Mat4d instanceCorrection() const;

    CaptureOptions m_options;
    TextureSource m_textureSource;
    CaptureData m_cap;
    std::map<Hash64, std::uint32_t> m_meshInstanceCount;
    std::size_t m_frames = 0;
    double m_currentTime = 0.0;
    Stats m_stats;
};

// ---- pieces exposed for the unit tests ---------------------------------------------------------------------

/// generateIndices over a whole draw: the triangle list of `topology` (VkPrimitiveTopology 3 / 4 / 5) from
/// `indices` (rebased, may be null for non-indexed draws) with `primCount` source indices or vertices.
std::vector<std::int32_t> triangleListIndices(std::uint32_t topology, const geometry::RebasedIndices* indices,
                                              std::uint32_t vertexCount);

/// reduceIdxBufferSet: renumbers the used vertices in ascending order; `used` receives the old index of each
/// new vertex.
std::vector<std::int32_t> reduceIndices(const std::vector<std::int32_t>& indices, std::vector<std::int32_t>& used);

/// Reads element `vertex` of an attribute as up to four floats (D3DDECLTYPE decoding as the input assembler
/// does: D3DCOLOR is BGRA, UBYTE4 / SHORT* unnormalised, *N normalised, UDEC3 / DEC3N 10:10:10, FLOAT16).
Vec4f readAttribute(const geometry::VertexAttribute& attribute, std::uint32_t vertex);

/// generateSkeleton (bind pose from the weighted centroid of each bone's vertices).
CaptureSkeleton generateSkeleton(const CaptureMesh& mesh);

} // namespace fuse::relight::capture::exporter
