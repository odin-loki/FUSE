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
    NonCacheableKey,
    NonCacheableCombinedKey,
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
/// Why a cook content-hash preflight rejected the request (B7.9 deepen).
enum class CookHashPreflightRejectReason : u8 {
    None = 0,
    MissingFile,
    EmptyInputOrOutput,

/// Human-readable label for hash preflight reject reasons (logging / tests).
const char* cookHashPreflightRejectReasonLabel(CookHashPreflightRejectReason reason);

/// Preflight guard before `hash_file_content` — true when the path is non-empty and readable.
[[nodiscard]] bool preflight_hash_file_content(const std::string& path,
                                               CookHashPreflightRejectReason* reason = nullptr);

/// Preflight guard before import/manifest hashing — true when inputs would yield a non-zero hash.
[[nodiscard]] bool preflight_hash_mesh_import(const MeshImportDesc& desc,
[[nodiscard]] bool preflight_hash_texture_import(const TextureImportDesc& desc,
[[nodiscard]] bool preflight_hash_audio_import(const AudioImportDesc& desc,
[[nodiscard]] bool preflight_hash_manifest_entry(const CookManifestEntry& entry,
                                              CookHashPreflightRejectReason* reason = nullptr);

/// Preflight guard before `combine_cook_cache_key` — true when the fold would be cacheable.
[[nodiscard]] bool preflight_combine_cook_cache_key(u64 source_hash,
                                                    u64 upstream_hash,

/// Preflight for file-content hashing — identifies empty paths and unreadable sources (B7.9 deepen).
struct CookContentHashPreflight {
    bool empty_path = false;
    bool missing_file = false;
    bool unreadable_file = false;

    [[nodiscard]] bool can_hash() const { return !empty_path && !missing_file && !unreadable_file; }

/// Preflight for import descriptor hashing — mirrors `hash_*_import` rejection paths (B7.9 deepen).
struct CookImportHashPreflight {
    bool empty_input_path = false;
    bool empty_output_path = false;
    bool source_unhashable = false;

    [[nodiscard]] bool can_hash() const {
        return !empty_input_path && !empty_output_path && !source_unhashable;
/// Why hash preflight rejected a cook cache key input (B7.9 deepen).
enum class CookHashPreflightReject : u8 {
    ZeroKey,

/// Read-only hash preflight — mirrors `hash_*` guards without folding bytes (B7.9 deepen).
    bool ok = false;
    CookHashPreflightReject reject = CookHashPreflightReject::None;

/// Preflight guard before `hash_upstream_dependencies` — true when deps would fold cleanly.
[[nodiscard]] bool preflight_hash_upstream_dependencies(
    const std::vector<std::string>& dependency_output_paths,
    const CookManifest& manifest,
/// Preflight checks before hashing a filesystem source (B7.9 deepen follow-up).
    bool empty_path = true;
    bool unreadable = false;

    [[nodiscard]] bool can_hash() const { return !empty_path && !missing_file && !unreadable; }
    [[nodiscard]] bool should_skip() const { return !can_hash(); }

/// Preflight checks before folding source and upstream hashes into a cache key (B7.9 deepen follow-up).
struct CookCacheKeyPreflight {
    bool zero_source_hash = true;
    bool zero_upstream_hash = false;

    [[nodiscard]] bool can_combine() const { return !zero_source_hash; }
    [[nodiscard]] bool should_skip() const { return !can_combine(); }
                                                    CookHashPreflightRejectReason* reason = nullptr);

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
[[nodiscard]] CookHashPreflight preflight_hash_file_content(const std::string& path);
[[nodiscard]] CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry);
/// Preflight FNV-1a byte input — null pointer with non-zero length is rejected (B7.9 deepen).
struct CookFnvInputPreflight {
    bool null_data = false;

    [[nodiscard]] bool can_hash() const { return !null_data; }
    [[nodiscard]] bool should_skip() const { return !can_hash(); }
};

[[nodiscard]] CookFnvInputPreflight preflight_fnv1a64_input(const u8* data, usize size);

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

/// Preflight guard before cache lookup/store — true when the combined key is cacheable.
[[nodiscard]] CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Why a file-content hash preflight rejected the path (B7.9 deepen).
enum class CookFileHashRejectReason : u8 {
    EmptyPath,
    UnreadableSource,

/// Read-only file hash diagnostics — no mutation (B7.9 deepen).
struct CookFileHashPreflight {
    CookFileHashRejectReason reason = CookFileHashRejectReason::None;

    [[nodiscard]] bool canHash() const { return valid; }

/// Preflight guard before hashing source bytes — true when `hash_file_content` would be non-zero.
[[nodiscard]] CookFileHashPreflight preflight_file_content_hash(const std::string& path);
/// Why a cook cache key fold was rejected (B7.9 deepen).
    None = 0,
    ZeroSource,
    ZeroFold,
/// Human-readable label for cache key reject reasons (B7.9 deepen).
[[nodiscard]] const char* cookCacheKeyRejectReasonLabel(CookCacheKeyRejectReason reason);
/// Non-mutating preflight for `combine_cook_cache_key` — valid folds return true (B7.9 deepen).
[[nodiscard]] bool preflight_cook_cache_key(u64 source_hash, u64 upstream_hash,
                                              CookCacheKeyRejectReason* reason = nullptr);

/// Why a source content hash probe was rejected (B7.9 deepen).
enum class CookHashRejectReason : u8 {
    Unreadable,
/// Human-readable label for content hash reject reasons (B7.9 deepen).
[[nodiscard]] const char* cookHashRejectReasonLabel(CookHashRejectReason reason);
/// Non-mutating preflight for `hash_file_content` — writes hash on success (B7.9 deepen).
[[nodiscard]] bool preflight_hash_file_content(const std::string& path, u64* out_hash = nullptr,
                                               CookHashRejectReason* reason = nullptr);
/// Non-mutating preflight for `hash_mesh_import` — writes hash on success (B7.9 deepen).
[[nodiscard]] bool preflight_mesh_import_hash(const MeshImportDesc& desc, u64* out_hash = nullptr,
[[nodiscard]] inline CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookHashPreflight result;
    result.ok = combine_cook_cache_key(source_hash, upstream_hash) != 0;
    if (!result.ok) {
        result.reject = CookHashPreflightReject::ZeroKey;
    return result;

/// Non-mutating preflight for combined cook cache keys (B7.9 deepen).
    bool zero_source = true;
    bool zero_combined = true;
    bool cacheable = false;

    [[nodiscard]] bool can_cache() const { return cacheable; }

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
/// Read-only preflight for filesystem-backed content keys — no byte reads (B7.9 deepen).
struct CookHashPreflight {
    bool empty_path = false;
    bool empty_input_path = false;
    bool empty_output_path = false;
    bool source_missing = false;
    bool ok = false;

    [[nodiscard]] bool can_hash() const { return ok; }

[[nodiscard]] CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry);

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

const char* cookHashRejectReasonLabel(CookHashRejectReason reason);

[[nodiscard]] CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size);
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
/// Non-destructive guards before hashing — do not alter `hash_*` return values (B7.9 deepen).
[[nodiscard]] CookContentHashPreflight preflight_file_content_hash(const std::string& path);
[[nodiscard]] CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_manifest_entry(const CookManifestEntry& entry);
/// Null-pointer guard for non-zero-length FNV input — mirrors `is_valid_fnv1a64_input` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_fnv1a64_input(const u8* data, usize size);
[[nodiscard]] CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                                       const CookManifest& manifest);
/// Validates source hash plus combined fold cacheability — additive guard beyond `preflight_cook_cache_key` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cacheable_cook_key(u64 source_hash, u64 upstream_hash);

} // namespace fuse::project
