#pragma once

#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

/// Preflight checks before hashing a filesystem source (B7.9 deepen follow-up).
struct CookHashPreflight {
    bool empty_path = true;
    bool missing_file = false;
    bool unreadable = false;

    [[nodiscard]] bool can_hash() const { return !empty_path && !missing_file && !unreadable; }
    [[nodiscard]] bool should_skip() const { return !can_hash(); }
};

/// Preflight checks before folding source and upstream hashes into a cache key (B7.9 deepen follow-up).
struct CookCacheKeyPreflight {
    bool zero_source_hash = true;
    bool zero_upstream_hash = false;

    [[nodiscard]] bool can_combine() const { return !zero_source_hash; }
    [[nodiscard]] bool should_skip() const { return !can_combine(); }
};

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

/// Combine source/descriptor hash with upstream dependency hash for cache lookup.
[[nodiscard]] u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Preflight file-content hashing without reading bytes (B7.9 deepen follow-up).
[[nodiscard]] CookHashPreflight preflight_hash_file_content(const std::string& path);
[[nodiscard]] bool should_skip_hash_file_content(const std::string& path);

/// Preflight import descriptor hashing — mirrors zero-hash guards on valid paths (B7.9 deepen follow-up).
[[nodiscard]] CookHashPreflight preflight_hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_hash_manifest_entry(const CookManifestEntry& entry);

[[nodiscard]] CookCacheKeyPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

} // namespace fuse::project
