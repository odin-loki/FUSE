#pragma once

#include <fuse/scene/scene.hpp>

#include <string>

namespace fuse::ecs {
class Registry;
} // namespace fuse::ecs

namespace fuse::scene {

enum class SerialiseStatus : u8 {
    Ok = 0,
    IoError,
    InvalidMagic,
    UnsupportedVersion,
    TruncatedFile,
};

struct SerialiseResult {
    SerialiseStatus status = SerialiseStatus::IoError;
    std::string error;
    /// Set by `load` when the file carried the pre-rename 'ENGC' magic (read by the compat path).
    bool legacyMagic = false;
};

/// Which editor world a `.fuselevel` belongs to (v3 header byte; v1/v2 files are 3D).
enum class SceneDimension : u8 {
    World3D = 0,
    World2D = 1,
};

/// What `loadWithRegistry` found in the file.
struct SceneFileInfo {
    u32 version = 0;
    SceneDimension dimension = SceneDimension::World3D;
    /// True when the file carried a v3 ECS block (component data restored exactly); false for
    /// v1/v2 files, whose entities were rebuilt as Transform-only ECS entities.
    bool hasEcsBlock = false;
    usize ecsEntityCount = 0;
    usize ecsArchetypeCount = 0;
};

/// Binary `.fuselevel` scene format (B3.7).
///
///   v1  header + camera + scene name + entity name table (+ transform table)
///   v2  v1 + parent index table
///   v3  v2 (hierarchy table always present) + scene dimension (header byte) + one ECS block: the
///       complete `ecs::RegistrySerialiser` ('FECS') image of the scene's registry — every
///       registered component (Transform, Mesh, lights, Collider, RigidBody, Script, ...), exact
///       entity ids, generations and free list — stored as `u32 byteCount` + bytes.
///
/// `save(scene, path)` still writes v1/v2 (no registry); `saveWithRegistry` writes v3.
/// `load(path, scene)` reads v1/v2/v3 (the ECS block of a v3 file is skipped);
/// `loadWithRegistry` reads all three into a scene + registry.
class SceneSerialiser {
public:
    /// On-disk bytes "FUSE" (little-endian u32, same convention as RegistrySerialiser 'FECS').
    static constexpr u32 MAGIC = 0x45535546u; // 'FUSE'
    /// Pre-rename magic ('ENGC', bytes "CGNE" on disk). Never written; `load` still accepts it
    /// (compat loader) so existing `.fuselevel` files keep loading. Re-saving upgrades them.
    static constexpr u32 LEGACY_MAGIC_ENGC = 0x454E4743u;
    static constexpr u32 VERSION = 1u;
    static constexpr u32 VERSION_HIERARCHY = 2u; // parent index table after transforms
    static constexpr u32 VERSION_ECS = 3u;       // v2 + dimension + FECS component block

    static SerialiseResult save(const Scene& scene, const std::string& path);
    static SerialiseResult load(const std::string& path, Scene& scene);

    /// v3: `scene` + the full component state of `registry`. Deterministic: saving the result of
    /// `loadWithRegistry` again produces the same bytes.
    static SerialiseResult saveWithRegistry(const Scene& scene, const ecs::Registry& registry,
                                            const std::string& path,
                                            SceneDimension dimension = SceneDimension::World3D);
    /// Replaces `scene` and `registry` with the file's contents. v3: the registry is restored
    /// exactly (entity ids included). v1/v2: the registry is re-initialised and gets one entity per
    /// scene entity (in order) with an `ecs::Transform` from the file's transform and parent.
    /// On failure neither `scene` nor `registry` is modified.
    static SerialiseResult loadWithRegistry(const std::string& path, Scene& scene, ecs::Registry& registry,
                                            SceneFileInfo* info = nullptr);
};

} // namespace fuse::scene
