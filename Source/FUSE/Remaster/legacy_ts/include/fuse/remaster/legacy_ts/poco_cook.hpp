// FUSE Remaster W1.3: POCO -> cooked FUSE assets (docs/plans/FUSE_REMASTER_PLAN.md §2.4, §7 W1.3).
//
//   Mesh       -> FMSH v2 `.fusemesh` (serialize_cooked_mesh): positions / normals / uv0 as stored (float), material
//                 slot names (the slot's Material record name), and Joints0 / Weights0 for skinned meshes. Always v2
//                 (material slots are a v2 stream). The strict cook's §5.3 mesh gates (asset_geometry triangle sanity,
//                 asset_skin weights; asset_rules.hpp) run on the written file, so a POCO cooks only if the strict
//                 AssetCooker would accept the same geometry.
//   TextureSet -> `.fusetex` through the strict AssetCooker (albedo: BC7 sRGB with mips; the §5.3 texture gate).
//   Material   -> binary `.fusemat` (fuse_project encodeCookedMaterial): name, base colour, roughness, metallic and
//                 the albedo texture's cook id (the `.fusetex` stem, as the T3D material cook writes it).
//   Skeleton   -> FSKL `.fskl` rig (fuse::animation::Skeleton::save; bind = inverse of inverseBind).
//
// cookPocoShape writes one asset in the asset plan's layout and a fuse_assetcheck descriptor next to it, so the
// asset_* gates (`fuse_assetcheck check <base>.asset.json`) judge exactly the cooked files:
//   <dir>/<base>_lod<N>.fusemesh, <base>_albedo.fusetex (+ <base>_albedo.<ext>, the POCO albedo blob as the gate's
//   texture source), <base>.fusemat, <base>_rig.fskl, <base>.asset.json
// with id "<class>/<style>/<base>", units m, up y (the POCO is FUSE space), the LOD screen errors given, and for
// skins the rig + its skeleton hash (FNV-1a 64 over names + parents: the `.fuseanim` and skin-gate identity).
#pragma once

#include <fuse/cook/mesh_cook.hpp>
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/store.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fuse::remaster::legacy_ts {

/// Records a cook reads (looked up by id: materials and texture sets that LOD submeshes / materials reference).
struct PocoRecordSet {
    std::vector<poco::Record> records;
    [[nodiscard]] const poco::Record* find(const std::string& id) const;
};

/// One POCO Mesh -> FMSH v2 bytes (gates not run). `slotNames` resolves materialSlot ids to names.
bool pocoMeshToCooked(const store::BlobStore& store, const poco::Mesh& mesh, const PocoRecordSet& records,
                      cook::CookedMesh& out, std::string* error = nullptr);

/// pocoMeshToCooked + serialize + the strict mesh gates + write `outPath`. FMSH bytes in `bytes` when given.
bool cookPocoMesh(const store::BlobStore& store, const poco::Mesh& mesh, const PocoRecordSet& records,
                  const std::filesystem::path& outPath, std::string* error = nullptr, std::vector<u8>* bytes = nullptr);

struct PocoShapeCookOptions {
    std::string assetClass = "prop";     ///< §1.2 class (descriptor "class")
    std::string style = "torque";        ///< id middle segment
    std::string category = "crate";      ///< §1.6 height category
    std::string base;                    ///< file base / id last segment (<name>_<variant>)
    std::vector<f32> lodErrors;          ///< screen-space error per LOD (LOD 0 = 0); default 0, 0.01, 0.02, ...
    f32 maxViewDistance = 100.f;
};

struct PocoShapeCookResult {
    std::vector<std::string> meshes; ///< LOD files, LOD 0 first
    std::string texture;             ///< `.fusetex` ("" without a texture set)
    std::string textureSource;
    std::string material;            ///< `.fusemat` ("" without a material)
    std::string rig;                 ///< `.fskl` ("" for static meshes)
    std::string descriptor;          ///< `<base>.asset.json`
    std::string assetId;
    u64 skeletonHash = 0;
};

/// Cook LOD 0 (`lods[0]`, whose lodIds name the rest, all in `records`) with its first submesh's material, that
/// material's texture set and the mesh's skeleton into `dir`, plus the assetcheck descriptor.
bool cookPocoShape(const store::BlobStore& store, const PocoRecordSet& records, const std::string& lod0Id,
                   const std::filesystem::path& dir, const PocoShapeCookOptions& options, PocoShapeCookResult& out,
                   std::string* error = nullptr);

} // namespace fuse::remaster::legacy_ts
