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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_options.h@0867d3c (vertexColorIsBakedLighting,
// fogIgnoreSky, enableMultiStageTextureFactorBlending, terrainAsDecalsAllowOverModulate) and
// src/dxvk/rtx_render/rtx_materials.h@0867d3c (LegacyMaterialDefaults: the 13 rtx.legacyMaterial.*
// options). Names, types, defaults, environment variables and descriptions as upstream.
//
// RL-1.5 owns these rtx.* options (RL-0.6 registry; each also answers to its relight.* twin). Other
// packages read them from here, never declare them again.
//
// Borrowed (declared by another package, read by name with the Remix default as fallback):
//   rtx.useWorldMatricesForShaders   (d3d9_rtx.h, default true; owner: vertex capture, RL-1.6)
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

namespace fuse::relight::scene {

struct TranslateOptions {
    using Vec3f = options::Vec3f;

    FUSE_RELIGHT_OPTION("rtx", bool, vertexColorIsBakedLighting, true, "If true, brightness contribution will be removed from the vertex color by dividing each component by the largest component.");
    FUSE_RELIGHT_OPTION("rtx", bool, fogIgnoreSky, false, "If true, sky draw calls will be skipped when searching for the D3D9 fog values.");
    FUSE_RELIGHT_OPTION_ENV("rtx", bool, enableMultiStageTextureFactorBlending, true, "RTX_ENABLE_MULTI_STAGE_TEXTURE_FACTOR_BLENDING", "Support texture factor blending in stage 1~7. Currently only support 1 additional blending stage, more than 1 additional blending stages will be ignored.");
    FUSE_RELIGHT_OPTION("rtx.terrain", bool, terrainAsDecalsAllowOverModulate, false, "Set to true, if it's known that terrain layers with ModulateX2 / ModulateX4 flags do not contain a lighting info, but ModulateX2 / ModulateX4 are used only to blend layers.");

    /// rtx.useWorldMatricesForShaders (borrowed; true when no package declares it).
    static bool useWorldMatricesForShaders();
};

/// Remix LegacyMaterialDefaults: rtx.legacyMaterial.* (the material of non-replaced draws).
struct LegacyMaterialDefaults {
    using Vec3f = options::Vec3f;

    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", float, anisotropy, 0.f,
                        "The default roughness anisotropy to use for non-replaced \"legacy\" materials. "
                        "Should be in the range -1 to 1, where 0 is isotropic.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", float, emissiveIntensity, 0.f,
                        "The default emissive intensity to use for non-replaced \"legacy\" materials.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", bool, useAlbedoTextureIfPresent, true,
                        "A flag to determine if an \"albedo\" texture (a qualifying color texture) from the original application "
                        "should be used if present on non-replaced \"legacy\" materials.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", Vec3f, albedoConstant, Vec3f(1.0f, 1.0f, 1.0f),
                        "The default albedo constant to use for non-replaced \"legacy\" materials. "
                        "Should be a color in sRGB colorspace with gamma encoding.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", float, opacityConstant, 1.f,
                        "The default opacity constant to use for non-replaced \"legacy\" materials. "
                        "Should be in the range 0 to 1.");
    FUSE_RELIGHT_OPTION_ENV("rtx.legacyMaterial", float, roughnessConstant, 0.7f, "DXVK_LEGACY_MATERIAL_DEFAULT_ROUGHNESS",
                            "The default perceptual roughness constant to use for non-replaced \"legacy\" materials. "
                            "Should be in the range 0 to 1.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", float, metallicConstant, 0.1f,
                        "The default metallic constant to use for non-replaced \"legacy\" materials. "
                        "Should be in the range 0 to 1.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", Vec3f, emissiveColorConstant, Vec3f(0.0f, 0.0f, 0.0f),
                        "The default emissive color constant to use for non-replaced \"legacy\" materials. "
                        "Should be a color in sRGB colorspace with gamma encoding.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", bool, enableEmissive, false,
                        "A flag to determine if emission should be used on non-replaced \"legacy\" materials.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", bool, ignoreAlphaChannel, false,
                        "A flag to determine if the albedo alpha channel should be ignored on non-replaced \"legacy\" materials.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", bool, enableThinFilm, false,
                        "A flag to determine if a thin-film layer should be used on non-replaced \"legacy\" materials.");
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", bool, alphaIsThinFilmThickness, false,
                        "A flag to determine if the alpha channel from the albedo source should be treated as thin film thickness "
                        "on non-replaced \"legacy\" materials.");
    // Note: Should be something non-zero as 0 is an invalid thickness to have (even if this is just unused).
    FUSE_RELIGHT_OPTION("rtx.legacyMaterial", float, thinFilmThicknessConstant, 200.f,
                        "The thickness (in nanometers) of the thin-film layer assuming it is enabled on non-replaced \"legacy\" materials.\n"
                        "Should be any value larger than 0, typically within the wavelength of light, but must be less than or equal to "
                        "OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS ((1500.0f) nm).");
};

/// The resolved rtx.legacyMaterial.* values (what LegacyMaterialData::createDefault / the opaque material of a
/// non-replaced draw reads).
struct LegacyMaterialDefaultValues {
    float anisotropy = 0.f;
    float emissiveIntensity = 0.f;
    bool useAlbedoTextureIfPresent = true;
    options::Vec3f albedoConstant{1.f, 1.f, 1.f};
    float opacityConstant = 1.f;
    float roughnessConstant = 0.7f;
    float metallicConstant = 0.1f;
    options::Vec3f emissiveColorConstant{0.f, 0.f, 0.f};
    bool enableEmissive = false;
    bool ignoreAlphaChannel = false;
    bool enableThinFilm = false;
    bool alphaIsThinFilmThickness = false;
    float thinFilmThicknessConstant = 200.f;
};
LegacyMaterialDefaultValues legacyMaterialDefaults();

} // namespace fuse::relight::scene
