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
};

/// FNV-1a 64-bit hash over raw bytes — shared by cook cache keys (B7.9 deepen stub).
[[nodiscard]] u64 fnv1a64_bytes(const u8* data, usize size);
[[nodiscard]] u64 fnv1a64_combine(u64 left, u64 right);

/// True when `size == 0` or `data` is non-null — guards null pointer with non-zero length (B7.9 deepen).
[[nodiscard]] inline bool is_valid_fnv1a64_input(const u8* data, usize size) {
    return size == 0 || data != nullptr;
}

/// Last-write-time in nanoseconds; returns 0 when the path is missing or unreadable.
[[nodiscard]] u64 file_mtime_ns(const std::string& path);

/// Hash source path + mtime + file bytes (stub content key); returns 0 when unreadable.
[[nodiscard]] u64 hash_file_content(const std::string& path);

/// Fold upstream dependency source hashes into a cook cache key (manifest output paths).
[[nodiscard]] u64 hash_upstream_dependencies(const std::vector<std::string>& dependency_output_paths,
                                             const CookManifest& manifest);

/// Combine source/descriptor hash with upstream dependency hash for cache lookup.
[[nodiscard]] u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

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

// --- deepen additive from b79-cooker-hash-guards-111f ---
struct CookCacheKeyPreflight {
    [[nodiscard]] bool should_skip() const { return !can_fold(); }
struct CookImportHashPreflight {
    [[nodiscard]] bool should_skip() const { return !can_hash(); }
[[nodiscard]] CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc);

// --- deepen additive from deepen-b79-cooker-hash-guards-7fd3 ---
enum class CookCacheKeyRejectReason : u8 {
    CookCacheKeyRejectReason reason = CookCacheKeyRejectReason::None;
enum class CookFileHashRejectReason : u8 {
struct CookFileHashPreflight {
    CookFileHashRejectReason reason = CookFileHashRejectReason::None;
[[nodiscard]] CookFileHashPreflight preflight_file_content_hash(const std::string& path);

// --- deepen additive from b79-hash-preflight-probes-fd33 ---
enum class CookHashPreflightRejectReason : u8 {
const char* cookHashPreflightRejectReasonLabel(CookHashPreflightRejectReason reason);
                                               CookHashPreflightRejectReason* reason = nullptr);

// --- deepen additive from deepen-b79-cooker-hash-preflight-27fe ---
struct CookContentHashPreflight {
[[nodiscard]] CookContentHashPreflight preflight_file_content_hash(const std::string& path);
[[nodiscard]] CookImportHashPreflight preflight_manifest_entry(const CookManifestEntry& entry);

// --- deepen additive from deepen-b79-cooker-hash-7359 ---
[[nodiscard]] const char* cookCacheKeyRejectReasonLabel(CookCacheKeyRejectReason reason);
                                              CookCacheKeyRejectReason* reason = nullptr);
[[nodiscard]] const char* cookHashRejectReasonLabel(CookHashRejectReason reason);
                                               CookHashRejectReason* reason = nullptr);

// --- deepen additive from deepen-b79-cooker-hash-preflight-0c1f ---
enum class CookHashPreflightReject : u8 {
    CookHashPreflightReject reject = CookHashPreflightReject::None;
[[nodiscard]] CookHashPreflight preflight_hash_file_content(const std::string& path);
[[nodiscard]] CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry);
[[nodiscard]] inline CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookHashPreflight result;
        result.reject = CookHashPreflightReject::ZeroKey;

// --- deepen additive from deepen-b79-cooker-hash-preflight-e529 ---
    [[nodiscard]] bool should_skip() const { return !can_combine(); }
[[nodiscard]] bool should_skip_hash_file_content(const std::string& path);
[[nodiscard]] CookHashPreflight preflight_hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] CookHashPreflight preflight_hash_manifest_entry(const CookManifestEntry& entry);
[[nodiscard]] CookCacheKeyPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash);

// --- deepen additive from deepen-b79-cooker-hash-0896 ---
[[nodiscard]] CookHashPreflight preflight_fnv1a64_input(const u8* data, usize size);

// --- deepen additive from deepen-b79-cooker-hash-314f ---
[[nodiscard]] CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash);

// --- deepen additive from deepen-b79-cooker-hash-reconcile-b4f0 ---
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
