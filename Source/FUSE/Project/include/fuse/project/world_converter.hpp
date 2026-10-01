#pragma once

#include <fuse/asset/asset_id.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/project/parity_legacy_sources.hpp>
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
