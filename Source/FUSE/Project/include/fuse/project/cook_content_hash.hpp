#pragma once

#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

/// FNV-1a 64-bit hash over raw bytes — shared by cook cache keys (B7.9 deepen stub).
[[nodiscard]] u64 fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] u64 fnv1a64_combine(u64 left, u64 right);

/// Last-write-time in nanoseconds; returns 0 when the path is missing or unreadable.
[[nodiscard]] u64 file_mtime_ns(const std::string& path);

/// Hash source path + mtime + file bytes (stub content key); returns 0 when unreadable.
[[nodiscard]] u64 hash_file_content(const std::string& path);

/// Fold upstream dependency source hashes into a cook cache key (manifest output paths).
[[nodiscard]] u64 hash_upstream_dependencies(const std::vector<std::string>& dependency_output_paths,
                                             const CookManifest& manifest);

/// Preflight for cook cache key folding — zero source hash is not cacheable (B7.9 deepen).
struct CookCacheKeyPreflight {
    bool zero_source_hash = false;

    [[nodiscard]] bool can_fold() const { return !zero_source_hash; }
    [[nodiscard]] bool should_skip() const { return !can_fold(); }
};

/// Preflight import descriptor hashing without computing keys (B7.9 deepen).
struct CookImportHashPreflight {
    bool empty_input_path = false;
    bool empty_output_path = false;
    bool unreadable_source = false;

    [[nodiscard]] bool can_hash() const { return !empty_input_path && !empty_output_path && !unreadable_source; }
    [[nodiscard]] bool should_skip() const { return !can_hash(); }
};

/// Preflight cache key fold without combining hashes (B7.9 deepen).
[[nodiscard]] CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Preflight mesh import hashing — empty paths and unreadable sources are rejected (B7.9 deepen).
[[nodiscard]] CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc);

/// Combine source/descriptor hash with upstream dependency hash for cache lookup.
[[nodiscard]] u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

} // namespace fuse::project
