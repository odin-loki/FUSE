#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

enum class CookAssetKind : u8 {
    Mesh,
    Texture,
    Audio,
    Shader,
};

enum class CookStatus : u8 {
    Ok = 0,
    InvalidInput,
    SourceMissing,
    OutputError,
    UnsupportedKind,
    // Strict import validation (the AssetCooker default) — the source exists but was rejected:
    MalformedSource,        ///< mesh file could not be parsed (truncated, corrupt, not a mesh format)
    InvalidGeometry,        ///< mesh parsed but unusable: no triangles, out-of-range indices, NaN/Inf
    CorruptImage,           ///< texture bytes could not be decoded (truncated, corrupt, not an image)
    InvalidImageDimensions, ///< texture is zero-size or exceeds the cook's maximum dimension
    ImporterUnavailable,    ///< the real importer (assimp / stb_image) is not linked into this build
};

/// True for the statuses strict import validation reports for a rejected-but-present source.
[[nodiscard]] constexpr bool isImportValidationFailure(CookStatus status) {
    return status == CookStatus::MalformedSource || status == CookStatus::InvalidGeometry ||
           status == CookStatus::CorruptImage || status == CookStatus::InvalidImageDimensions ||
           status == CookStatus::ImporterUnavailable;
}

struct CookManifestEntry {
    CookAssetKind kind = CookAssetKind::Mesh;
    std::string source_path;
    std::string output_path;
    std::vector<std::string> dependencies;
};

/// Versioned cook manifest — maps to `cook_manifest.json` beside project.json (B7.9 stub).
struct CookManifest {
    u32 schema_version = 1;
    std::string project_root;
    std::vector<CookManifestEntry> assets;
};

struct CookRecord {
    CookAssetKind kind = CookAssetKind::Mesh;
    std::string source_path;
    std::string output_path;
    CookStatus status = CookStatus::InvalidInput;
    bool ok = false;
    bool cache_hit = false;
    u64 content_hash = 0;
    std::string note;
};

struct CookBatchResult {
    bool ok = false;
    std::vector<CookRecord> records;
    std::string summary;
};

enum class CookManifestLoadStatus : u8 {
    Ok = 0,
    FileNotFound,
    ParseError,
    UnsupportedSchema,
};

struct CookManifestLoadResult {
    CookManifestLoadStatus status = CookManifestLoadStatus::ParseError;
    CookManifest manifest;
    std::string error;
};

CookManifestLoadResult loadCookManifestFromFile(const std::string& path);
CookManifestLoadResult parseCookManifest(const std::string& json, const std::string& project_root);
CookManifest makeDefaultCookManifest(const std::string& project_root);

const char* cookAssetKindName(CookAssetKind kind);
const char* cookStatusName(CookStatus status);

} // namespace fuse::project
