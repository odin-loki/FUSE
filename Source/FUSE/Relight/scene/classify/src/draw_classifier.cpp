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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/d3d9/d3d9_rtx.cpp@0867d3c, src/d3d9/d3d9_rtx_utils.cpp@0867d3c and
// src/dxvk/rtx_render/rtx_types.cpp@0867d3c. See draw_classifier.hpp.
#include <fuse/relight/scene/classify/draw_classifier.hpp>

#include <fuse/relight/scene/classify/classify_options.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::scene {

namespace {

bool lookupHash(const options::Option<options::HashSet>& list, Hash64 hash) { return list.containsHash(hash); }

bool isIdentityExact(const std::array<float, 16>& m) {
    const std::array<float, 16> id = identityMatrix();
    return std::memcmp(m.data(), id.data(), sizeof(float) * 16) == 0;
}

/// Remix areCamerasClose.
bool areCamerasClose(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float d = ClassifyOptions::skyAutoDetectUniqueCameraDistance();
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz < d * d;
}

/// D3D9Rtx::processTextures's ArgsMask (Remix's own table, which differs from DXVK's for BUMPENVMAP).
std::uint32_t remixArgsMask(std::uint32_t op) {
    switch (op) {
    case d3d::TOP_DISABLE:
        return 0b000u;
    case d3d::TOP_SELECTARG1:
    case d3d::TOP_PREMODULATE:
        return 0b010u;
    case d3d::TOP_SELECTARG2:
        return 0b100u;
    case d3d::TOP_MULTIPLYADD:
    case d3d::TOP_LERP:
        return 0b111u;
    default:
        return 0b110u;
    }
}

} // namespace

const char* classifyReasonName(ClassifyReason r) {
    switch (r) {
    case ClassifyReason::RayTraced: return "RayTraced";
    case ClassifyReason::PostInjection: return "PostInjection";
    case ClassifyReason::DrawCallRange: return "DrawCallRange";
    case ClassifyReason::VertexShaderWithoutCapture: return "VertexShaderWithoutCapture";
    case ClassifyReason::ZeroPrimitives: return "ZeroPrimitives";
    case ClassifyReason::UnsupportedTopology: return "UnsupportedTopology";
    case ClassifyReason::AlphaTestDisabled: return "AlphaTestDisabled";
    case ClassifyReason::AlphaBlendDisabled: return "AlphaBlendDisabled";
    case ClassifyReason::OcclusionQuery: return "OcclusionQuery";
    case ClassifyReason::NoColorTarget: return "NoColorTarget";
    case ClassifyReason::TargetWithoutImage: return "TargetWithoutImage";
    case ClassifyReason::ColorWriteDisabled: return "ColorWriteDisabled";
    case ClassifyReason::ShadowMask: return "ShadowMask";
    case ClassifyReason::DrawingToRaytracedTarget: return "DrawingToRaytracedTarget";
    case ClassifyReason::NonPrimaryTarget: return "NonPrimaryTarget";
    case ClassifyReason::StencilShadow: return "StencilShadow";
    case ClassifyReason::UserInterface: return "UserInterface";
    case ClassifyReason::PositionTAsUI: return "PositionTAsUI";
    case ClassifyReason::PositionT: return "PositionT";
    case ClassifyReason::ColorTextureWithoutHash: return "ColorTextureWithoutHash";
    case ClassifyReason::IgnoreTexture: return "IgnoreTexture";
    case ClassifyReason::SkyInRaytracedTarget: return "SkyInRaytracedTarget";
    }
    return "?";
}

const char* geometryStatusName(GeometryStatus s) {
    switch (s) {
    case GeometryStatus::Ignored: return "ignored";
    case GeometryStatus::Rasterized: return "rasterized";
    case GeometryStatus::RayTraced: return "raytraced";
    }
    return "?";
}

tap::DrawDecision toTapDecision(std::uint32_t flags) {
    const bool original = (flags & prepare_draw::OriginalDrawCall) != 0;
    const bool commit = (flags & prepare_draw::CommitToRayTracing) != 0;
    if (original) {
        return commit ? tap::DrawDecision::RayTracedPreserveRaster : tap::DrawDecision::Raster;
    }
    return tap::DrawDecision::Ignore;
}

// ---- rules ----------------------------------------------------------------------------------------------

bool isPrimitiveSupported(std::uint32_t pt) {
    return pt == d3d::PT_TRIANGLELIST || pt == d3d::PT_TRIANGLEFAN || pt == d3d::PT_TRIANGLESTRIP;
}

bool isRenderTargetPrimary(std::uint32_t bbWidth, std::uint32_t bbHeight, const TextureRecord& rt) {
    return bbWidth == rt.width && bbHeight == rt.height;
}

bool isShadowMaskDraw(const D3DStateModel& s) {
    // Conditions: non-textured flood-fill draws into a small quad render target.
    const std::uint32_t op = s.tss(0, d3d::TSS_COLOROP);
    const bool floodFill = (op == d3d::TOP_SELECTARG1 && s.tss(0, d3d::TSS_COLORARG1) != d3d::TA_TEXTURE) ||
                           (op == d3d::TOP_SELECTARG2 && s.tss(0, d3d::TSS_COLORARG2) != d3d::TA_TEXTURE);
    if (!floodFill || !s.renderTarget0.valid()) {
        return false;
    }
    // If rt is a quad at least 4 times smaller than backbuffer and the format is invalid format,
    // then it is likely a shadow mask.
    const TextureRecord& rt = s.renderTarget0;
    return rt.width == rt.height && rt.width < s.backBufferWidth / 4 &&
           formatCompatibilityCategory(renderTargetVkFormat(rt.format)) ==
               FormatCompatibilityCategory::InvalidFormatCompatibilityCategory;
}

bool isStencilShadowDraw(const D3DStateModel& s) {
    // Conditions: passing-through stencil is enabled with increment or decrement z-fail action.
    const std::uint32_t zfail = s.rs(d3d::RS_STENCILZFAIL);
    return s.rs(d3d::RS_STENCILENABLE) == 1 && s.rs(d3d::RS_STENCILFUNC) == d3d::CMP_ALWAYS &&
           (zfail == d3d::STENCILOP_DECR || zfail == d3d::STENCILOP_INCR || zfail == d3d::STENCILOP_DECRSAT ||
            zfail == d3d::STENCILOP_INCRSAT) &&
           s.rs(d3d::RS_ZWRITEENABLE) == 0;
}

bool isOrthographicProjection(const D3DStateModel& s) { return s.projection[15] == 1.0f; }

bool checkBoundTextureCategory(const D3DStateModel& s, const std::unordered_set<Hash64>& set) {
    const std::uint32_t usedSamplerMask = s.psSamplerMask | s.vsSamplerMask;
    const std::uint32_t usedTextureMask = s.activeTextureMask() & usedSamplerMask;
    for (std::uint32_t slot = 0; slot < s.textures.size(); ++slot) {
        if (!(usedTextureMask & (1u << slot))) {
            continue;
        }
        if (set.count(s.textures[slot].imageHash) != 0) {
            return true;
        }
    }
    return false;
}

bool isRaytracedRenderTarget(const TextureRecord& t) {
    return ClassifyOptions::RaytracedRenderTarget::enable() && t.valid() && t.hasImage &&
           lookupHash(ClassifyOptions::raytracedRenderTargetTextures, t.descriptorHash);
}

ColorTextureSelection selectColorTextures(const D3DStateModel& s) {
    ColorTextureSelection sel;
    const bool fixedFunction = !s.usesPixelShader;
    constexpr std::uint32_t kInvalidStage = 0xff;
    constexpr std::uint32_t kBins = kMaxTexcoords * kMaxSupportedTextures;
    std::uint8_t texcoordIndexToStage[kBins];
    std::memset(texcoordIndexToStage, kInvalidStage, sizeof texcoordIndexToStage);
    if (fixedFunction) {
        for (std::uint32_t stage = 0; stage < tap::kTextureStageCount; ++stage) {
            if (!s.textures[stage].valid()) {
                continue;
            }
            // Subsequent stages do not occur if this is true.
            if (s.tss(stage, d3d::TSS_COLOROP) == d3d::TOP_DISABLE) {
                break;
            }
            const std::uint32_t argsMask =
                remixArgsMask(s.tss(stage, d3d::TSS_COLOROP)) | remixArgsMask(s.tss(stage, d3d::TSS_ALPHAOP));
            auto isTex = [&](std::uint32_t colorArg, std::uint32_t alphaArg) {
                return (s.tss(stage, colorArg) & d3d::TA_SELECTMASK) == d3d::TA_TEXTURE ||
                       (s.tss(stage, alphaArg) & d3d::TA_SELECTMASK) == d3d::TA_TEXTURE;
            };
            const std::uint32_t texMask = (isTex(d3d::TSS_COLORARG0, d3d::TSS_ALPHAARG0) ? 0b001u : 0u) |
                                          (isTex(d3d::TSS_COLORARG1, d3d::TSS_ALPHAARG1) ? 0b010u : 0u) |
                                          (isTex(d3d::TSS_COLORARG2, d3d::TSS_ALPHAARG2) ? 0b100u : 0u);
            // Is texture used?
            if ((argsMask & texMask) == 0) {
                continue;
            }
            const TextureRecord& tex = s.textures[stage];
            // Remix can only handle 2D textures - no volumes.
            if (tex.type != d3d::RTYPE_TEXTURE &&
                (!ClassifyOptions::allowCubemaps() || tex.type != d3d::RTYPE_CUBETEXTURE)) {
                continue;
            }
            // Currently we only support regular textures, skip lightmaps.
            if (lookupHash(ClassifyOptions::lightmapTextures, tex.imageHash)) {
                continue;
            }
            // Allow for two stage candidates per texcoord index.
            const std::uint32_t texcoordIndex = s.tss(stage, d3d::TSS_TEXCOORDINDEX) & 0b111u;
            const std::uint32_t candidateIndex = texcoordIndex * kMaxSupportedTextures;
            const std::uint32_t subIndex = texcoordIndexToStage[candidateIndex] == kInvalidStage ? 0 : 1;
            // Don't override if candidate exists.
            if (texcoordIndexToStage[candidateIndex + subIndex] == kInvalidStage) {
                texcoordIndexToStage[candidateIndex + subIndex] = static_cast<std::uint8_t>(stage);
            }
        }
    }
    const std::uint32_t bins = fixedFunction ? kBins : kMaxSupportedTextures;
    for (std::uint32_t idx = 0, textureId = 0; idx < bins && textureId < kMaxSupportedTextures; ++idx) {
        const std::uint32_t stage = fixedFunction ? texcoordIndexToStage[idx] : textureId;
        if (stage == kInvalidStage || !s.textures[stage].valid()) {
            continue;
        }
        if (textureId == 0) {
            sel.texture0Hash = s.textures[stage].imageHash;
            if (fixedFunction) {
                sel.firstStage = stage;
            }
        }
        sel.slots[textureId] = static_cast<std::int32_t>(stage);
        ++textureId;
    }
    return sel;
}

CategoryFlags categoriesForTexture(Hash64 h, bool usingRaytracedRenderTarget) {
    using C = InstanceCategories;
    using O = ClassifyOptions;
    CategoryFlags f;
    auto set = [&f](C c, bool doSet) {
        if (doSet) {
            f.set(c);
        }
    };
    set(C::WorldUI, lookupHash(O::worldSpaceUiTextures, h));
    set(C::WorldMatte, lookupHash(O::worldSpaceUiBackgroundTextures, h));
    set(C::Ignore, lookupHash(O::ignoreTextures, h));
    set(C::IgnoreLights, lookupHash(O::ignoreLights, h));
    set(C::IgnoreAntiCulling, lookupHash(O::antiCullingTextures, h));
    set(C::IgnoreMotionBlur, lookupHash(O::motionBlurMaskOutTextures, h));
    set(C::IgnoreOpacityMicromap, lookupHash(O::opacityMicromapIgnoreTextures, h) || usingRaytracedRenderTarget);
    set(C::IgnoreAlphaChannel, lookupHash(O::ignoreAlphaOnTextures, h));
    set(C::IgnoreBakedLighting, lookupHash(O::ignoreBakedLightingTextures, h));
    set(C::Hidden, lookupHash(O::hideInstanceTextures, h));
    set(C::Particle, lookupHash(O::particleTextures, h));
    set(C::Beam, lookupHash(O::beamTextures, h));
    set(C::DecalStatic, lookupHash(O::decalTextures, h));
    set(C::DecalDynamic, lookupHash(O::dynamicDecalTextures, h));
    set(C::DecalSingleOffset, lookupHash(O::singleOffsetDecalTextures, h));
    set(C::DecalNoOffset, lookupHash(O::nonOffsetDecalTextures, h));
    set(C::AnimatedWater, lookupHash(O::animatedWaterTextures, h));
    set(C::ThirdPersonPlayerModel, lookupHash(O::playerModelTextures, h));
    set(C::ThirdPersonPlayerBody, lookupHash(O::playerModelBodyTextures, h));
    set(C::Terrain, lookupHash(O::terrainTextures, h));
    set(C::Sky, lookupHash(O::skyBoxTextures, h));
    set(C::ParticleEmitter, lookupHash(O::particleEmitterTextures, h));
    set(C::HairCards, lookupHash(O::hairCardTextures, h));
    return f;
}

bool isSkyGeometry(Hash64 geometryAssetHash) { return lookupHash(ClassifyOptions::skyBoxGeometries, geometryAssetHash); }

std::optional<std::array<float, 3>> makeCameraPosition(const std::array<float, 16>& worldToView, bool zWrite,
                                                       bool alphaBlend, bool hasSkinning) {
    if (hasSkinning) {
        return std::nullopt;
    }
    // particles
    if (!zWrite && alphaBlend) {
        return std::nullopt;
    }
    if (isIdentityExact(worldToView)) {
        return std::nullopt;
    }
    // As we compare the cameras relatively and don't need a precise camera position, a
    // position-like vector (the view translation row) avoids a matrix inverse.
    return std::array<float, 3>{worldToView[12], worldToView[13], worldToView[14]};
}

bool checkSkyAutoDetect(bool depthTestEnable, const std::optional<std::array<float, 3>>& newCameraPos,
                        std::uint32_t prevFrameSeenCamerasCount,
                        const std::vector<std::array<float, 3>>& seenCameraPositions) {
    const SkyAutoDetectMode mode = ClassifyOptions::skyAutoDetect();
    if (mode != SkyAutoDetectMode::CameraPositionAndDepthFlags && mode != SkyAutoDetectMode::CameraPosition) {
        return false;
    }
    const bool withDepthFlags = mode == SkyAutoDetectMode::CameraPositionAndDepthFlags;

    const bool searchingForSkyCamera = seenCameraPositions.empty();
    const bool skyFoundAndSearchingForMainCamera = seenCameraPositions.size() == 1;
    const bool skyAndMainCameraFound = seenCameraPositions.size() >= 2;

    if (skyAndMainCameraFound) {
        // Assume that subsequent draw calls can not be sky.
        return false;
    }
    if (searchingForSkyCamera) {
        if (withDepthFlags) {
            // No depth test: frame starts with a sky; depth test: frame starts with a world.
            return !depthTestEnable;
        }
        // Assume the first camera to be sky.
        return true;
    }
    // Corner case: if there was no sky camera at all, fall back (this also costs one rasterized
    // frame, like a flicker).
    if (prevFrameSeenCamerasCount < 2) {
        if (withDepthFlags) {
            return !depthTestEnable;
        }
        return false;
    }
    if (skyFoundAndSearchingForMainCamera) {
        // A draw without a camera position can't contain the main camera: still sky.
        if (!newCameraPos) {
            return true;
        }
        // Same as the existing sky camera: still sky; otherwise a new unique camera (the main one).
        return areCamerasClose(seenCameraPositions[0], *newCameraPos);
    }
    return false;
}

SkyDetectionSource shouldBakeSky(const SkyHeuristicInput& in, std::uint32_t prevFrameSeenCamerasCount,
                                 std::vector<std::array<float, 3>>& seenCameraPositions) {
    const std::optional<std::array<float, 3>> cameraPos =
        in.isDrawingToRaytracedRenderTarget
            ? std::nullopt
            : makeCameraPosition(in.worldToView, in.zWriteEnable, in.alphaBlend, in.hasSkinning);
    if (cameraPos) {
        bool unique = true;
        for (const auto& seen : seenCameraPositions) {
            if (areCamerasClose(seen, *cameraPos)) {
                unique = false;
                break;
            }
        }
        if (unique) {
            seenCameraPositions.push_back(*cameraPos);
        }
    }

    if (in.minZ >= ClassifyOptions::skyMinZThreshold()) {
        return SkyDetectionSource::Explicit;
    }
    if (in.usesTexture) {
        if (lookupHash(ClassifyOptions::skyBoxTextures, in.materialHash)) {
            return SkyDetectionSource::Explicit;
        }
    } else if (in.drawCallId < ClassifyOptions::skyDrawcallIdThreshold()) {
        return SkyDetectionSource::Explicit;
    }
    // Camera positions are not tracked for raytraced render targets (a different camera).
    static const std::vector<std::array<float, 3>> kRenderTargetCameraPositions;
    if (checkSkyAutoDetect(in.zEnable, cameraPos, prevFrameSeenCamerasCount,
                           in.isDrawingToRaytracedRenderTarget ? kRenderTargetCameraPositions : seenCameraPositions)) {
        return SkyDetectionSource::AutoDetect;
    }
    return SkyDetectionSource::None;
}

bool shouldBakeTerrain(Hash64 materialHash) {
    if (!ClassifyOptions::needsTerrainBaking()) {
        return false;
    }
    return lookupHash(ClassifyOptions::terrainTextures, materialHash);
}

// ---- DrawClassifier -------------------------------------------------------------------------------------

DrawCallType DrawClassifier::makeDrawCallType(const D3DStateModel& s, bool* usingRtRt, bool* drawingToRtRt) {
    // Track the draw-call index (DrawCallState::drawCallID is the value before the increment; the
    // range test reads the incremented counter, as upstream).
    ++m_drawCallId;
    if (usingRtRt) {
        *usingRtRt = false;
    }
    if (drawingToRtRt) {
        *drawingToRtRt = false;
    }
    const options::Vec2i range = ClassifyOptions::drawCallRange();
    if (m_drawCallId < static_cast<std::uint32_t>(range.x) || m_drawCallId > static_cast<std::uint32_t>(range.y)) {
        return {GeometryStatus::Ignored, false, ClassifyReason::DrawCallRange};
    }

    // Raytraced render target support: a bound (render-target) texture that is in the list.
    if (ClassifyOptions::RaytracedRenderTarget::enable() && usingRtRt) {
        for (const TextureRecord& t : s.textures) {
            if ((t.usage & d3d::USAGE_RENDERTARGET) && isRaytracedRenderTarget(t)) {
                *usingRtRt = true;
            }
        }
    }

    if (s.usesVertexShader && !ClassifyOptions::useVertexCapture()) {
        return {GeometryStatus::Ignored, false, ClassifyReason::VertexShaderWithoutCapture};
    }
    if (s.primitiveCount == 0) {
        return {GeometryStatus::Ignored, false, ClassifyReason::ZeroPrimitives};
    }
    // Only certain draw calls are worth ray tracing.
    if (!isPrimitiveSupported(s.primitiveType)) {
        return {GeometryStatus::Ignored, false, ClassifyReason::UnsupportedTopology};
    }
    if (!ClassifyOptions::enableAlphaTest() && s.isAlphaTestEnabled()) {
        return {GeometryStatus::Ignored, false, ClassifyReason::AlphaTestDisabled};
    }
    if (!ClassifyOptions::enableAlphaBlend() && s.rs(d3d::RS_ALPHABLENDENABLE)) {
        return {GeometryStatus::Ignored, false, ClassifyReason::AlphaBlendDisabled};
    }
    if (s.activeOcclusionQueries > 0) {
        return {GeometryStatus::Rasterized, false, ClassifyReason::OcclusionQuery};
    }
    if (!s.renderTarget0.valid()) {
        return {GeometryStatus::Ignored, false, ClassifyReason::NoColorTarget};
    }
    if (!s.renderTarget0.hasImage) {
        return {GeometryStatus::Ignored, false, ClassifyReason::TargetWithoutImage};
    }
    if ((s.rs(d3d::RS_COLORWRITEENABLE) & d3d::COLORWRITE_RGB) != d3d::COLORWRITE_RGB) {
        return {GeometryStatus::Ignored, false, ClassifyReason::ColorWriteDisabled};
    }
    if (isShadowMaskDraw(s)) {
        return {GeometryStatus::Ignored, false, ClassifyReason::ShadowMask};
    }
    // Raytraced render target: this render target is in the list, so the draw is ray traced with
    // its own camera and the result used as a texture later.
    if (isRaytracedRenderTarget(s.renderTarget0)) {
        if (drawingToRtRt) {
            *drawingToRtRt = true;
        }
        return {GeometryStatus::RayTraced, false, ClassifyReason::DrawingToRaytracedTarget};
    }
    if (!s.resolutionOverride && !isRenderTargetPrimary(s.backBufferWidth, s.backBufferHeight, s.renderTarget0)) {
        return {GeometryStatus::Rasterized, false, ClassifyReason::NonPrimaryTarget};
    }
    if (isStencilShadowDraw(s)) {
        return {GeometryStatus::Ignored, false, ClassifyReason::StencilShadow};
    }
    // Check UI only on the primary render target.
    if (isRenderingUI(s)) {
        return {GeometryStatus::Rasterized, true, ClassifyReason::UserInterface};
    }
    if (s.hasPositionT) {
        if (ClassifyOptions::preTransformedVerticesIsUI()) {
            return {GeometryStatus::Rasterized, true, ClassifyReason::PositionTAsUI};
        }
        return {GeometryStatus::Rasterized, false, ClassifyReason::PositionT};
    }
    return {GeometryStatus::RayTraced, false, ClassifyReason::RayTraced};
}

bool DrawClassifier::isRenderingUI(const D3DStateModel& s) const {
    if (!s.usesVertexShader && ClassifyOptions::orthographicIsUI()) {
        // Draw calls with an orthographic projection are assumed to be UI (a common pattern, and
        // these objects can't be ray traced).
        if (isOrthographicProjection(s) && s.rs(d3d::RS_ZWRITEENABLE) == 0) {
            return true;
        }
    }
    // Check if a UI texture is bound.
    return checkBoundTextureCategory(s, ClassifyOptions::uiTextures());
}

DrawClassification DrawClassifier::classify(const D3DStateModel& s) {
    DrawClassification r;
    const std::uint32_t ignoredFlags =
        ClassifyOptions::enableRaytracing() ? prepare_draw::Ignore : prepare_draw::PreserveDrawCallAndItsState;

    // After RTX injection everything else is rasterized, unless it targets a raytraced render target
    // (render-to-texture in games that draw UI before 3D content).
    if (m_rtxInjectTriggered && !isRaytracedRenderTarget(s.renderTarget0)) {
        r.drawCallId = m_drawCallId;
        r.status = GeometryStatus::Rasterized;
        r.reason = ClassifyReason::PostInjection;
        r.prepareFlags = ClassifyOptions::skipDrawCallsPostRTXInjection() ? prepare_draw::Ignore
                                                                           : prepare_draw::PreserveDrawCallAndItsState;
        return r;
    }

    r.drawCallId = m_drawCallId;
    const DrawCallType type = makeDrawCallType(s, &r.isUsingRaytracedRenderTarget, &r.isDrawingToRaytracedRenderTarget);
    r.status = type.status;
    r.reason = type.reason;

    if (type.status == GeometryStatus::Ignored) {
        r.prepareFlags = ignoredFlags;
        return r;
    }
    if (type.triggerRtxInjection) {
        r.triggerRtxInjection = true;
        m_rtxInjectTriggered = true;
        r.prepareFlags = prepare_draw::PreserveDrawCallAndItsState;
        return r;
    }
    if (type.status == GeometryStatus::Rasterized) {
        r.prepareFlags = prepare_draw::PreserveDrawCallAndItsState;
        return r;
    }

    // (Geometry: index rebase, empty index ranges and zero vertex counts are RL-1.3's checks.)

    // processRenderState -> processTextures: pick the colour textures, then the texture categories.
    const ColorTextureSelection sel = selectColorTextures(s);
    r.colorTextureSlots = sel.slots;
    r.colorTextureHash = sel.texture0Hash;
    if (sel.slots[0] >= 0 && sel.texture0Hash == kEmptyHash) {
        // Texture 0 without a valid hash: skip the draw.
        r.status = GeometryStatus::Ignored;
        r.reason = ClassifyReason::ColorTextureWithoutHash;
        r.prepareFlags = ignoredFlags;
        return r;
    }
    if (s.textures[sel.firstStage].valid()) {
        r.categories = categoriesForTexture(sel.texture0Hash, r.isUsingRaytracedRenderTarget);
        if (lookupHash(ClassifyOptions::smoothNormalsTextures, sel.texture0Hash)) {
            r.categories.set(InstanceCategories::SmoothNormals);
        }
        // An ignore texture is bound.
        if (r.categories.test(InstanceCategories::Ignore)) {
            r.status = GeometryStatus::Ignored;
            r.reason = ClassifyReason::IgnoreTexture;
            r.prepareFlags = ignoredFlags;
            return r;
        }
        if (r.categories.test(InstanceCategories::Terrain) && ClassifyOptions::terrainAsDecalsEnabledIfNoBaker() &&
            !ClassifyOptions::terrainBakerEnableBaking()) {
            r.categories.clr(InstanceCategories::Terrain);
            r.categories.set(InstanceCategories::DecalStatic);
        }
    }
    r.texcoordIndex = s.tss(sel.firstStage, d3d::TSS_TEXCOORDINDEX);

    // Heuristics (setupCategoriesForHeuristics): sky and terrain.
    SkyHeuristicInput in;
    in.drawCallId = r.drawCallId;
    in.minZ = std::clamp(s.viewport.minZ, 0.0f, 1.0f);
    in.zEnable = s.rs(d3d::RS_ZENABLE) == d3d::ZB_TRUE;
    in.zWriteEnable = s.rs(d3d::RS_ZWRITEENABLE) != 0;
    in.alphaBlend = s.rs(d3d::RS_ALPHABLENDENABLE) != 0;
    in.hasSkinning = s.hasSkinning();
    in.usesTexture = sel.slots[0] >= 0;
    in.materialHash = sel.texture0Hash;
    in.isDrawingToRaytracedRenderTarget = r.isDrawingToRaytracedRenderTarget;
    in.worldToView = s.view;
    const SkyDetectionSource sky = shouldBakeSky(in, m_prevFrameSeenCamerasCount, m_seenCameraPositions);
    if (sky != SkyDetectionSource::None) {
        r.categories.set(InstanceCategories::Sky);
    }
    r.skyAutoDetected = sky == SkyDetectionSource::AutoDetect;
    if (shouldBakeTerrain(sel.texture0Hash)) {
        r.categories.set(InstanceCategories::Terrain);
    }

    // Sky draws into a raytraced render target are dropped: those scenes use the main sky.
    if (r.isDrawingToRaytracedRenderTarget && r.categories.test(InstanceCategories::Sky)) {
        r.status = GeometryStatus::Ignored;
        r.reason = ClassifyReason::SkyInRaytracedTarget;
        r.prepareFlags = ignoredFlags;
        return r;
    }

    const bool preserveOriginalDraw = s.usesVertexShader && ClassifyOptions::useVertexCapture();
    const bool needsDrawState =
        r.categories.test(InstanceCategories::Sky) || r.categories.test(InstanceCategories::Terrain);
    r.prepareFlags = prepare_draw::CommitToRayTracing | (needsDrawState ? prepare_draw::ApplyDrawState : 0u) |
                     (preserveOriginalDraw ? prepare_draw::PreserveDrawCallAndItsState : 0u);
    return r;
}

void DrawClassifier::applyGeometryCategories(DrawClassification& result, Hash64 geometryAssetHash) {
    if (isSkyGeometry(geometryAssetHash)) {
        result.categories.set(InstanceCategories::Sky);
    }
}

void DrawClassifier::endFrame() {
    m_rtxInjectTriggered = false;
    m_drawCallId = 0;
    m_prevFrameSeenCamerasCount = static_cast<std::uint32_t>(m_seenCameraPositions.size());
    m_seenCameraPositions.clear();
}

} // namespace fuse::relight::scene
