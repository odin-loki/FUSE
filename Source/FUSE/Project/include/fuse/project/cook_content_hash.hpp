#pragma once

#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

namespace fuse::project {
struct CookCacheEntry;
} // namespace fuse::project
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
    UnknownDependencyOutput,
    UnresolvedDependency,
    MissingManifestDependency,
    ZeroSourceHash,
    ZeroContentHash,
    NonCacheableKey,
    UnresolvedDependency,
    NonCacheableCombinedKey,
    InvalidCacheEntry,
    InvalidCacheKey,
    UnresolvedDependencyOutput,
    UnsupportedAssetKind,
    UnknownDependency,
    UnsupportedKind,
};

struct CookCacheEntry;

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
    /// True when hashing should be skipped — mirrors net preflight `should_skip` (B7.9 deepen).
    /// Non-mutating skip predicate — mirrors \c ok() inversion (B7.9 deepen).
    [[nodiscard]] bool should_skip_hash() const { return !can_hash; }
    /// True when hashing must be skipped — mirrors `!ok()` (B7.9 deepen).
    /// True when hashing would be rejected — mirrors `ok()` negation (B7.9 deepen).
    /// True when hashing should be skipped — mirrors empty-input guards (B7.9 deepen).
    /// True when hashing should be skipped — mirrors guard paths without computing keys (B7.9 deepen).
};

/// True when hash preflight succeeded — mirrors `CookHashPreflight::ok()` (B7.9 deepen).
[[nodiscard]] inline bool is_valid_cook_hash_preflight(const CookHashPreflight& preflight) {
    return preflight.ok();
}

/// FNV-1a 64-bit hash over raw bytes — shared by cook cache keys (B7.9 deepen stub).
[[nodiscard]] u64 fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] u64 fnv1a64_combine(u64 left, u64 right);

/// Read-only FNV input guard — rejects null pointer with non-zero length (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size);

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
/// Read-only FNV input preflight — mirrors `is_valid_fnv1a64_input` without hashing (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size);

/// Last-write-time in nanoseconds; returns 0 when the path is missing or unreadable.
[[nodiscard]] u64 file_mtime_ns(const std::string& path);
/// Read-only mtime preflight — rejects empty paths without touching the filesystem (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_file_mtime_ns(const std::string& path);

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
/// Walks `entry.dependencies` for readable source paths — mirrors `hash_manifest_entry` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry,
                                                              const CookManifest& manifest);
/// Dependency path readability preflight — skips empty dependency strings (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_dependencies(const CookManifestEntry& entry);
/// Manifest entry plus dependency-list preflight when non-empty deps are present (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
/// Rejects unknown manifest output paths — mirrors silent skips in `hash_upstream_dependencies` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
/// Resolve each non-empty manifest dependency output to a readable source (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_dependency_outputs(const CookManifestEntry& entry,
                                                                      const CookManifest& manifest);
/// Empty-path guard for mtime reads — mirrors `file_mtime_ns` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_file_mtime(const std::string& path);
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
/// Validates source hash plus combined fold cacheability — additive guard beyond `preflight_cook_cache_key` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cacheable_cook_key(u64 source_hash, u64 upstream_hash);
/// Read-only cache-key fold preflight — mirrors `combine_cook_cache_key` zero guards (B7.9 deepen).
/// Read-only FNV input preflight — mirrors `is_valid_fnv1a64_input` without hashing (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_cook_hash(const CookManifestEntry& entry,
/// Entry source plus upstream dependency readability — does not alter `preflight_manifest_entry_hash` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_upstream_hash(const CookManifestEntry& entry,
/// Read-only FNV input guard — mirrors `is_valid_fnv1a64_input` with reject reason (B7.9 deepen).
/// Read-only manifest entry guard — source plus dependency paths must be readable (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_dependencies(const CookManifestEntry& entry);
/// Cacheable fold preflight — rejects zero combined keys after source/upstream guards (B7.9 deepen).
/// Manifest entry plus dependency source readability — additive over `preflight_manifest_entry_hash` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_dependencies_hash(
    const CookManifestEntry& entry, const CookManifest& manifest);
/// Structural cache-entry preflight — mirrors `is_valid_cook_cache_entry` without storing (B7.9 deepen).
/// Non-empty dependency output paths must resolve to manifest entries (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_dependency_coverage(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
/// Mirrors `is_cacheable_cook_cache_key` — rejects zero source fold with `ZeroSourceHash` (B7.9 deepen).
/// Empty-path guard for mtime reads — mirrors `file_mtime_ns` (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_file_mtime_ns(const std::string& path);
/// Single dependency output path must resolve in the manifest (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_upstream_dependency_path(const std::string& dependency_output_path,
                                                                 const CookManifest& manifest);
/// FNV combine preflight — rejects zero left operand (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_fnv1a64_combine(u64 left, u64 right);
/// Path-only preflight for deferred shader cooks — no source readability required (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry);
/// Manifest entry plus optional upstream dependency preflight — read-only planning guard (B7.9 deepen).
/// Cacheability preflight — mirrors `is_cacheable_cook_cache_key` without folding keys (B7.9 deepen).
/// Shared empty-path guard for import descriptors — mirrors mesh/texture/audio hash preconditions (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_import_paths(const std::string& input_path,
                                                       const std::string& output_path);
/// Structural + source-readability preflight for cache records — shader kind is rejected (B7.9 deepen).
/// Manifest entry plus upstream dependency preflight — empty deps skip upstream fold (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_dependencies(const CookManifestEntry& entry,
/// Structural cache-entry preflight — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
/// Manifest entry plus upstream dependency preflight — guarded on empty deps (B7.9 deepen).
/// Structural + source readability guard for cache persistence — mirrors `CookCache::store` (B7.9 deepen).
/// Cacheable fold preflight — mirrors `is_cacheable_cook_cache_key` (B7.9 deepen).

/// Convenience skip probes — mirror preflight `ok()` without computing keys (B7.9 deepen).
[[nodiscard]] inline bool should_skip_file_content_hash(const std::string& path) {
    return !preflight_file_content_hash(path).ok();
}
[[nodiscard]] inline bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return !preflight_mesh_import_hash(desc).ok();
[[nodiscard]] inline bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return !preflight_texture_import_hash(desc).ok();
[[nodiscard]] inline bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return !preflight_audio_import_hash(desc).ok();
[[nodiscard]] inline bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return !preflight_manifest_entry_hash(entry).ok();
[[nodiscard]] inline bool should_skip_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest) {
    return !preflight_upstream_dependencies_hash(dependency_output_paths, manifest).ok();
[[nodiscard]] inline bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return !preflight_combine_cook_cache_key(source_hash, upstream_hash).ok();
/// Manifest entry cook-key preflight — import hash plus upstream dependency fold (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_manifest_cook_key(const CookManifestEntry& entry,
/// Import descriptor cook-key preflight — import hash plus optional upstream fold (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_mesh_import_cook_key(const MeshImportDesc& desc, u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_texture_import_cook_key(const TextureImportDesc& desc, u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_audio_import_cook_key(const AudioImportDesc& desc, u64 upstream_hash = 0);
/// Import descriptor preflight chained with cache-key fold — mirrors `hash_*_import` guards (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_mesh_import_cache_key(const MeshImportDesc& desc, u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_texture_import_cache_key(const TextureImportDesc& desc,
                                                                   u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_audio_import_cache_key(const AudioImportDesc& desc, u64 upstream_hash = 0);

/// Structural + source readability preflight for cache records (B7.9 deepen).

/// Bool preflight entry points — mirror `preflight_*` without allocating (B7.9 deepen).
[[nodiscard]] bool tryPreflightMeshImportHash(const MeshImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightTextureImportHash(const TextureImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightAudioImportHash(const AudioImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightManifestEntryHash(const CookManifestEntry& entry, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightCookCacheEntry(const CookCacheEntry& entry, CookHashRejectReason& reason);

/// Read-only skip predicates — true when matching hash/cache preflight would fail (B7.9 deepen).
[[nodiscard]] inline bool shouldSkipMeshImportHash(const MeshImportDesc& desc) {
[[nodiscard]] inline bool shouldSkipTextureImportHash(const TextureImportDesc& desc) {
[[nodiscard]] inline bool shouldSkipAudioImportHash(const AudioImportDesc& desc) {
[[nodiscard]] inline bool shouldSkipManifestEntryHash(const CookManifestEntry& entry) {
[[nodiscard]] inline bool shouldSkipCookCacheEntry(const CookCacheEntry& entry) {
    return !preflight_cook_cache_entry(entry).ok();
/// Structural + source readability preflight for cache records — mirrors `store` guards (B7.9 deepen).
/// Shader manifest entries are not hashable in the stub cook path (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_shader_manifest_hash(const CookManifestEntry& entry);

/// Map a preflight result to its reject reason — `None` when `ok()` (B7.9 deepen).
[[nodiscard]] CookHashRejectReason classifyCookHashReject(const CookHashPreflight& preflight);

/// Non-mutating hash preflight predicates — mirror `preflight_*` without computing keys (B7.9 deepen).
[[nodiscard]] bool wouldHashFileContent(const std::string& path);
[[nodiscard]] bool wouldHashMeshImport(const MeshImportDesc& desc);
[[nodiscard]] bool wouldHashTextureImport(const TextureImportDesc& desc);
[[nodiscard]] bool wouldHashAudioImport(const AudioImportDesc& desc);
[[nodiscard]] bool wouldHashManifestEntry(const CookManifestEntry& entry);
[[nodiscard]] bool wouldHashUpstreamDependencies(const std::vector<std::string>& dependency_output_paths,
[[nodiscard]] bool wouldHashCookCacheKey(u64 source_hash, u64 upstream_hash);

/// Bool preflight wrappers that populate `reason` on rejection (B7.9 deepen).
[[nodiscard]] bool tryPreflightFileContentHash(const std::string& path, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightUpstreamDependenciesHash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest,
    CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightCookCacheKey(u64 source_hash, u64 upstream_hash, CookHashRejectReason& reason);
/// Cacheability preflight — mirrors `is_cacheable_cook_cache_key` with reject diagnostics (B7.9 deepen).

/// Structural cache-entry preflight — mirrors `CookCache::store` guards (B7.9 deepen).

/// True when `CookCache::store` would reject `entry` (B7.9 deepen).
[[nodiscard]] bool should_skip_cook_cache_store(const CookCacheEntry& entry);

/// Convenience skip guards — mirror each `preflight_*_hash` helper (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// True when hash preflight would reject the input — mirrors `preflight_*` guards (B7.9 deepen).
[[nodiscard]] bool should_skip_upstream_dependencies_hash(


    return preflight_file_content_hash(path).should_skip();
    return preflight_mesh_import_hash(desc).should_skip();
    return preflight_texture_import_hash(desc).should_skip();
    return preflight_audio_import_hash(desc).should_skip();
[[nodiscard]] inline bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
[[nodiscard]] inline bool should_skip_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cacheable_cook_cache_key(source_hash, upstream_hash).should_skip();

/// Non-mutating skip predicate — mirrors \c CookHashPreflight::should_skip_hash (B7.9 deepen).
[[nodiscard]] inline bool should_skip_cook_hash_preflight(const CookHashPreflight& preflight) {
    return preflight.should_skip_hash();
}

[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// True when hash preflight would reject the input — mirrors `CookHashPreflight::should_skip` (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Read-only skip probes — mirror `preflight_*` guards without computing keys (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                                          const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// True when hash preflight would reject — mirrors `preflight_*` without computing keys (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Non-mutating hash skip predicates — mirror `preflight_*` without building diagnostics (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                                          const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// True when file content hashing should be skipped — mirrors `hash_file_content` empty/unreadable guards (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
/// True when import/manifest/upstream hashing should be skipped — mirrors respective `hash_*` guards (B7.9 deepen).
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

/// Read-only skip probes — mirror `preflight_*` guards without computing keys (B7.9 deepen).
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest);
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

} // namespace fuse::project
