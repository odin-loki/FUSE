#pragma once

// UNI-U7-ASSET-1: runtime reader for cooked `.fusemat` materials (asset plan W0.7).
//
// The format is owned by the renderer (Source/FUSE/Renderer/include/fuse/renderer/material_layers/
// fusemat.hpp: JSON source, cook, binary writer, GPU resolve). The renderer's library links fuse_rhi,
// so the runtime asset core carries this dependency-free copy of the *binary reader and its
// validation* only. Byte layout (little endian):
//
//   "FMAT" | u32 version (1) | u32 payload bytes | payload | u64 FNV-1a 64 of every preceding byte
//
// payload = the fields of CookedMaterial in declaration order (strings as u16 length + bytes, enums
// as u32, f32 raw bits, bools as u32 0/1, then u32 layer count + layers, u32 procedural function, u32
// parameter count + params). The reader refuses bad magic / version / size / trailer / trailing bytes
// and then runs the same range checks as the renderer's validate_fusemat. The gate
// fuse_asset_runtime_material cross-checks this reader against the renderer's writer and reader
// field for field whenever the renderer is in the build.
//
// Texture slots stay cook ids (strings); the renderer resolves them at upload (E06).

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::asset {

inline constexpr u32 kCookedMaterialVersion = 1u;
inline constexpr u32 kCookedMaterialMagic = 0x54414D46u; ///< "FMAT" little endian
inline constexpr u32 kCookedMaterialMaxLayers = 3u;      ///< renderer kMlMaxLayers
inline constexpr u32 kCookedMaterialMaxProceduralParams = 8u;

/// Enum values mirror the renderer's FuseMatShading / FuseMatCategory / FuseMatWind / MlMask /
/// MlLayerMode (stored as u32 in the file).
inline constexpr u32 kMaterialShadingCount = 6u;  ///< default_lit .. unlit
inline constexpr u32 kMaterialCategoryCount = 16u; ///< generic .. plastic
inline constexpr u32 kMaterialWindCount = 5u;      ///< none .. trunk
inline constexpr u32 kMaterialMaskCount = 7u;      ///< constant .. world_height
inline constexpr u32 kMaterialLayerModeCount = 2u; ///< height, wet
inline constexpr u32 kMaterialLayerModeWet = 1u;

struct CookedMaterialTextures {
    std::string albedo; ///< cook id, empty = none
    std::string normal;
    bool operator==(const CookedMaterialTextures&) const = default;
};

struct CookedMaterialLayer {
    std::string name;
    u32 mode = 0u; ///< MlLayerMode
    u32 mask = 0u; ///< MlMask
    f32 mask_bias = 0.f;
    f32 mask_scale = 1.f;
    f32 coverage = 1.f;
    f32 contrast = 8.f;
    f32 albedo[3] = {1.f, 1.f, 1.f};
    f32 roughness = 1.f;
    f32 metallic = 0.f;
    f32 uv_scale = 1.f;
    f32 normal_strength = 1.f;
    CookedMaterialTextures textures;
    bool operator==(const CookedMaterialLayer&) const = default;
};

struct CookedMaterial {
    u32 version = kCookedMaterialVersion;
    std::string name;
    u32 shading = 0u;  ///< FuseMatShading
    u32 category = 0u; ///< FuseMatCategory
    u32 wind = 0u;     ///< FuseMatWind
    f32 albedo[3] = {0.5f, 0.5f, 0.5f};
    f32 roughness = 1.f;
    f32 metallic = 0.f;
    f32 normal_strength = 1.f;
    CookedMaterialTextures textures;
    f32 uv_scale = 1.f;
    bool triplanar = false;
    f32 triplanar_sharpness = 4.f;
    bool stochastic = false;
    f32 stochastic_lattice = 2.f;
    f32 macro_scale = 0.25f;
    f32 macro_strength = 0.f;
    CookedMaterialTextures detail;
    f32 detail_scale = 8.f;
    f32 detail_strength = 1.f;
    f32 detail_fade[2] = {5.f, 30.f};
    std::vector<CookedMaterialLayer> layers;
    u32 procedural_function = 0u;
    std::vector<f32> procedural_params;
    bool operator==(const CookedMaterial&) const = default;
};

/// Range checks of the renderer's validate_fusemat. Returns false with "path: message" in `error`
/// (first failure) when any field is out of range.
bool validate_cooked_material(const CookedMaterial& material, std::string* error = nullptr);

/// Parse and validate a cooked `.fusemat` binary. On failure `out` is reset and `error` says why.
bool read_cooked_material(const u8* data, usize size, CookedMaterial& out, std::string* error = nullptr);

/// Read a `.fusemat` from the host file system (tools / tests).
bool read_cooked_material_file(const std::string& path, CookedMaterial& out, std::string* error = nullptr);

} // namespace fuse::asset
