// FUSE Relight RL-3.1: the Remix view of a composed mod stage (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.2, §4.4).
//
// Semantics follow dxvk-remix src/dxvk/rtx_render/rtx_mod_usd.cpp@0867d3c (MIT; read for the interface facts
// only, no code copied):
//   * prim classification (classifyChangedPath / getModelHash / getLightHash / getMaterialHash):
//       /RootNode/meshes/mesh_<H>   mesh replacement,   H = strtoull(name + 5, 16) (0: not a replacement)
//       /RootNode/Looks/mat_<H>     material replacement; a Material without the prefix is keyed by the hash
//                                   of its shader's strongest opinion site (XXH64 of the layer's real path,
//                                   then of the spec path)
//       /RootNode/lights/light_<H>  light replacement (legacy "sphereLight_<H>" names too)
//     only active children of those three sections count; other names there are reported as unrecognized.
//   * categories (processCategoryFlags): `remix_category:<name>` bool attributes on the prim; an authored
//     attribute sets the category's "exists" bit, its value the flag bit.
//   * particles (processParticleSystem): prims with the ParticleSystemAPI schema carry `primvars:particle:*`
//     attributes. RL-3.1 surfaces them as authored values; curve baking belongs to the importer (RL-3.2).
#pragma once

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::mods::usd {

using hash::Hash64;

inline constexpr std::string_view kRootNode = "/RootNode";
inline constexpr std::string_view kMeshSection = "/RootNode/meshes";
inline constexpr std::string_view kLooksSection = "/RootNode/Looks";
inline constexpr std::string_view kLightsSection = "/RootNode/lights";
inline constexpr std::string_view kParticleSystemApi = "ParticleSystemAPI";
inline constexpr std::string_view kParticlePrimvarPrefix = "primvars:particle:";

enum class PrimClass : unsigned char {
    None,         ///< not a replacement root (outside the sections, or deeper than their children)
    Section,      ///< /RootNode or one of the three section prims
    Mesh,         ///< /RootNode/meshes/mesh_<H>
    Material,     ///< /RootNode/Looks/mat_<H>
    Light,        ///< /RootNode/lights/light_<H> (or sphereLight_<H>)
    Unrecognized, ///< a child of a section whose name has no hash (hash 0)
};
const char* primClassName(PrimClass c);

struct Classification {
    PrimClass cls = PrimClass::None;
    Hash64 hash = 0;
    bool legacyLightName = false; ///< "sphereLight_" prefix
    friend bool operator==(const Classification&, const Classification&) = default;
};

/// Classifies a prim path by name only (no stage access): the replacement root prims and the sections.
Classification classifyPrimPath(std::string_view path);

/// `remix_category:<name>` for each scene::InstanceCategories (Remix getInstanceCategorySubKey).
const char* remixCategoryAttributeName(scene::InstanceCategories c);

struct CategoryOverrides {
    scene::CategoryFlags exists; ///< categories with an authored attribute
    scene::CategoryFlags flags;  ///< categories authored true
};
CategoryOverrides readCategories(const Prim& prim);

struct ParticleSystem {
    /// Authored `primvars:particle:<name>` values by <name> (e.g. "minTimeToLive", "minColor:values").
    std::map<std::string, Value> primvars;
    const Value* get(std::string_view name) const;
    std::optional<double> number(std::string_view name) const;
};
/// nullopt unless the prim applies ParticleSystemAPI.
std::optional<ParticleSystem> readParticleSystem(const Prim& prim);

struct MeshReplacement {
    Hash64 hash = 0;
    std::string path;
    std::vector<std::string> meshPrims;  ///< descendant UsdGeomMesh prims (active), depth first
    std::vector<std::string> lightPrims; ///< descendant UsdLux lights (active)
    std::vector<std::string> materialBindings; ///< material:binding targets of the root and its meshes
    CategoryOverrides categories;              ///< from the root prim, else the first descendant that authors any
    std::optional<ParticleSystem> particles;   ///< from the root, else the first descendant applying the API
    std::optional<bool> preserveOriginalDrawCall;
    bool instanceable = false;
};

struct MaterialReplacement {
    Hash64 hash = 0;
    bool hashFromName = true; ///< false: keyed by the shader's strongest opinion site (Remix fallback)
    std::string path;
    std::string shaderPath;
    std::string mdlSourceAsset;   ///< info:mdl:sourceAsset (authored), e.g. "AperturePBR_Opacity.mdl"
    std::string mdlSubIdentifier; ///< info:mdl:sourceAsset:subIdentifier
    std::optional<ParticleSystem> particles;
};

struct LightReplacement {
    Hash64 hash = 0;
    std::string path;
    std::string type; ///< SphereLight, DistantLight, RectLight, DiskLight, CylinderLight, DomeLight
    bool legacyName = false;
};

struct RemixMod {
    std::vector<MeshReplacement> meshes;
    std::vector<MaterialReplacement> materials;
    std::vector<LightReplacement> lights;
    std::vector<Diagnostic> diagnostics; ///< unrecognized names, missing shaders, ...
};

/// The Remix replacements of a composed mod stage (upstream UsdMod::Impl::processUSD walk order: materials,
/// meshes, lights; children in composed order).
RemixMod collectRemixMod(const ComposedStage& stage);

} // namespace fuse::relight::mods::usd
