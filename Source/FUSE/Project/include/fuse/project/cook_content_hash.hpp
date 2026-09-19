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

/// Combine source/descriptor hash with upstream dependency hash for cache lookup.
[[nodiscard]] u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Why a cook cache key preflight rejected the fold (B7.9 deepen).
enum class CookCacheKeyRejectReason : u8 {
    None,
    ZeroSourceHash,
    UncacheableFold,
};

/// Read-only cache-key diagnostics — no mutation (B7.9 deepen).
struct CookCacheKeyPreflight {
    bool valid = false;
    CookCacheKeyRejectReason reason = CookCacheKeyRejectReason::None;
    u64 combined_key = 0;

    [[nodiscard]] bool canCache() const { return valid; }
};

/// Preflight guard before cache lookup/store — true when the combined key is cacheable.
[[nodiscard]] CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Why a file-content hash preflight rejected the path (B7.9 deepen).
enum class CookFileHashRejectReason : u8 {
    None,
    EmptyPath,
    UnreadableSource,
};

/// Read-only file hash diagnostics — no mutation (B7.9 deepen).
struct CookFileHashPreflight {
    bool valid = false;
    CookFileHashRejectReason reason = CookFileHashRejectReason::None;

    [[nodiscard]] bool canHash() const { return valid; }
};

/// Preflight guard before hashing source bytes — true when `hash_file_content` would be non-zero.
[[nodiscard]] CookFileHashPreflight preflight_file_content_hash(const std::string& path);

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

} // namespace fuse::project
