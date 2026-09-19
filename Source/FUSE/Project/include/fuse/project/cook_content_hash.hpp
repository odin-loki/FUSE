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

// --- deepen additive from deepen-b79-cooker-hash-preflight-6a72 ---
struct CookFnvInputPreflight {
[[nodiscard]] CookFnvInputPreflight preflight_fnv1a64_input(const u8* data, usize size);

// --- deepen additive from deepen-b79-cooker-hash-be66 ---
[[nodiscard]] CookHashPreflight preflight_cacheable_cook_key(u64 source_hash, u64 upstream_hash);

// --- deepen additive from deepen-fuse-b79-cooker-hash-fdd2 ---
[[nodiscard]] CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry,

// --- deepen additive from deepen-b79-cooker-hash-ff33 ---
[[nodiscard]] CookHashPreflight preflight_manifest_cook_hash(const CookManifestEntry& entry,

// --- deepen additive from deepen-b79-cooker-hash-b4e0 ---
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_upstream_hash(const CookManifestEntry& entry,

// --- deepen additive from deepen-b79-cooker-hash-reconcile-5e9c ---
[[nodiscard]] CookHashPreflight preflight_manifest_entry_dependencies(const CookManifestEntry& entry);

// --- deepen additive from deepen-b79-cooker-hash-83b8 ---
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_dependencies_hash(

// --- deepen additive from deepen-b79-cooker-hash-88c3 ---
[[nodiscard]] CookHashPreflight preflight_manifest_dependency_coverage(

// --- deepen additive from deepen-b79-cooker-hash-4ea3 ---
[[nodiscard]] CookHashPreflight preflight_file_mtime_ns(const std::string& path);
[[nodiscard]] CookHashPreflight preflight_upstream_dependency_path(const std::string& dependency_output_path,
[[nodiscard]] CookHashPreflight preflight_fnv1a64_combine(u64 left, u64 right);

// --- deepen additive from deepen-b79-cooker-hash-0e64 ---
[[nodiscard]] CookHashPreflight preflight_manifest_dependency_outputs(const CookManifestEntry& entry,
[[nodiscard]] CookHashPreflight preflight_file_mtime(const std::string& path);

// --- deepen additive from deepen-fuse-b79-cooker-hash-1101 ---
[[nodiscard]] CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry);

// --- deepen additive from deepen-b79-cooker-hash-2ca8 ---
[[nodiscard]] CookHashPreflight preflight_import_paths(const std::string& input_path,

// --- deepen additive from deepen-b79-cooker-hash-4af9 ---
[[nodiscard]] CookHashPreflight preflight_manifest_entry_with_dependencies(const CookManifestEntry& entry,

// --- deepen additive from deepen-b79-cooker-hash-90b0 ---
[[nodiscard]] CookHashPreflight preflight_manifest_cook_key(const CookManifestEntry& entry,

// --- deepen additive from b79-cooker-hash-deepen-6979 ---
[[nodiscard]] CookHashPreflight preflight_mesh_import_cook_key(const MeshImportDesc& desc, u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_texture_import_cook_key(const TextureImportDesc& desc, u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_audio_import_cook_key(const AudioImportDesc& desc, u64 upstream_hash = 0);

// --- deepen additive from deepen-b79-cooker-hash-9039 ---
[[nodiscard]] inline bool is_valid_cook_hash_preflight(const CookHashPreflight& preflight) {
[[nodiscard]] CookHashPreflight preflight_mesh_import_cache_key(const MeshImportDesc& desc, u64 upstream_hash = 0);
[[nodiscard]] CookHashPreflight preflight_texture_import_cache_key(const TextureImportDesc& desc,
[[nodiscard]] CookHashPreflight preflight_audio_import_cache_key(const AudioImportDesc& desc, u64 upstream_hash = 0);

// --- deepen additive from deepen-b79-cooker-hash-478e ---
[[nodiscard]] bool tryPreflightMeshImportHash(const MeshImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightTextureImportHash(const TextureImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightAudioImportHash(const AudioImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightManifestEntryHash(const CookManifestEntry& entry, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightCookCacheEntry(const CookCacheEntry& entry, CookHashRejectReason& reason);
[[nodiscard]] inline bool shouldSkipManifestEntryHash(const CookManifestEntry& entry) {

// --- deepen additive from b79-cooker-hash-deepen-3135 ---
[[nodiscard]] CookHashPreflight preflight_shader_manifest_hash(const CookManifestEntry& entry);

// --- deepen additive from deepen-b79-cooker-hash-34c2 ---
[[nodiscard]] CookHashRejectReason classifyCookHashReject(const CookHashPreflight& preflight);
[[nodiscard]] bool wouldHashFileContent(const std::string& path);
[[nodiscard]] bool wouldHashMeshImport(const MeshImportDesc& desc);
[[nodiscard]] bool wouldHashTextureImport(const TextureImportDesc& desc);
[[nodiscard]] bool wouldHashAudioImport(const AudioImportDesc& desc);
[[nodiscard]] bool wouldHashManifestEntry(const CookManifestEntry& entry);
[[nodiscard]] bool wouldHashUpstreamDependencies(const std::vector<std::string>& dependency_output_paths,
[[nodiscard]] bool wouldHashCookCacheKey(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool tryPreflightFileContentHash(const std::string& path, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightUpstreamDependenciesHash(
[[nodiscard]] bool tryPreflightCookCacheKey(u64 source_hash, u64 upstream_hash, CookHashRejectReason& reason);

// --- deepen additive from b79-cooker-hash-skip-guards-93f1 ---
[[nodiscard]] bool should_skip_cook_cache_store(const CookCacheEntry& entry);
[[nodiscard]] bool should_skip_file_content_hash(const std::string& path);
[[nodiscard]] bool should_skip_mesh_import_hash(const MeshImportDesc& desc);
[[nodiscard]] bool should_skip_texture_import_hash(const TextureImportDesc& desc);
[[nodiscard]] bool should_skip_audio_import_hash(const AudioImportDesc& desc);
[[nodiscard]] bool should_skip_manifest_entry_hash(const CookManifestEntry& entry);
[[nodiscard]] bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
[[nodiscard]] bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash);
[[nodiscard]] bool should_skip_fnv1a64_bytes(const u8* data, usize size);
