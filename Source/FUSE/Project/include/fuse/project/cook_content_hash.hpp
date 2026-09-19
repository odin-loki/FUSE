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
};

/// Read-only hash preflight — mirrors empty-input guards without computing keys (B7.9 deepen).
struct CookHashPreflight {
    bool can_hash = false;
    CookHashRejectReason reason = CookHashRejectReason::None;

    [[nodiscard]] bool ok() const { return can_hash; }
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

/// Structural + source readability preflight for cache records (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);

/// Bool preflight entry points — mirror `preflight_*` without allocating (B7.9 deepen).
[[nodiscard]] bool tryPreflightMeshImportHash(const MeshImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightTextureImportHash(const TextureImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightAudioImportHash(const AudioImportDesc& desc, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightManifestEntryHash(const CookManifestEntry& entry, CookHashRejectReason& reason);
[[nodiscard]] bool tryPreflightCookCacheEntry(const CookCacheEntry& entry, CookHashRejectReason& reason);

/// Read-only skip predicates — true when matching hash/cache preflight would fail (B7.9 deepen).
[[nodiscard]] inline bool shouldSkipMeshImportHash(const MeshImportDesc& desc) {
    return !preflight_mesh_import_hash(desc).ok();
}
[[nodiscard]] inline bool shouldSkipTextureImportHash(const TextureImportDesc& desc) {
    return !preflight_texture_import_hash(desc).ok();
}
[[nodiscard]] inline bool shouldSkipAudioImportHash(const AudioImportDesc& desc) {
    return !preflight_audio_import_hash(desc).ok();
}
[[nodiscard]] inline bool shouldSkipManifestEntryHash(const CookManifestEntry& entry) {
    return !preflight_manifest_entry_hash(entry).ok();
}
[[nodiscard]] inline bool shouldSkipCookCacheEntry(const CookCacheEntry& entry) {
    return !preflight_cook_cache_entry(entry).ok();
}

} // namespace fuse::project
