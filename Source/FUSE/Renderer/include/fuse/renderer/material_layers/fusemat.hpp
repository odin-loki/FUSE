#pragma once

// Asset plan W0.7 (docs/plans/FUSE_ASSET_PLAN.md §5.1): the `.fusemat` material format.
//
// Source: JSON (`*.fusemat.json`, committed recipes), strict: unknown keys, wrong types and out-of-range values are
// errors with a JSON path ("layers[1].contrast: ..."). Cooked: binary `.fusemat` (little endian):
//
//   "FMAT" | u32 version (1) | u32 payload bytes | payload | u64 FNV-1a 64 of every preceding byte
//
// payload = the FuseMat fields in declaration order (strings as u16 length + bytes, enums as u32, f32 raw bits).
// The cook is deterministic (same FuseMat -> same bytes) and the loader refuses a bad magic / version / size /
// trailer. Texture slots are names (cook ids); resolve_fusemat() turns them into texture indices at load time
// (bindless / pool indices) and produces the GPU record MlMaterial.
//
// Schema (version 1), every key optional except "fusemat" and "name":
//   {
//     "fusemat": 1, "name": "rock/alpine/granite_a",
//     "shading_model": "default_lit" | "subsurface" | "foliage" | "clear_coat" | "cloth" | "unlit",
//     "category": "generic" | "stone" | "soil" | "sand" | "snow" | "ice" | "wood" | "foliage" | "metal" | "fabric"
//                 | "plaster" | "brick" | "glass" | "water" | "flesh" | "plastic",      (footsteps / impacts / audio)
//     "wind": "none" | "grass" | "leaves" | "branch" | "trunk",
//     "base": { "albedo": [r, g, b] (linear), "roughness": f, "metallic": f, "normal_strength": f,
//               "textures": { "albedo": "id", "normal": "id" } },
//     "uv_scale": f (> 0; texture repeats per UV unit, or per metre with triplanar),
//     "triplanar": { "enabled": b, "sharpness": f in [1, 16] },
//     "stochastic": { "enabled": b, "lattice": f in (0, 16] },
//     "macro": { "scale": f > 0, "strength": f in [0, 1] },
//     "detail": { "albedo": "id", "normal": "id", "scale": f in [1, 32], "strength": f in [0, 2],
//                 "fade": [start, end] (metres, end > start) },
//     "layers": [ (at most 3) { "name": "moss", "mode": "height" | "wet",
//                 "mask": "constant" | "vertex_r" | "vertex_g" | "vertex_b" | "vertex_a" | "slope_up" | "world_height",
//                 "mask_bias": f, "mask_scale": f, "coverage": f in [0, 1], "contrast": f in [1, 64],
//                 "albedo": [r, g, b], "roughness": f, "metallic": f, "uv_scale": f > 0, "normal_strength": f,
//                 "textures": { "albedo": "id", "normal": "id" } } ],
//     "procedural": { "function": u32 id (0 = none), "params": [up to 8 f] }
//   }
// Calibration (§1.6): an untextured dielectric albedo (metallic < 0.5) in [0.02, 0.9] linear, metal albedo >= 0.45
// (a textured albedo factor is a tint in [0, 1]; the texture gates calibrate the texture); roughness and
// metallic in [0, 1]; a wet layer's albedo is a darkening factor in (0, 1].

#include <fuse/renderer/material_layers/ml_types.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::renderer::material_layers {

inline constexpr u32 kFuseMatVersion = 1u;
inline constexpr u32 kFuseMatMagic = 0x54414D46u; ///< "FMAT" little endian
inline constexpr u32 kFuseMatMaxProceduralParams = 8u;

enum class FuseMatShading : u32 { DefaultLit = 0, Subsurface, Foliage, ClearCoat, Cloth, Unlit, Count };
enum class FuseMatCategory : u32 {
    Generic = 0, Stone, Soil, Sand, Snow, Ice, Wood, Foliage, Metal, Fabric, Plaster, Brick, Glass, Water, Flesh,
    Plastic, Count
};
enum class FuseMatWind : u32 { None = 0, Grass, Leaves, Branch, Trunk, Count };

struct FuseMatTextureSet {
    std::string albedo; ///< cook id, empty = none
    std::string normal;
    bool operator==(const FuseMatTextureSet&) const = default;
};

struct FuseMatLayer {
    std::string name;
    MlLayerMode mode = kMlLayerHeight;
    MlMask mask = kMlMaskConstant;
    f32 maskBias = 0.f;
    f32 maskScale = 1.f;
    f32 coverage = 1.f;
    f32 contrast = 8.f;
    f32 albedo[3] = {1.f, 1.f, 1.f};
    f32 roughness = 1.f;
    f32 metallic = 0.f;
    f32 uvScale = 1.f;
    f32 normalStrength = 1.f;
    FuseMatTextureSet textures;
    bool operator==(const FuseMatLayer&) const = default;
};

struct FuseMat {
    u32 version = kFuseMatVersion;
    std::string name;
    FuseMatShading shading = FuseMatShading::DefaultLit;
    FuseMatCategory category = FuseMatCategory::Generic;
    FuseMatWind wind = FuseMatWind::None;
    f32 albedo[3] = {0.5f, 0.5f, 0.5f};
    f32 roughness = 1.f;
    f32 metallic = 0.f;
    f32 normalStrength = 1.f;
    FuseMatTextureSet textures;
    f32 uvScale = 1.f;
    bool triplanar = false;
    f32 triplanarSharpness = 4.f;
    bool stochastic = false;
    f32 stochasticLattice = 2.f;
    f32 macroScale = 0.25f;
    f32 macroStrength = 0.f;
    FuseMatTextureSet detail;
    f32 detailScale = 8.f;
    f32 detailStrength = 1.f;
    f32 detailFade[2] = {5.f, 30.f};
    std::vector<FuseMatLayer> layers;
    u32 proceduralFunction = 0;
    std::vector<f32> proceduralParams;
    bool operator==(const FuseMat&) const = default;
};

/// One validation / parse error: `path` is a JSON path ("layers[0].contrast", "" for the document).
struct FuseMatError {
    std::string path;
    std::string message;
};

struct FuseMatResult {
    bool ok = false;
    std::vector<FuseMatError> errors;
    /// "path: message" lines.
    std::string describe() const;
};

/// Checks the ranges / counts of a FuseMat (what the JSON parser and the binary loader also run).
FuseMatResult validate_fusemat(const FuseMat& m);

/// Parses the JSON source (strict) and validates it. `out` is filled as far as it parsed.
FuseMatResult parse_fusemat_json(std::string_view text, FuseMat& out);
/// Canonical JSON (stable key order, every key written, shortest round-trip floats): parse(write(m)) == m.
std::string write_fusemat_json(const FuseMat& m);

/// The cooked binary (deterministic).
std::vector<u8> write_fusemat_binary(const FuseMat& m);
/// Loads a cooked binary (magic, version, sizes, trailer, then validate_fusemat).
FuseMatResult read_fusemat_binary(const u8* data, usize size, FuseMat& out);

/// Texture resolution at load time: cook id -> texture index (kMlNoTexture = not found).
using FuseMatTextureResolver = std::function<u32(std::string_view id)>;

/// The GPU record of a validated FuseMat. An unresolved texture id is an error ("textures.albedo: unknown texture").
FuseMatResult resolve_fusemat(const FuseMat& m, const FuseMatTextureResolver& resolve, MlMaterial& out);

const char* fusemat_shading_name(FuseMatShading s);
const char* fusemat_category_name(FuseMatCategory c);
const char* fusemat_wind_name(FuseMatWind w);
const char* fusemat_mask_name(MlMask m);
const char* fusemat_layer_mode_name(MlLayerMode m);

} // namespace fuse::renderer::material_layers
