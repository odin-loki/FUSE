// FUSE Remaster W1.4: world_converter extension, T3D mission -> POCO Level whose entities reference POCO assets
// (docs/plans/FUSE_REMASTER_PLAN.md §4.5, §7 W1.4).
//
// convertT3DMissionToPoco walks the mission like convertT3DMissionToFuselevel (fuse_project inventoryT3DMission:
// same objects, same order, same archetype table, same FUSE-space transforms) and:
//   1. imports every distinct TSStatic shape (W1.2 importTorqueShape) into the store and registers it in the
//      replacement DB: shape file -> oaid (sha256 of its bytes) -> replacement (T0) -> the POCO LOD 0 mesh record;
//   2. cooks that POCO mesh (W1.3 cookPocoMesh) to `<cookedRoot>/<shapeCookedRelPath>`, the very file the converted
//      `.fuselevel`'s ecs::MeshAssets::mesh id names (game:/cooked/<rel>), so the TSStatic's shape asset id maps to
//      the POCO record (and through the DB back to the original);
//   3. writes a Level record `<game>.level.<mission stem>` with one Entity per scene entity: archetype, local
//      transform (column-major 4x4, FUSE space), legacyClass / legacyName, components (`parent` index, and for
//      TSStatic `mesh_asset` = {"path": "game:/cooked/<rel>", "id": "<asset id hex>"}), and assetRefs
//      ["oaid:<uuid>", "poco:<mesh record id>"] for TSStatic entities.
// Classes without an archetype keep archetype "" and are counted (the `.fuselevel` converter's unexpected stubs).
#pragma once

#include <fuse/asset/asset_id.hpp>
#include <fuse/remaster/db.hpp>
#include <fuse/remaster/legacy_ts/dts_poco.hpp>
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/store.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fuse::remaster::legacy_ts {

struct MissionPocoOptions {
    std::string gameId = "game";
    poco::Origin origin = poco::Origin::Original;
    std::string licenceId;
    std::vector<std::string> searchRoots;   ///< as cookT3DMissionShapes
    std::filesystem::path cookedRoot;       ///< game:/cooked/ on disk (empty: no cook)
};

struct MissionShapeBinding {
    std::string shapeRef;
    std::string sourcePath;
    std::string cookedRelPath;
    fuse::asset::AssetId cookedAssetId{}; ///< the `.fuselevel` MeshAssets::mesh of the TSStatic entities
    std::string oaid;
    std::string pocoMeshId;               ///< LOD 0 mesh record
    std::vector<std::string> entities;    ///< names of the TSStatic entities using it
};

struct MissionPocoResult {
    poco::Record level;
    std::vector<MissionShapeBinding> shapes;
    u32 entities = 0;
    u32 unexpected = 0;                    ///< entities without an archetype
    std::vector<std::string> unhandledClasses;
};

bool convertT3DMissionToPoco(const std::filesystem::path& mission, store::BlobStore& store, db::Database& database,
                             const MissionPocoOptions& options, MissionPocoResult& out, std::string* error = nullptr);

} // namespace fuse::remaster::legacy_ts
