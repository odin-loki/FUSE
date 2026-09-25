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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_material_data.h@0867d3c (the LIST_OPAQUE_MATERIAL_*,
// LIST_TRANSLUCENT_MATERIAL_* and LIST_PORTAL_MATERIAL_* parameter tables: USD token, type, range, default),
// src/dxvk/shaders/rtx/concept/surface/surface_shared.h@0867d3c (BlendType / AlphaTestType values),
// src/dxvk/shaders/rtx/utility/shared_constants.h@0867d3c (OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS),
// src/lssusd/mdl_helpers.h@0867d3c (lss::Mdl filter / wrap values) and the material-type selection of
// src/dxvk/rtx_render/rtx_mod_usd.cpp@0867d3c (UsdMod::Impl::processMaterial).
//
// FUSE Relight RL-3.2: the material model mapping of plan §4.5 / §4.4.
//
//   * The three Remix surface types and every parameter they read from a mod's Shader prim
//     (`inputs:<token>`), with upstream's ranges and defaults verbatim. Values are sanitized the way upstream
//     does (clamp to [min, max]) and every clamp is reported.
//   * The mapping onto the POCO records (Remaster §2.3): the base `Material` + `TextureSet` carry what they can
//     represent exactly (the base colour / opacity / roughness / metallic constants, the alpha reference, the
//     albedo / normal / height / emissive maps), and the `material_ext` record (kind "material_ext", same id)
//     carries every Remix-only parameter (plan §4.4 list: thin film, anisotropy, sprite sheet, blend and alpha
//     test state, legacy alpha state, displacement, SSS, sampler state, translucent IoR / transmittance / thin
//     walls / diffuse layer, the portal index, emission state) plus per-texture metadata for every texture
//     parameter. `materialParamsFromPoco` inverts the mapping; the unit test proves that every parameter of
//     every table round-trips.
//
// FUSE decisions (not upstream facts): the base `Material.model` is Translucent for AperturePBR_Translucent and
// for an opaque material with blend_enabled, Masked for an opaque material whose alpha_test_type is not
// Always, Subsurface for an opaque material with SSS (measurement distance > 0 or a diffusion profile),
// otherwise Opaque (portals too: `material_ext.surface` is authoritative). `emissiveNits` is
// emissive_intensity when enable_emission is set, else 0 (the Remix scale, not photometric nits).
#pragma once

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::mods::import {

namespace json = capture::exporter::json;

/// RtSurfaceMaterialType as selected from `info:mdl:sourceAsset`.
enum class SurfaceType : std::uint8_t { Opaque, Translucent, Portal };
const char* surfaceTypeName(SurfaceType t);
std::optional<SurfaceType> surfaceTypeFromName(std::string_view name);

/// processMaterial: "AperturePBR_Portal.mdl" -> Portal; "AperturePBR_Translucent.mdl" -> Translucent, or Portal
/// when the shader still has the legacy `rayPortalIndex` attribute; anything else (or no source asset) -> Opaque.
/// `known` is false when a non-empty source asset names none of the three AperturePBR modules.
SurfaceType surfaceTypeFromMdl(std::string_view sourceAsset, bool hasLegacyRayPortalIndex, bool* known = nullptr);

enum class ParamType : std::uint8_t { Texture, Float, Vec3, Bool, U8, BlendType, AlphaTestType };
const char* paramTypeName(ParamType t);

/// BlendType (surface_shared.h).
inline constexpr std::uint32_t kBlendTypeMax = 10;     ///< kReverseColor
inline constexpr std::uint32_t kBlendTypeAlpha = 0;
/// AlphaTestType (surface_shared.h; the VkCompareOp values).
inline constexpr std::uint32_t kAlphaTestNever = 0;
inline constexpr std::uint32_t kAlphaTestAlways = 7;
/// OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS (nm).
inline constexpr float kThinFilmMaxThickness = 1500.0f;
/// The float16 maximum upstream clamps many constants to.
inline constexpr float kFloat16Max = 65504.0f;

/// One row of a Remix parameter table. Scalars use component 0; bools and integers are stored as floats.
struct ParamDesc {
    std::string_view name; ///< the USD token without "inputs:" (e.g. "emissive_intensity")
    ParamType type = ParamType::Float;
    std::array<float, 3> minValue{};
    std::array<float, 3> maxValue{};
    std::array<float, 3> defaultValue{};
    /// Carried by the base POCO Material / TextureSet (else by material_ext).
    bool inBaseMaterial = false;
};

/// The parameter table of a surface type, in upstream order (textures first, then constants). The portal's
/// second mask texture (`unused_in_usd_so_dont`) is never read from USD upstream and is not listed.
std::span<const ParamDesc> materialParamTable(SurfaceType t);
const ParamDesc* findParam(SurfaceType t, std::string_view name);

struct ParamValue {
    ParamType type = ParamType::Float;
    std::array<float, 3> value{};  ///< non-texture parameters
    std::string asset;             ///< Texture: the authored asset path ("" = no texture)
    std::string colorSpace;        ///< Texture: the authored `colorSpace` metadata ("" when not authored)
    friend bool operator==(const ParamValue&, const ParamValue&) = default;
};

/// Everything the importer reads from one material's Shader prim.
struct MaterialParams {
    SurfaceType surface = SurfaceType::Opaque;
    std::string mdlSourceAsset;   ///< info:mdl:sourceAsset (authored)
    std::string mdlSubIdentifier; ///< info:mdl:sourceAsset:subIdentifier
    std::map<std::string, ParamValue> values; ///< every table parameter (defaults filled in)
    std::set<std::string> authored;           ///< parameters authored on the shader (upstream "dirty" bits)
    bool ignoreMaterial = false;  ///< inputs:ignore_material
    bool preloadTextures = false; ///< inputs:preload_textures
    friend bool operator==(const MaterialParams&, const MaterialParams&) = default;
};

/// The defaults of a surface type (nothing authored).
MaterialParams defaultMaterialParams(SurfaceType t);

struct ParamIssue {
    std::string param;
    std::string message;
};

/// Reads the shader's parameters (`inputs:<token>`; with `allowUnprefixed`, a bare `<token>` attribute is used
/// when the prefixed one is absent: RL-1.8 captures write the sampler state that way). Type mismatches keep
/// the default and are reported; values are sanitized (clamped) and every clamp is reported.
MaterialParams readMaterialParams(const usd::Prim* shader, SurfaceType surface, bool allowUnprefixed,
                                  std::vector<ParamIssue>* issues);

/// Upstream's clamp of every constant to its range; returns the names that changed.
std::vector<std::string> sanitizeMaterialParams(MaterialParams& p);

/// Base texture-set slot of a texture parameter: "albedo", "normal", "height", "emissive" or "" (ext only).
std::string_view textureSetSlot(SurfaceType t, std::string_view param);

/// The base POCO `Material` model name for these parameters (see the header comment).
std::string materialModel(const MaterialParams& p);

/// The base `Material` payload (Remaster §2.3 field order).
json::Value materialPayload(const MaterialParams& p, const std::string& textureSetId);

/// Per-texture facts the importer adds to material_ext.textures[<param>] next to the authored path and colour
/// space (the resolved store path, the blob, the decoded format...). Keyed by parameter name.
using TextureExtras = std::map<std::string, json::Value>;

/// The `material_ext` payload: surface, mdl, Remix flags, `params` (every non-texture parameter that the base
/// Material does not carry), `textures` (every authored texture parameter), `authored` (sorted names).
json::Value materialExtPayload(const MaterialParams& p, const TextureExtras& textures);

/// Inverse of materialPayload + materialExtPayload: nullopt (with `error`) when a record is malformed.
std::optional<MaterialParams> materialParamsFromPoco(const json::Value& materialPayload, const json::Value& extPayload,
                                                     std::string* error = nullptr);

} // namespace fuse::relight::mods::import
