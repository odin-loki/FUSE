#pragma once

#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

struct CookCacheEntry;

enum class CookHashRejectReason : u8 {
    None,
    NullData,
    EmptyPath,
    EmptyInputPath,
    EmptyOutputPath,
    SourceUnreadable,
    EmptyDependencyList,
    ZeroSourceHash,
    ZeroContentHash,
};

/// Read-only hash preflight — mirrors empty-input guards without computing keys (B7.9 deepen).
struct CookHashPreflight {
    bool can_hash = false;
    CookHashRejectReason reason = CookHashRejectReason::None;

    [[nodiscard]] bool ok() const { return can_hash; }
    /// True when hashing should be skipped — mirrors `!ok()` (B7.9 deepen).
    [[nodiscard]] bool should_skip() const { return !can_hash; }
/// Non-null buffer with at least one byte — required before hashing raw spans (B7.9 deepen).
[[nodiscard]] inline bool is_hashable_byte_span(const u8* data, usize size) {
    return data != nullptr && size > 0;
/// Zero is reserved — unreadable or unhashable sources must not produce cache keys (B7.9 deepen).
[[nodiscard]] inline bool is_valid_content_hash(u64 hash) {
    return hash != 0;
}

/// FNV-1a 64-bit hash over raw bytes — shared by cook cache keys (B7.9 deepen stub).
[[nodiscard]] u64 fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] u64 fnv1a64_combine(u64 left, u64 right);

/// True when `size == 0` or `data` is non-null — guards null pointer with non-zero length (B7.9 deepen).
[[nodiscard]] inline bool is_valid_fnv1a64_input(const u8* data, usize size) {
    return size == 0 || data != nullptr;
}

/// Zero is reserved for unreadable paths and rejected cache keys (B7.9 deepen).
[[nodiscard]] inline bool is_zero_cook_hash(u64 hash) {
    return hash == 0;

/// True when `path` is non-empty and yields a non-zero content hash (B7.9 deepen).
[[nodiscard]] bool is_readable_cook_source_path(const std::string& path);

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
/// Returns zero when `source_hash` is zero — upstream alone is never cacheable.
/// Returns zero when `source_hash` is zero (B7.9 deepen guard).
[[nodiscard]] u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// True when `combine_cook_cache_key` would yield a non-zero lookup key (B7.9 deepen).
[[nodiscard]] inline bool is_valid_combined_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return combine_cook_cache_key(source_hash, upstream_hash) != 0;
}

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

const char* cookHashRejectReasonLabel(CookHashRejectReason reason);

[[nodiscard]] CookHashPreflight preflight_file_content_hash(const std::string& path);
[[nodiscard]] CookHashPreflight preflight_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] CookHashPreflight preflight_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
[[nodiscard]] CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash);
/// Null-pointer guard for non-zero byte spans — mirrors `is_valid_fnv1a64_input` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size);
/// Fold source/upstream preflight — upstream zero is allowed on valid source keys (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);
/// Structural cache-entry preflight — mirrors `is_valid_cook_cache_entry` guards (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);

} // namespace fuse::project
