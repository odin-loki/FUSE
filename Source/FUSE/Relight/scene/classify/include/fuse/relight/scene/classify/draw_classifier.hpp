/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c (makeDrawCallType, isRenderingUI,
// checkBoundTextureCategory, the classification part of internalPrepareDraw / processTextures),
// src/d3d9/d3d9_rtx_utils.cpp@0867d3c (isRenderTargetPrimary) and
// src/dxvk/rtx_render/rtx_types.cpp@0867d3c (setupCategoriesForTexture / ForGeometry /
// ForHeuristics, checkSkyAutoDetect, shouldBakeSky, shouldBakeTerrain).
//
// Draw classification (plan §1.5), driven by the RL-0.6 rtx.* options (classify_options.hpp) and
// reading a D3DStateModel instead of DXVK's state. DrawClassifier::classify() is Remix's
// internalPrepareDraw up to the point where geometry processing starts: it returns the geometry
// status, whether the draw triggers RTX injection, the instance categories and the resulting
// PrepareDrawFlags / tap decision. Geometry (index rebase, hashes, skinning data), material and fog
// translation belong to RL-1.3 / RL-1.5; the few classification rules that need their results take
// them as inputs (setupCategoriesForGeometry takes the asset hash).
//
// Not thread-safe: one classifier per device, driven from the thread that makes the D3D9 calls.
#pragma once

#include <fuse/relight/scene/classify/d3d_state_model.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace fuse::relight::scene {

/// Remix RtxGeometryStatus.
enum class GeometryStatus : std::uint32_t { Ignored = 0, Rasterized = 1, RayTraced = 2 };

/// Which rule decided (Relight's diagnostic: one value per Remix return path).
enum class ClassifyReason : std::uint32_t {
    RayTraced = 0,              ///< fell through every rule
    PostInjection,              ///< internalPrepareDraw: after RTX injection, not a raytraced RT
    DrawCallRange,              ///< rtx.drawCallRange
    VertexShaderWithoutCapture, ///< programmable VS and rtx.useVertexCapture off
    ZeroPrimitives,
    UnsupportedTopology,        ///< points / lines
    AlphaTestDisabled,          ///< rtx.enableAlphaTest off and alpha test on
    AlphaBlendDisabled,         ///< rtx.enableAlphaBlend off and blending on
    OcclusionQuery,             ///< an occlusion query is active
    NoColorTarget,              ///< render target 0 not bound
    TargetWithoutImage,         ///< render target 0 has no GPU image
    ColorWriteDisabled,         ///< RGB colour writes of RT 0 not all enabled
    ShadowMask,                 ///< non-textured flood fill into a small square RT, incompatible format
    DrawingToRaytracedTarget,   ///< RT 0 is in rtx.raytracedRenderTargetTextures
    NonPrimaryTarget,           ///< RT 0 not the back-buffer size
    StencilShadow,              ///< stencil ALWAYS, INCR/DECR on z-fail, no z-write
    UserInterface,              ///< isRenderingUI(): orthographic without z-write, or a UI texture
    PositionTAsUI,              ///< POSITIONT vertices with rtx.preTransformedVerticesIsUI
    PositionT,                  ///< POSITIONT vertices (not supported by Remix): rasterized
    ColorTextureWithoutHash,    ///< processTextures: texture 0 has no hash
    IgnoreTexture,              ///< colour texture in rtx.ignoreTextures
    SkyInRaytracedTarget,       ///< sky draw into a raytraced render target
};
const char* classifyReasonName(ClassifyReason reason);
const char* geometryStatusName(GeometryStatus status);

/// Remix PrepareDrawFlag (d3d9_rtx.h).
namespace prepare_draw {
inline constexpr std::uint32_t Ignore = 0;
inline constexpr std::uint32_t CommitToRayTracing = 1u << 0;
inline constexpr std::uint32_t ApplyDrawState = 1u << 1;
inline constexpr std::uint32_t OriginalDrawCall = 1u << 2;
inline constexpr std::uint32_t PreserveDrawCallAndItsState = ApplyDrawState | OriginalDrawCall;
} // namespace prepare_draw

/// Remix D3D9Rtx::DrawCallType.
struct DrawCallType {
    GeometryStatus status = GeometryStatus::Ignored;
    bool triggerRtxInjection = false;
    ClassifyReason reason = ClassifyReason::RayTraced;
};

/// Result of DrawClassifier::classify() for one draw.
struct DrawClassification {
    std::uint32_t drawCallId = 0; ///< DrawCallState::drawCallID (not advanced after injection)
    GeometryStatus status = GeometryStatus::Ignored;
    ClassifyReason reason = ClassifyReason::RayTraced;
    bool triggerRtxInjection = false;
    /// Remix PrepareDrawFlags: what DXVK does with the original draw and whether the draw state is
    /// committed to ray tracing.
    std::uint32_t prepareFlags = prepare_draw::Ignore;
    CategoryFlags categories;
    bool skyAutoDetected = false;
    bool isDrawingToRaytracedRenderTarget = false;
    bool isUsingRaytracedRenderTarget = false;
    /// Colour textures chosen by processTextures (tap sampler slots, -1 = none) and texture 0's hash.
    std::array<std::int32_t, kMaxSupportedTextures> colorTextureSlots{-1, -1};
    Hash64 colorTextureHash = kEmptyHash;
    /// D3DTSS_TEXCOORDINDEX of the first colour stage (D3D9Rtx::m_texcoordIndex, for geometry).
    std::uint32_t texcoordIndex = 0;

    bool committed() const { return (prepareFlags & prepare_draw::CommitToRayTracing) != 0; }
    /// Remix issues the original (rasterized) draw.
    bool keepsOriginalDraw() const { return (prepareFlags & prepare_draw::OriginalDrawCall) != 0; }
};

/// Map Remix PrepareDrawFlags onto the tap decision (plan §2.3): raster kept -> Raster (or
/// RayTracedPreserveRaster when also committed), committed without the raster draw -> Ignore
/// (Relight draws it), neither -> Ignore.
tap::DrawDecision toTapDecision(std::uint32_t prepareFlags);

// ---- the individual rules (free functions: each is unit-tested on its own) ---------------------------

/// D3D9Rtx::isPrimitiveSupported: triangle lists, strips and fans.
bool isPrimitiveSupported(std::uint32_t primitiveType);
/// isRenderTargetPrimary (d3d9_rtx_utils.cpp): RT size equals the back-buffer size.
bool isRenderTargetPrimary(std::uint32_t backBufferWidth, std::uint32_t backBufferHeight, const TextureRecord& rt);
/// makeDrawCallType's shadow-mask test: stage 0 selects a non-texture argument, RT 0 is square and
/// narrower than a quarter of the back buffer, and its format has no compatibility class.
bool isShadowMaskDraw(const D3DStateModel& state);
/// makeDrawCallType's stencil-shadow-volume test.
bool isStencilShadowDraw(const D3DStateModel& state);
/// isRenderingUI's orthographic test: PROJECTION[3][3] == 1.
bool isOrthographicProjection(const D3DStateModel& state);
/// D3D9Rtx::checkBoundTextureCategory: a texture bound to a slot the shaders sample is in `set`.
bool checkBoundTextureCategory(const D3DStateModel& state, const std::unordered_set<Hash64>& set);
/// A texture's descriptor hash is in rtx.raytracedRenderTargetTextures (lookupHash on
/// getDescriptorHash(); false when the option is off).
bool isRaytracedRenderTarget(const TextureRecord& texture);

/// The colour-texture choice of D3D9Rtx::processTextures<FixedFunction> (texture binding part).
struct ColorTextureSelection {
    std::array<std::int32_t, kMaxSupportedTextures> slots{-1, -1};
    std::uint32_t firstStage = 0;
    Hash64 texture0Hash = kEmptyHash;
};
ColorTextureSelection selectColorTextures(const D3DStateModel& state);

/// DrawCallState::setupCategoriesForTexture: categories from the colour-texture hash lists.
/// `usingRaytracedRenderTarget` also sets IgnoreOpacityMicromap.
CategoryFlags categoriesForTexture(Hash64 colorTextureHash, bool usingRaytracedRenderTarget);
/// DrawCallState::setupCategoriesForGeometry: Sky when the asset hash is in rtx.skyBoxGeometries.
bool isSkyGeometry(Hash64 geometryAssetHash);

/// makeCameraPosition (rtx_types.cpp): a camera "position" (VIEW row 3) for sky auto-detection, or
/// nothing for skinned draws, particles (blend without z-write) and identity views.
std::optional<std::array<float, 3>> makeCameraPosition(const std::array<float, 16>& worldToView, bool zWrite,
                                                       bool alphaBlend, bool hasSkinning);
/// checkSkyAutoDetect (rtx_types.cpp).
bool checkSkyAutoDetect(bool depthTestEnable, const std::optional<std::array<float, 3>>& newCameraPos,
                        std::uint32_t prevFrameSeenCamerasCount,
                        const std::vector<std::array<float, 3>>& seenCameraPositions);

/// Remix SkyDetectionSource.
enum class SkyDetectionSource : std::uint32_t { None, Explicit, AutoDetect };

/// Inputs of shouldBakeSky / setupCategoriesForHeuristics taken from the DrawCallState.
struct SkyHeuristicInput {
    std::uint32_t drawCallId = 0;
    float minZ = 0.0f; ///< viewport MinZ clamped to [0, 1]
    bool zEnable = false, zWriteEnable = false, alphaBlend = false, hasSkinning = false;
    bool usesTexture = false; ///< a colour texture was selected
    Hash64 materialHash = kEmptyHash; ///< legacy material hash = colour texture 0 hash
    bool isDrawingToRaytracedRenderTarget = false;
    std::array<float, 16> worldToView{};
};
/// shouldBakeSky (rtx_types.cpp): adds the draw's camera to `seenCameraPositions`.
SkyDetectionSource shouldBakeSky(const SkyHeuristicInput& in, std::uint32_t prevFrameSeenCamerasCount,
                                 std::vector<std::array<float, 3>>& seenCameraPositions);
/// shouldBakeTerrain (rtx_types.cpp).
bool shouldBakeTerrain(Hash64 materialHash);

// ---- the per-device classifier ---------------------------------------------------------------------------

class DrawClassifier {
public:
    /// D3D9Rtx::makeDrawCallType: advances the draw-call id. `isUsingRaytracedRenderTarget` receives
    /// whether a bound texture is a raytraced render target.
    DrawCallType makeDrawCallType(const D3DStateModel& state, bool* isUsingRaytracedRenderTarget = nullptr,
                                  bool* isDrawingToRaytracedRenderTarget = nullptr);
    /// D3D9Rtx::isRenderingUI.
    bool isRenderingUI(const D3DStateModel& state) const;
    /// The classification part of D3D9Rtx::internalPrepareDraw (see the header comment).
    DrawClassification classify(const D3DStateModel& state);
    /// DrawCallState::setupCategoriesForGeometry, once the geometry asset hash is known (RL-1.3).
    static void applyGeometryCategories(DrawClassification& result, Hash64 geometryAssetHash);

    /// D3D9Rtx::EndFrame: reset the draw-call id and injection, keep last frame's camera count.
    void endFrame();

    bool rtxInjectTriggered() const { return m_rtxInjectTriggered; }
    std::uint32_t drawCallId() const { return m_drawCallId; }
    const std::vector<std::array<float, 3>>& seenCameraPositions() const { return m_seenCameraPositions; }
    std::uint32_t prevFrameSeenCamerasCount() const { return m_prevFrameSeenCamerasCount; }

private:
    std::uint32_t m_drawCallId = 0;
    bool m_rtxInjectTriggered = false;
    std::vector<std::array<float, 3>> m_seenCameraPositions;
    std::uint32_t m_prevFrameSeenCamerasCount = 0;
};

} // namespace fuse::relight::scene
