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
};

/// Binary scene format stub (B3.7) — header + camera + object name table.
class SceneSerialiser {
public:
    static constexpr u32 MAGIC = 0x454E4743u; // 'ENGC'
    static constexpr u32 VERSION = 1u;

    static SerialiseResult save(const Scene& scene, const std::string& path);
    static SerialiseResult load(const std::string& path, Scene& scene);
};

} // namespace fuse::scene
