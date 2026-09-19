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

/// Why a cook cache key fold was rejected (B7.9 deepen).
enum class CookCacheKeyRejectReason : u8 {
    None = 0,
    ZeroSource,
    ZeroFold,
};
/// Human-readable label for cache key reject reasons (B7.9 deepen).
[[nodiscard]] const char* cookCacheKeyRejectReasonLabel(CookCacheKeyRejectReason reason);
/// Non-mutating preflight for `combine_cook_cache_key` — valid folds return true (B7.9 deepen).
[[nodiscard]] bool preflight_cook_cache_key(u64 source_hash, u64 upstream_hash,
                                              CookCacheKeyRejectReason* reason = nullptr);

/// Why a source content hash probe was rejected (B7.9 deepen).
enum class CookHashRejectReason : u8 {
    None = 0,
    EmptyPath,
    Unreadable,
};
/// Human-readable label for content hash reject reasons (B7.9 deepen).
[[nodiscard]] const char* cookHashRejectReasonLabel(CookHashRejectReason reason);
/// Non-mutating preflight for `hash_file_content` — writes hash on success (B7.9 deepen).
[[nodiscard]] bool preflight_hash_file_content(const std::string& path, u64* out_hash = nullptr,
                                               CookHashRejectReason* reason = nullptr);
/// Non-mutating preflight for `hash_mesh_import` — writes hash on success (B7.9 deepen).
[[nodiscard]] bool preflight_mesh_import_hash(const MeshImportDesc& desc, u64* out_hash = nullptr,
                                              CookHashRejectReason* reason = nullptr);

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

} // namespace fuse::project
