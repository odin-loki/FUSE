#pragma once

#include <fuse/scene/scene.hpp>

#include <string>

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

/// Binary scene format stub (B3.7) — header + camera + object name table.
class SceneSerialiser {
public:
    /// On-disk bytes "FUSE" (little-endian u32, same convention as RegistrySerialiser 'FECS').
    static constexpr u32 MAGIC = 0x45535546u; // 'FUSE'
    /// Pre-rename magic ('ENGC', bytes "CGNE" on disk). Never written; `load` still accepts it
    /// (compat loader) so existing `.fuselevel` files keep loading. Re-saving upgrades them.
    static constexpr u32 LEGACY_MAGIC_ENGC = 0x454E4743u;
    static constexpr u32 VERSION = 1u;
    static constexpr u32 VERSION_HIERARCHY = 2u; // parent index table after transforms

    static SerialiseResult save(const Scene& scene, const std::string& path);
    static SerialiseResult load(const std::string& path, Scene& scene);
};

} // namespace fuse::scene
