#pragma once

#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

/// Why hash preflight rejected a cook cache key input (B7.9 deepen).
enum class CookHashPreflightReject : u8 {
    None,
    EmptyPath,
    MissingFile,
    ZeroKey,
};

/// Read-only hash preflight — mirrors `hash_*` guards without folding bytes (B7.9 deepen).
struct CookHashPreflight {
    bool ok = false;
    CookHashPreflightReject reject = CookHashPreflightReject::None;
};

/// FNV-1a 64-bit hash over raw bytes — shared by cook cache keys (B7.9 deepen stub).
[[nodiscard]] u64 fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] u64 fnv1a64_combine(u64 left, u64 right);

[[nodiscard]] CookHashPreflight preflight_hash_file_content(const std::string& path);
[[nodiscard]] CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry);

/// Last-write-time in nanoseconds; returns 0 when the path is missing or unreadable.
[[nodiscard]] u64 file_mtime_ns(const std::string& path);

/// Hash source path + mtime + file bytes (stub content key); returns 0 when unreadable.
[[nodiscard]] u64 hash_file_content(const std::string& path);

/// Fold upstream dependency source hashes into a cook cache key (manifest output paths).
[[nodiscard]] u64 hash_upstream_dependencies(const std::vector<std::string>& dependency_output_paths,
                                             const CookManifest& manifest);

/// Combine source/descriptor hash with upstream dependency hash for cache lookup.
[[nodiscard]] u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

[[nodiscard]] inline CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookHashPreflight result;
    result.ok = combine_cook_cache_key(source_hash, upstream_hash) != 0;
    if (!result.ok) {
        result.reject = CookHashPreflightReject::ZeroKey;
    }
    return result;
}

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

} // namespace fuse::project
