#pragma once

// UNI-U7-MAT-1: Torque 3D material definitions -> cooked `.fusemat` + strictly cooked `.fusetex`.
//
// Sources understood:
//   * TorqueScript `singleton Material(Name) { ... };` / `new Material(Name) { ... };` blocks in `.mat`,
//     `.tscript` and `.cs` files (materials.tscript, level material files). `Material(Name : Parent)`
//     inherits the fields of a Parent defined earlier in the same parse. Keys are case-insensitive (as in
//     TorqueScript); only stage 0 (`diffuseMap[0]`, a bare `diffuseMap` is stage 0) is read.
//   * T3D 4.x `MaterialAsset` TAML files (`<Material Name=... mapTo=...>` with a first
//     `<Stages_beginarray DiffuseMapAsset=... />` stage).
//
// Fields read: mapTo, diffuseMap / diffuseMapAsset, normalMap / normalMapAsset, diffuseColor, roughness,
// metalness, specular / specularPower (older materials: roughness = sqrt(2 / (specularPower + 2))),
// emissive, translucent, alphaTest, alphaRef, doubleSided.
//
// Map references resolve (resolveT3DMaterialMapPath) in T3D order: an `@asset=` / `Module:Asset` image
// asset through the ImageAsset `.asset.taml` files under the search roots; a path relative to the
// defining file (T3D's "same folder" rule, `./` and `~/` prefixes included); a path relative to the game
// root; the mounted VFS (remapLegacyAssetPath). Extension-less references try .png .jpg .jpeg .tga .bmp
// .ktx2 in that order.
//
// Cooking (cookT3DMaterials) is strict (ImportValidation::Strict): albedo maps -> BC7 sRGB, normal maps ->
// BC5 linear, each source image cooked once per call (reused by every material that names it). A map
// that does not resolve or does not decode leaves that texture slot empty (counted; no placeholder
// texture is written) and the material is still emitted with its constant colour. Texture slots of
// the `.fusemat` hold cook ids = the cooked texture's file stem, which the renderer's cooked-asset
// registry resolves. `.fusemat` v1 has no alpha fields: alpha settings are parsed and reported
// (T3DMaterialDef) but not written.

#include <fuse/asset/cooked_material.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::project {

struct T3DMaterialDef {
    std::string name;
    std::string parent;
    std::string mapTo;
    std::string diffuseMap;      ///< path-style map reference (stage 0)
    std::string diffuseMapAsset; ///< "Module:Asset" image asset reference
    std::string normalMap;
    std::string normalMapAsset;
    f32 diffuseColor[4] = {1.f, 1.f, 1.f, 1.f};
    bool hasRoughness = false;
    f32 roughness = 1.f;
    bool hasMetalness = false;
    f32 metalness = 0.f;
    bool hasSpecularPower = false;
    f32 specularPower = 8.f;
    bool emissive = false;
    bool translucent = false;
    bool alphaTest = false;
    f32 alphaRef = 0.f;
    bool doubleSided = false;
    /// File the definition came from (map references resolve relative to it).
    std::string sourcePath;
};

struct T3DMaterialParseResult {
    std::vector<T3DMaterialDef> materials;
    /// `Material(...)` headers found (a malformed block is counted but not returned).
    u32 blocksSeen = 0;
    std::string note;
};

/// TorqueScript text (`.mat` / `.tscript` / `.cs`).
[[nodiscard]] T3DMaterialParseResult parseT3DMaterialScript(std::string_view text, const std::string& sourcePath = {});
/// MaterialAsset TAML text (`*.asset.taml`).
[[nodiscard]] T3DMaterialParseResult parseT3DMaterialAssetTaml(std::string_view text,
                                                               const std::string& sourcePath = {});
/// Reads `path` and dispatches on its extension (`.taml` -> TAML, anything else -> TorqueScript).
[[nodiscard]] T3DMaterialParseResult parseT3DMaterialFile(const std::string& path);

struct T3DMaterialCookOptions {
    /// Where `<MaterialName>.fusemat` files go; textures go to `<outputDir>/textures/`.
    std::string outputDir;
    /// Directories searched (recursively) for ImageAsset `.asset.taml` files (asset-style references).
    std::vector<std::string> assetSearchRoots;
    /// Game root for game-relative references (`art/...`, `data/...`); optional.
    std::string gameRoot;
    /// Also try the process VFS (`/t3d/`, `/game/` mounts) for references.
    bool useVfs = true;
};

struct T3DCookedMaterialEntry {
    std::string name;
    std::string mapTo;
    std::string fusematPath;
    std::string albedoCookId; ///< empty = no albedo texture
    std::string normalCookId;
    std::string albedoSource; ///< resolved source image path
    std::string normalSource;
    bool written = false;
    std::string note;
};

struct T3DMaterialCookResult {
    u32 materialCount = 0;
    u32 fusematWritten = 0;
    u32 texturesCooked = 0;  ///< distinct cooked .fusetex files
    u32 textureReuses = 0;   ///< slots served by a texture cooked earlier in the call
    u32 mapsReferenced = 0;  ///< texture slots with a reference
    u32 mapsResolved = 0;    ///< references that resolved to a source file
    u32 mapsUnresolved = 0;
    u32 textureCookFailures = 0; ///< resolved sources the strict cook refused
    std::vector<T3DCookedMaterialEntry> entries;
    std::string note;
};

/// Source image for a map reference of `material` ("" when nothing resolves).
[[nodiscard]] std::string resolveT3DMaterialMapPath(const std::string& mapRef, bool assetReference,
                                                    const std::string& definingFile,
                                                    const T3DMaterialCookOptions& options);

/// The `.fusemat` record of a definition (texture slots = the given cook ids). Values are clamped
/// into the validator's ranges (asset::validate_cooked_material).
[[nodiscard]] asset::CookedMaterial t3dMaterialToCooked(const T3DMaterialDef& material, const std::string& albedoCookId,
                                                        const std::string& normalCookId);

/// Binary `.fusemat` writer (the layout asset::read_cooked_material reads). Validates first; the
/// written bytes are read back through asset::read_cooked_material before returning true.
bool writeCookedMaterialFile(const asset::CookedMaterial& material, const std::string& path,
                             std::string* error = nullptr);
[[nodiscard]] std::vector<u8> encodeCookedMaterial(const asset::CookedMaterial& material);

/// Strict texture cook + `.fusemat` emission for parsed definitions.
[[nodiscard]] T3DMaterialCookResult cookT3DMaterials(const std::vector<T3DMaterialDef>& materials,
                                                     const T3DMaterialCookOptions& options);
/// parseT3DMaterialFile + cookT3DMaterials.
[[nodiscard]] T3DMaterialCookResult cookT3DMaterialFile(const std::string& path, const T3DMaterialCookOptions& options);

} // namespace fuse::project
