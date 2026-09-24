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
// Ported from dxvk-remix src/d3d9/d3d9_rtx_utils.cpp@0867d3c and src/d3d9/d3d9_rtx.cpp@0867d3c
// (processRenderState, processTextures<FixedFunction>). See ff_translate.hpp.
#include <fuse/relight/scene/translate/ff_translate.hpp>
#include <fuse/relight/scene/translate/translate_options.hpp>

#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/classify/classify_options.hpp>

#include <cstring>
#include <variant>

namespace fuse::relight::scene {

namespace {

float bitsToFloat(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

Mat4f identity4() {
    Mat4f m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

/// D3D9BlendState + FixupBlendState (DXVK d3d9_util.h).
struct D3D9BlendState {
    std::uint32_t src, dst, op;
};
void fixupBlendState(D3D9BlendState& s) {
    // Old DirectX 6 HW feature that still exists...
    if (s.src == d3dff::BLEND_BOTHSRCALPHA) {
        s.src = d3dff::BLEND_SRCALPHA;
        s.dst = d3dff::BLEND_INVSRCALPHA;
    } else if (s.src == d3dff::BLEND_BOTHINVSRCALPHA) {
        s.src = d3dff::BLEND_INVSRCALPHA;
        s.dst = d3dff::BLEND_SRCALPHA;
    }
}

// d3d9_rtx_utils.cpp convertTextureOp. TODO upstream: support more D3DTEXTUREOP members when necessary.
TextureOperation convertTextureOp(std::uint32_t op) {
    switch (op) {
    default:
    case d3d::TOP_MODULATE: return TextureOperation::Modulate;
    case d3d::TOP_DISABLE: return TextureOperation::Disable;
    case d3d::TOP_SELECTARG1: return TextureOperation::SelectArg1;
    case d3d::TOP_SELECTARG2: return TextureOperation::SelectArg2;
    case d3dff::TOP_MODULATE2X: return TextureOperation::Modulate2x;
    case d3dff::TOP_MODULATE4X: return TextureOperation::Modulate4x;
    case d3dff::TOP_ADD: return TextureOperation::Add;
    }
}

// d3d9_rtx_utils.cpp convertColorSource.
TextureArgSource convertColorSource(std::uint32_t source) {
    switch (source) {
    default:
    case d3dff::MCS_COLOR2: // TODO upstream: support the 2nd vertex color array
    case d3dff::MCS_MATERIAL: return TextureArgSource::None;
    case d3dff::MCS_COLOR1: return TextureArgSource::VertexColor0;
    }
}

// d3d9_rtx_utils.cpp convertTextureArg (the full value: an argument with modifier flags is None).
TextureArgSource convertTextureArg(std::uint32_t arg, TextureArgSource color0, TextureArgSource color1) {
    switch (arg) {
    default: return TextureArgSource::None;
    case d3d::TA_CURRENT:
    case d3d::TA_DIFFUSE: return color0;
    case d3dff::TA_SPECULAR: return color1;
    case d3d::TA_TEXTURE: return TextureArgSource::Texture;
    case d3d::TA_TFACTOR: return TextureArgSource::TFactor;
    }
}

// d3d9_rtx.cpp processTextures ArgsMask: the arguments an operation uses (bit 0: arg0, 1: arg1, 2: arg2).
std::uint32_t argsMask(std::uint32_t op) {
    switch (op) {
    case d3d::TOP_DISABLE: return 0b000u;
    case d3d::TOP_SELECTARG1:
    case d3d::TOP_PREMODULATE: return 0b010u;
    case d3d::TOP_SELECTARG2: return 0b100u;
    case d3d::TOP_MULTIPLYADD:
    case d3d::TOP_LERP: return 0b111u;
    default: return 0b110u;
    }
}

bool lookupHash(const options::Option<options::HashSet>& list, hash::Hash64 h) { return list.containsHash(h); }

} // namespace

// ---- decoding ---------------------------------------------------------------------------------------------

std::uint32_t decodeCompareOp(std::uint32_t f) {
    switch (f) {
    default:
    case d3dff::CMP_NEVER: return vk::COMPARE_OP_NEVER;
    case d3dff::CMP_LESS: return vk::COMPARE_OP_LESS;
    case d3dff::CMP_EQUAL: return vk::COMPARE_OP_EQUAL;
    case d3dff::CMP_LESSEQUAL: return vk::COMPARE_OP_LESS_OR_EQUAL;
    case d3dff::CMP_GREATER: return vk::COMPARE_OP_GREATER;
    case d3dff::CMP_NOTEQUAL: return vk::COMPARE_OP_NOT_EQUAL;
    case d3dff::CMP_GREATEREQUAL: return vk::COMPARE_OP_GREATER_OR_EQUAL;
    case d3dff::CMP_ALWAYS: return vk::COMPARE_OP_ALWAYS;
    }
}

std::uint32_t decodeBlendFactor(std::uint32_t b, bool isAlpha) {
    switch (b) {
    default:
    case d3dff::BLEND_ZERO: return vk::BLEND_FACTOR_ZERO;
    case d3dff::BLEND_ONE: return vk::BLEND_FACTOR_ONE;
    case d3dff::BLEND_SRCCOLOR: return vk::BLEND_FACTOR_SRC_COLOR;
    case d3dff::BLEND_INVSRCCOLOR: return vk::BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case d3dff::BLEND_SRCALPHA: return vk::BLEND_FACTOR_SRC_ALPHA;
    case d3dff::BLEND_INVSRCALPHA: return vk::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case d3dff::BLEND_DESTALPHA: return vk::BLEND_FACTOR_DST_ALPHA;
    case d3dff::BLEND_INVDESTALPHA: return vk::BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case d3dff::BLEND_DESTCOLOR: return vk::BLEND_FACTOR_DST_COLOR;
    case d3dff::BLEND_INVDESTCOLOR: return vk::BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case d3dff::BLEND_SRCALPHASAT: return vk::BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case d3dff::BLEND_BOTHSRCALPHA: return vk::BLEND_FACTOR_SRC_ALPHA;
    case d3dff::BLEND_BOTHINVSRCALPHA: return vk::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case d3dff::BLEND_BLENDFACTOR: return isAlpha ? vk::BLEND_FACTOR_CONSTANT_ALPHA : vk::BLEND_FACTOR_CONSTANT_COLOR;
    case d3dff::BLEND_INVBLENDFACTOR:
        return isAlpha ? vk::BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : vk::BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case d3dff::BLEND_SRCCOLOR2: return vk::BLEND_FACTOR_SRC1_COLOR;
    case d3dff::BLEND_INVSRCCOLOR2: return vk::BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    }
}

std::uint32_t decodeBlendOp(std::uint32_t op) {
    switch (op) {
    default:
    case d3dff::BLENDOP_ADD: return vk::BLEND_OP_ADD;
    case d3dff::BLENDOP_SUBTRACT: return vk::BLEND_OP_SUBTRACT;
    case d3dff::BLENDOP_REVSUBTRACT: return vk::BLEND_OP_REVERSE_SUBTRACT;
    case d3dff::BLENDOP_MIN: return vk::BLEND_OP_MIN;
    case d3dff::BLENDOP_MAX: return vk::BLEND_OP_MAX;
    }
}

std::array<float, 4> decodeD3DColor(std::uint32_t color) {
    // Encoded in D3DCOLOR as argb.
    std::array<float, 4> rgba;
    rgba[3] = static_cast<float>((color & 0xff000000u) >> 24) / 255.0f;
    rgba[0] = static_cast<float>((color & 0x00ff0000u) >> 16) / 255.0f;
    rgba[1] = static_cast<float>((color & 0x0000ff00u) >> 8) / 255.0f;
    rgba[2] = static_cast<float>((color & 0x000000ffu)) / 255.0f;
    return rgba;
}

bool renderTargetHasAlphaSwizzle(std::uint32_t f) {
    switch (f) {
    case 22:  // X8R8G8B8
    case 24:  // X1R5G5B5
    case 33:  // X8B8G8R8
    case 34:  // G16R16
    case 50:  // L8
    case 60:  // V8U8
    case 62:  // X8L8V8U8
    case 64:  // V16U16
    case 65:  // W11V11U10
    case 81:  // L16
    case 111: // R16F
    case 112: // G16R16F
    case 114: // R32F
    case 115: // G32R32F
        return true;
    default:
        return f == d3d::fourcc('A', 'T', 'I', '1') || f == d3d::fourcc('A', 'T', 'I', '2') ||
               f == d3d::fourcc('D', 'F', '2', '4') || f == d3d::fourcc('D', 'F', '1', '6');
    }
}

const char* textureArgSourceName(TextureArgSource s) {
    switch (s) {
    case TextureArgSource::None: return "None";
    case TextureArgSource::Texture: return "Texture";
    case TextureArgSource::VertexColor0: return "VertexColor0";
    case TextureArgSource::TFactor: return "TFactor";
    }
    return "?";
}

const char* textureOperationName(TextureOperation o) {
    switch (o) {
    case TextureOperation::Disable: return "Disable";
    case TextureOperation::SelectArg1: return "SelectArg1";
    case TextureOperation::SelectArg2: return "SelectArg2";
    case TextureOperation::Modulate: return "Modulate";
    case TextureOperation::Modulate2x: return "Modulate2x";
    case TextureOperation::Modulate4x: return "Modulate4x";
    case TextureOperation::Add: return "Add";
    case TextureOperation::Force_Modulate2x: return "Force_Modulate2x";
    }
    return "?";
}

const char* texGenModeName(TexGenMode m) {
    switch (m) {
    case TexGenMode::None: return "None";
    case TexGenMode::ViewPositions: return "ViewPositions";
    case TexGenMode::CascadedViewPositions: return "CascadedViewPositions";
    case TexGenMode::ViewNormals: return "ViewNormals";
    }
    return "?";
}

bool TranslateOptions::useWorldMatricesForShaders() {
    if (const options::OptionBase* o = options::OptionManager::findOption("rtx.useWorldMatricesForShaders")) {
        const options::OptionValue v = o->getResolvedValue();
        if (const bool* b = std::get_if<bool>(&v)) {
            return *b;
        }
    }
    return true; // d3d9_rtx.h default
}

LegacyMaterialDefaultValues legacyMaterialDefaults() {
    using D = LegacyMaterialDefaults;
    LegacyMaterialDefaultValues v;
    v.anisotropy = D::anisotropy();
    v.emissiveIntensity = D::emissiveIntensity();
    v.useAlbedoTextureIfPresent = D::useAlbedoTextureIfPresent();
    v.albedoConstant = D::albedoConstant();
    v.opacityConstant = D::opacityConstant();
    v.roughnessConstant = D::roughnessConstant();
    v.metallicConstant = D::metallicConstant();
    v.emissiveColorConstant = D::emissiveColorConstant();
    v.enableEmissive = D::enableEmissive();
    v.ignoreAlphaChannel = D::ignoreAlphaChannel();
    v.enableThinFilm = D::enableThinFilm();
    v.alphaIsThinFilmThickness = D::alphaIsThinFilmThickness();
    v.thinFilmThicknessConstant = D::thinFilmThicknessConstant();
    return v;
}

// ---- inputs -------------------------------------------------------------------------------------------------

FixedFunctionState buildFixedFunctionState(const tap::DrawState& s) {
    FixedFunctionState ff;
    ff.material = s.material;
    for (std::uint32_t i = 0; i < s.elementCount && i < tap::kMaxVertexElements; ++i) {
        const tap::VertexElement& e = s.elements[i];
        if (e.usage == d3dff::DECLUSAGE_COLOR && e.usageIndex == 0) {
            ff.hasColor0 = true;
        } else if (e.usage == d3dff::DECLUSAGE_COLOR && e.usageIndex == 1) {
            ff.hasColor1 = true;
        }
    }
    for (std::uint32_t t = 0; t < 8; ++t) {
        if (s.transforms) {
            std::memcpy(ff.textureTransforms[t].data(), s.transforms[tap::kTransformTexture0 + t], sizeof(float) * 16);
        } else {
            ff.textureTransforms[t] = identity4();
        }
    }
    for (std::uint32_t p = 0; s.clipPlanes && p < d3dff::kMaxClipPlanes; ++p) {
        std::memcpy(ff.clipPlanes[p].data(), s.clipPlanes[p], sizeof(float) * 4);
    }
    return ff;
}

// ---- material -----------------------------------------------------------------------------------------------

LegacyMaterialRecord setLegacyMaterialState(const D3DStateModel& s, const FixedFunctionState& ff, bool alphaSwizzle) {
    LegacyMaterialRecord m;

    const bool hasPositionT = s.hasPositionT;
    const bool hasColor0 = ff.hasColor0;
    const bool hasColor1 = ff.hasColor1;
    const bool lighting = s.rs(d3dff::RS_LIGHTING) != 0 && !hasPositionT; // FFP lighting on only if not positionT

    std::uint32_t diffuseSource = hasColor0 ? d3dff::MCS_COLOR1 : d3dff::MCS_MATERIAL;
    std::uint32_t specularSource = hasColor1 ? d3dff::MCS_COLOR2 : d3dff::MCS_MATERIAL;
    if (lighting) {
        const bool colorVertex = s.rs(d3dff::RS_COLORVERTEX) != 0;
        const std::uint32_t mask = (lighting && colorVertex) ? (diffuseSource | specularSource) : 0;
        diffuseSource = s.rs(d3dff::RS_DIFFUSEMATERIALSOURCE) & mask;
        specularSource = s.rs(d3dff::RS_SPECULARMATERIALSOURCE) & mask;
    }

    m.alphaTestEnabled = s.isAlphaTestEnabled();
    m.alphaTestCompareOp = m.alphaTestEnabled ? decodeCompareOp(s.rs(d3dff::RS_ALPHAFUNC)) : vk::COMPARE_OP_ALWAYS;
    // Note: only the bottom 8 bits are used, as per the standard.
    m.alphaTestReferenceValue = static_cast<std::uint8_t>(s.rs(d3dff::RS_ALPHAREF) & 0xff);

    m.diffuseColorSource = convertColorSource(diffuseSource);
    m.specularColorSource = convertColorSource(specularSource);

    m.tFactor = s.rs(d3dff::RS_TEXTUREFACTOR);

    BlendMode& b = m.blendMode;
    b.enableBlending = s.rs(d3d::RS_ALPHABLENDENABLE) != 0;

    D3D9BlendState color{s.rs(d3dff::RS_SRCBLEND), s.rs(d3dff::RS_DESTBLEND), s.rs(d3dff::RS_BLENDOP)};
    fixupBlendState(color);

    D3D9BlendState alpha = color;
    if (s.rs(d3dff::RS_SEPARATEALPHABLENDENABLE)) {
        alpha = {s.rs(d3dff::RS_SRCBLENDALPHA), s.rs(d3dff::RS_DESTBLENDALPHA), s.rs(d3dff::RS_BLENDOPALPHA)};
        fixupBlendState(alpha);
    }

    b.colorSrcFactor = decodeBlendFactor(color.src, false);
    b.colorDstFactor = decodeBlendFactor(color.dst, false);
    b.colorBlendOp = decodeBlendOp(color.op);
    b.alphaSrcFactor = decodeBlendFactor(alpha.src, true);
    b.alphaDstFactor = decodeBlendFactor(alpha.dst, true);
    b.alphaBlendOp = decodeBlendOp(alpha.op);
    b.writeMask = s.rs(d3d::RS_COLORWRITEENABLE); // ColorWriteIndex(0)

    // An alpha-swizzled render target (XRGB formats) reads destination alpha as one.
    auto normalizeFactor = [alphaSwizzle](std::uint32_t f) {
        if (alphaSwizzle) {
            if (f == vk::BLEND_FACTOR_DST_ALPHA) {
                return vk::BLEND_FACTOR_ONE;
            }
            if (f == vk::BLEND_FACTOR_ONE_MINUS_DST_ALPHA) {
                return vk::BLEND_FACTOR_ZERO;
            }
        }
        return f;
    };
    b.colorSrcFactor = normalizeFactor(b.colorSrcFactor);
    b.colorDstFactor = normalizeFactor(b.colorDstFactor);
    b.alphaSrcFactor = normalizeFactor(b.alphaSrcFactor);
    b.alphaDstFactor = normalizeFactor(b.alphaDstFactor);

    m.d3dMaterial = ff.material;

    // Allow the users to configure vertex color as baked lighting for legacy draw calls.
    m.isVertexColorBakedLighting = TranslateOptions::vertexColorIsBakedLighting();
    return m;
}

TextureFactorBlending textureFactorBlending(const D3DStateModel& s) {
    TextureFactorBlending out;
    if (s.usesPixelShader) {
        return out;
    }
    bool& useStage = out.useStageTextureFactorBlending;
    bool& useMultiple = out.useMultipleStageTextureFactorBlending;

    for (std::uint32_t stage = 0; stage < tap::kTextureStageCount; ++stage) {
        auto isTextureFactorBlendingEnabled = [&](std::uint32_t st) {
            const std::uint32_t colorOp = s.tss(st, d3d::TSS_COLOROP);
            const std::uint32_t alphaOp = s.tss(st, d3d::TSS_ALPHAOP);
            if (colorOp == d3d::TOP_DISABLE && alphaOp == d3d::TOP_DISABLE) {
                return false;
            }
            const std::uint32_t a1c = s.tss(st, d3d::TSS_COLORARG1) & d3d::TA_SELECTMASK;
            const std::uint32_t a2c = s.tss(st, d3d::TSS_COLORARG2) & d3d::TA_SELECTMASK;
            const std::uint32_t a1a = s.tss(st, d3d::TSS_ALPHAARG1) & d3d::TA_SELECTMASK;
            const std::uint32_t a2a = s.tss(st, d3d::TSS_ALPHAARG2) & d3d::TA_SELECTMASK;
            // If the previous stage wrote to TEMP, the prior result this stage reads is D3DTA_TEMP.
            std::uint32_t prevResultSel = d3d::TA_CURRENT;
            if (st != 0) {
                const std::uint32_t resultArg = s.tss(st - 1, d3d::TSS_RESULTARG) & d3d::TA_SELECTMASK;
                prevResultSel = resultArg == d3d::TA_TEMP ? d3d::TA_TEMP : d3d::TA_CURRENT;
            }
            auto isModulate = [](std::uint32_t op) {
                return op == d3d::TOP_MODULATE || op == d3dff::TOP_MODULATE2X || op == d3dff::TOP_MODULATE4X;
            };
            const bool colorMul = isModulate(colorOp) && ((a1c == d3d::TA_TFACTOR && a2c == prevResultSel) ||
                                                          (a2c == d3d::TA_TFACTOR && a1c == prevResultSel));
            const bool alphaMul = isModulate(alphaOp) && ((a1a == d3d::TA_TFACTOR && a2a == prevResultSel) ||
                                                          (a2a == d3d::TA_TFACTOR && a1a == prevResultSel));
            return colorMul || alphaMul;
        };

        // Texture factor blending besides the first stage (1 additional stage supported). If the tFactor is
        // disabled for the current texture (useStage), multiple-stage tFactor blendings are ignored.
        bool isCurrentStageTextureFactorBlendingEnabled = false;
        if (useStage && TranslateOptions::enableMultiStageTextureFactorBlending() && stage != 0 &&
            isTextureFactorBlendingEnabled(stage)) {
            isCurrentStageTextureFactorBlendingEnabled = true;
            useMultiple = true;
        }

        if (!s.textures[stage].valid()) {
            continue;
        }
        // Subsequent stages do not occur if this is true.
        if (s.tss(stage, d3d::TSS_COLOROP) == d3d::TOP_DISABLE) {
            break;
        }
        const std::uint32_t used = argsMask(s.tss(stage, d3d::TSS_COLOROP)) | argsMask(s.tss(stage, d3d::TSS_ALPHAOP));
        auto isTex = [&](std::uint32_t colorArg, std::uint32_t alphaArg) {
            return (s.tss(stage, colorArg) & d3d::TA_SELECTMASK) == d3d::TA_TEXTURE ||
                   (s.tss(stage, alphaArg) & d3d::TA_SELECTMASK) == d3d::TA_TEXTURE;
        };
        const std::uint32_t texMask = (isTex(d3d::TSS_COLORARG0, d3d::TSS_ALPHAARG0) ? 0b001u : 0u) |
                                      (isTex(d3d::TSS_COLORARG1, d3d::TSS_ALPHAARG1) ? 0b010u : 0u) |
                                      (isTex(d3d::TSS_COLORARG2, d3d::TSS_ALPHAARG2) ? 0b100u : 0u);
        // Is texture used?
        if ((used & texMask) == 0) {
            continue;
        }
        const TextureRecord& tex = s.textures[stage];
        // Remix can only handle 2D textures - no volumes.
        if (tex.type != d3d::RTYPE_TEXTURE && (!ClassifyOptions::allowCubemaps() || tex.type != d3d::RTYPE_CUBETEXTURE)) {
            continue;
        }
        // Currently we only support regular textures, skip lightmaps.
        if (lookupHash(ClassifyOptions::lightmapTextures, tex.imageHash)) {
            continue;
        }
        // Check if texture factor blending is enabled for the first stage.
        if (useStage && stage == 0) {
            isCurrentStageTextureFactorBlendingEnabled = isTextureFactorBlendingEnabled(stage);
        }
        // A texture whose baked lighting is ignored disables texture factor blending.
        if (isCurrentStageTextureFactorBlendingEnabled && lookupHash(ClassifyOptions::ignoreBakedLightingTextures, tex.imageHash)) {
            useStage = false;
            useMultiple = false;
        }
    }
    return out;
}

void setTextureStageState(const D3DStateModel& s, const FixedFunctionState& ff, std::uint32_t stageIdx,
                          bool useStageTextureFactorBlending, bool useMultipleStageTextureFactorBlending,
                          LegacyMaterialRecord& m, DrawTransforms& t) {
    const TextureArgSource c0 = m.diffuseColorSource;
    const TextureArgSource c1 = m.specularColorSource;
    auto dropTFactor = [useStageTextureFactorBlending](TextureArgSource& a) {
        if (!useStageTextureFactorBlending && a == TextureArgSource::TFactor) {
            a = TextureArgSource::None;
        }
    };
    m.textureColorOperation = convertTextureOp(s.tss(stageIdx, d3d::TSS_COLOROP));
    m.textureColorArg1Source = convertTextureArg(s.tss(stageIdx, d3d::TSS_COLORARG1), c0, c1);
    m.textureColorArg2Source = convertTextureArg(s.tss(stageIdx, d3d::TSS_COLORARG2), c0, c1);
    dropTFactor(m.textureColorArg1Source);
    dropTFactor(m.textureColorArg2Source);

    m.textureAlphaOperation = convertTextureOp(s.tss(stageIdx, d3d::TSS_ALPHAOP));
    m.textureAlphaArg1Source = convertTextureArg(s.tss(stageIdx, d3d::TSS_ALPHAARG1), c0, c1);
    m.textureAlphaArg2Source = convertTextureArg(s.tss(stageIdx, d3d::TSS_ALPHAARG2), c0, c1);
    dropTFactor(m.textureAlphaArg1Source);
    dropTFactor(m.textureAlphaArg2Source);

    m.isTextureFactorBlend = useMultipleStageTextureFactorBlending;

    const std::uint32_t texcoordIndex = s.tss(stageIdx, d3d::TSS_TEXCOORDINDEX);
    const std::uint32_t transformFlags = s.tss(stageIdx, d3dff::TSS_TEXTURETRANSFORMFLAGS);
    const std::uint32_t textureTransformCount = transformFlags & 0x3;

    // Counts beyond 2 are clamped to 2 elements upstream (logged); projected transforms are not supported.
    if (textureTransformCount != d3dff::TTFF_DISABLE && stageIdx < ff.textureTransforms.size()) {
        t.textureTransform = ff.textureTransforms[stageIdx];
    } else {
        t.textureTransform = identity4();
    }

    switch (texcoordIndex) {
    default:
    case d3dff::TCI_PASSTHRU:
        t.texgenMode = TexGenMode::None;
        break;
    case d3dff::TCI_CAMERASPACEREFLECTIONVECTOR:
    case d3dff::TCI_SPHEREMAP:
        t.texgenMode = TexGenMode::None; // not supported upstream (logged)
        break;
    case d3dff::TCI_CAMERASPACEPOSITION:
        t.texgenMode = TexGenMode::ViewPositions;
        break;
    case d3dff::TCI_CAMERASPACENORMAL:
        t.texgenMode = TexGenMode::ViewNormals;
        break;
    }
}

void applyTerrainAsDecalModulate(hash::Hash64 colorTextureHash, LegacyMaterialRecord& m) {
    // processTextures: a Terrain draw turned into a static decal (classifier) because there is no baker.
    if (!lookupHash(ClassifyOptions::terrainTextures, colorTextureHash) ||
        !ClassifyOptions::terrainAsDecalsEnabledIfNoBaker() || ClassifyOptions::terrainBakerEnableBaking()) {
        return;
    }
    // Modulate to compensate the multilayer blending.
    if (TranslateOptions::terrainAsDecalsAllowOverModulate()) {
        TextureOperation& op = m.textureColorOperation;
        if (op == TextureOperation::Modulate2x || op == TextureOperation::Modulate4x) {
            op = TextureOperation::Force_Modulate2x;
        }
    }
}

Mat4f multiply(const Mat4f& a, const Mat4f& b) {
    Mat4f r{};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            r[i * 4 + j] =
                a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
    return r;
}

DrawTransforms processTransforms(const D3DStateModel& s, const FixedFunctionState& ff, bool useVertexCapture) {
    DrawTransforms t;
    // When games use vertex shaders, the object to world transforms can be unreliable, and so we can ignore them.
    const bool useObjectToWorldTransform =
        !s.usesVertexShader || (s.usesVertexShader && useVertexCapture && TranslateOptions::useWorldMatricesForShaders());
    t.objectToWorld = useObjectToWorldTransform ? s.world : identity4();
    t.worldToView = s.view;
    t.viewToProjection = s.projection;
    t.objectToView = multiply(t.objectToWorld, t.worldToView);
    t.textureTransform = identity4();

    // Some games pass invalid matrices which D3D9 does not care about; sanitize to avoid NaNs in inversions.
    if (t.objectToWorld[15] == 0.f) {
        t.objectToWorld[15] = 1.f;
    }
    if (t.objectToView[15] == 0.f) {
        t.objectToView[15] = 1.f;
    }
    if (t.worldToView[15] == 0.f) {
        t.worldToView[15] = 1.f;
    }

    // Find one truly enabled clip plane because we don't support more than one.
    t.enableClipPlane = false;
    const std::uint32_t enabled = s.rs(d3dff::RS_CLIPPLANEENABLE);
    if (enabled != 0) {
        for (std::uint32_t i = 0; i < d3dff::kMaxClipPlanes; ++i) {
            if ((enabled & (1u << i)) == 0) {
                continue;
            }
            // Make sure that the plane equation is not degenerate.
            const std::array<float, 4>& plane = ff.clipPlanes[i];
            if (plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2] > 0.f) {
                if (t.enableClipPlane) {
                    break; // more than 1 user clip plane is not supported (logged upstream)
                }
                t.enableClipPlane = true;
                t.clipPlane = plane;
            }
        }
    }
    return t;
}

// ---- fog ------------------------------------------------------------------------------------------------------

hash::Hash64 FogRecord::hash() const {
    std::uint8_t bytes[28];
    std::memcpy(bytes + 0, &mode, 4);
    std::memcpy(bytes + 4, color.data(), 12);
    std::memcpy(bytes + 16, &scale, 4);
    std::memcpy(bytes + 20, &end, 4);
    std::memcpy(bytes + 24, &density, 4);
    return hash::xxh3_64(bytes, sizeof bytes);
}

FogRecord setFogState(const D3DStateModel& s) {
    FogRecord fog;
    if (s.rs(d3dff::RS_FOGENABLE)) {
        const std::array<float, 4> color = decodeD3DColor(s.rs(d3dff::RS_FOGCOLOR));
        const float end = bitsToFloat(s.rs(d3dff::RS_FOGEND));
        const float start = bitsToFloat(s.rs(d3dff::RS_FOGSTART));
        fog.mode = s.rs(d3dff::RS_FOGTABLEMODE) != d3dff::FOG_NONE ? s.rs(d3dff::RS_FOGTABLEMODE)
                                                                    : s.rs(d3dff::RS_FOGVERTEXMODE);
        fog.color = {color[0], color[1], color[2]};
        fog.scale = 1.0f / (end - start);
        fog.end = end;
        fog.density = bitsToFloat(s.rs(d3dff::RS_FOGDENSITY));
    } else {
        fog.mode = d3dff::FOG_NONE;
    }
    return fog;
}

} // namespace fuse::relight::scene
