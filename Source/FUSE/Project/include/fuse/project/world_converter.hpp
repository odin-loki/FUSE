#pragma once

#include <fuse/asset/asset_id.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/project/parity_legacy_sources.hpp>
#include <fuse/scene/scene_snapshot.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

enum class ConvertStatus : u8 {
    Ok = 0,
    IoError,
    ParseError,
    UnsupportedSource,
};

/// UNI-U7-MIS-1: components written per T3D archetype by convertT3DMissionToFuselevel.
struct MissionArchetypeCounts {
    u32 groups = 0;            ///< SimGroup / SimSet: Transform only (hierarchy)
    u32 directionalLights = 0; ///< Sun -> DirectionalLight (azimuth / elevation -> orientation)
    u32 pointLights = 0;       ///< PointLight -> PointLight
    u32 spotLights = 0;        ///< SpotLight -> SpotLight (full cone angles halved)
    u32 ambientLights = 0;     ///< Skylight -> AmbientLight (Sun ambient colour, DDGI on)
    u32 atmospheres = 0;       ///< SkyBox (cubemap material) / ScatterSky (scattering) -> SkyAtmosphere
    u32 fogs = 0;              ///< LevelInfo -> EnvironmentFog (fog colour / density, clip, clear colour)
    u32 meshes = 0;            ///< GroundPlane (builtin plane) / TSStatic (shape asset id) -> Mesh + MeshAssets
    u32 staticColliders = 0;   ///< GroundPlane -> Collider(Plane) + static RigidBody + TagStatic
    u32 spawnMarkers = 0;      ///< SpawnSphere -> SpawnMarker (datablock hash)
    u32 unhandled = 0;         ///< classes without an archetype (kept as nodes + `archetype` wire stub)
};

/// Half extent (metres) of the visual quad a T3D GroundPlane becomes (its collider plane is infinite).
inline constexpr f32 kGroundPlaneHalfExtent = 256.f;

struct ConvertResult {
    ConvertStatus status = ConvertStatus::IoError;
    std::string outputPath;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    /// Stubs for objects the converter has no archetype for (`__fuse.wire|archetype|Name|Class`).
    u32 unexpectedStubCount = 0;
    /// Entities in the v3 ECS block (one per scene entity; wire stubs are component-less).
    u32 ecsEntityCount = 0;
    MissionArchetypeCounts components{};
    /// Distinct MaterialAsset refs in slot order (ecs::Mesh::material_id indexes this list).
    std::vector<std::string> materialSlots;
    std::vector<std::string> unhandledClasses;
    std::string note;
};

/// Convert a T3D `.mis` mission into a `.fuselevel` v3 scene (U7, UNI-U7-MIS-1).
///
/// The scene table keeps names / transforms / hierarchy (+ `__fuse.wire|datablock|...` and `material|...`
/// legacy binding stubs); the v3 ECS block holds one entity per scene entity (same index) with real
/// components from the archetype table (MissionArchetypeCounts). Coordinates are converted from Torque
/// (Z-up: +X right, +Y forward, +Z up) to FUSE (Y-up, right handed): p' = (x, z, -y); T3D's axis-angle
/// `rotation` field becomes a quaternion in the same frame and scale swaps y / z. World matrices are
/// computed (TransformSystem) before saving.
ConvertResult convertT3DMissionToFuselevel(const std::string& missionPath,
                                           const std::string& outputPath);

/// Asset id of the cooked `.fusemat` a T3D MaterialAsset ref ("Module:Name") cooks to:
/// `game:/cooked/materials/Module/Name.fusemat` (materialVirtualPathToCookOutput of the ref's VFS path).
[[nodiscard]] fuse::asset::AssetId t3dMaterialRefAssetId(const std::string& materialRef);

/// Asset plan 4a: one TSStatic shape of a mission and what cookT3DMissionShapes did with it.
struct T3DShapeCookEntry {
    std::string objectName;
    std::string shapeRef;     ///< the `shapeAsset` ("Module:Name") or `shapeName` (game-relative path) field
    std::string sourcePath;   ///< resolved `.dts` / `.dae` (empty when unresolved)
    std::string outputPath;   ///< `<cookedRoot>/<relative>.fusemesh`
    fuse::asset::AssetId id{}; ///< = the entity's ecs::MeshAssets::mesh (`game:/cooked/<relative>.fusemesh`)
    bool ok = false;
    std::string note;
};

struct T3DShapeCookResult {
    bool ok = false;      ///< every TSStatic shape resolved and cooked
    u32 shapes = 0;       ///< distinct shape refs
    u32 cooked = 0;
    u32 unresolved = 0;   ///< no source file found
    u32 failed = 0;       ///< found, but the strict cook refused it
    std::vector<T3DShapeCookEntry> entries;
    std::string note;
};

/// Asset plan 4a (L-DTS): cook the shapes of a mission's TSStatic objects (`.dts` through the in-house reader, `.dae`
/// through assimp; strict AssetCooker, so the §5.3 cook gates apply) to `<cookedRoot>/<relative>.fusemesh`, the
/// `game:/cooked/` layout whose asset ids convertT3DMissionToFuselevel writes into ecs::MeshAssets::mesh
/// (World3D::setCookedAssetRoot(cookedRoot) resolves them at level load). Sources are looked up in `searchRoots`,
/// then (shapeName only) the mission's directory and its ancestors, then the repository's Templates/BaseGame/game;
/// module scans for shapeAsset refs only walk `searchRoots` and Templates/BaseGame/game (4 levels deep):
///   shapeName  "art/shapes/x.dts"       -> <root>/art/shapes/x.dts            -> art/shapes/x.fusemesh
///   shapeAsset "Module:Name"            -> <module dir>/**/Name.asset.taml fileName="@assetFile=..."
///                                          (module dir = the directory of <Module>.module) -> shapes/Module/Name.fusemesh
[[nodiscard]] T3DShapeCookResult cookT3DMissionShapes(const std::string& missionPath, const std::string& cookedRoot,
                                                      const std::vector<std::string>& searchRoots = {});

/// Remaster W1.4: the archetype convertT3DMissionToFuselevel gives a T3D class (the UNI-U7-MIS-1 table, by name):
/// group, directional_light, point_light, spot_light, ambient_light, sky_atmosphere, environment_fog, ground_plane,
/// spawn_marker, static_mesh. Empty: no archetype (the converter keeps the node and adds an `archetype` wire stub).
[[nodiscard]] std::string t3dArchetypeName(const std::string& className);

/// Remaster W1.4: one mission object as the converter sees it (the `.fuselevel` scene entity it becomes).
struct T3DMissionObject {
    std::string className;
    std::string name;
    std::string archetype;            ///< t3dArchetypeName (empty = wiring stub)
    s32 parent = -1;                  ///< index into T3DMissionInventory::objects (-1 = top level)
    fuse::scene::SceneEntityTransform transform{}; ///< local, FUSE space (as written to the scene table)
    std::string datablock;
    std::string materialAsset;
    // TSStatic only:
    std::string shapeRef;             ///< `shapeAsset` or `shapeName`
    std::string shapeSourcePath;      ///< resolved like cookT3DMissionShapes (empty when unresolved)
    std::string shapeCookedRelPath;   ///< below game:/cooked/ (e.g. "shapes/crate.fusemesh")
    fuse::asset::AssetId shapeAssetId{}; ///< = the entity's ecs::MeshAssets::mesh in the converted level
};

struct T3DMissionInventory {
    bool ok = false;
    std::string sceneName;
    std::vector<T3DMissionObject> objects; ///< scene order (the `.fuselevel` scene entity order)
    std::string note;
};

/// Remaster W1.4: the objects of a T3D mission with their archetype, transform and (TSStatic) shape binding, without
/// converting or cooking anything. `searchRoots` as cookT3DMissionShapes.
[[nodiscard]] T3DMissionInventory inventoryT3DMission(const std::string& missionPath,
                                                      const std::vector<std::string>& searchRoots = {});

/// Convert a T2D module script into a minimal `.fuselevel` scene file (U7).
ConvertResult convertT2DModuleToFuselevel(const std::string& modulePath,
                                          const std::string& outputPath);

/// Cook manifest default worlds to `.fuselevel` when legacy sources are present (U7).
std::vector<ConvertResult> convertManifestWorlds(const ProjectManifest& project,
                                                 const std::string& outputRoot = "");

struct Ensure3DWorldResult {
    bool ok = false;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    LegacySourceOrigin sourceOrigin = LegacySourceOrigin::Missing;
    std::string loadedPath;
    std::string note;
};

/// Ensure `.fuselevel` exists (convert from golden/bundled `.mis` when needed).
[[nodiscard]] Ensure3DWorldResult ensureDefault3DWorldReady(const LoadResult& projectLoad);

struct Ensure2DWorldResult {
    bool ok = false;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    LegacySourceOrigin sourceOrigin = LegacySourceOrigin::Missing;
    std::string loadedPath;
    std::string note;
};

/// Ensure default 2D `.fuselevel` exists (convert from golden/bundled `.cs` when needed).
[[nodiscard]] Ensure2DWorldResult ensureDefault2DWorldReady(const LoadResult& projectLoad);

} // namespace fuse::project
