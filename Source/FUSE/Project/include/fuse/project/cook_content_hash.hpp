#pragma once

#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

/// Why a cook content-hash preflight rejected the request (B7.9 deepen).
enum class CookHashPreflightRejectReason : u8 {
    None = 0,
    EmptyPath,
    MissingFile,
    EmptyInputOrOutput,
    ZeroSourceHash,
};

/// Human-readable label for hash preflight reject reasons (logging / tests).
const char* cookHashPreflightRejectReasonLabel(CookHashPreflightRejectReason reason);

/// Preflight guard before `hash_file_content` — true when the path is non-empty and readable.
[[nodiscard]] bool preflight_hash_file_content(const std::string& path,
                                               CookHashPreflightRejectReason* reason = nullptr);

/// Preflight guard before import/manifest hashing — true when inputs would yield a non-zero hash.
[[nodiscard]] bool preflight_hash_mesh_import(const MeshImportDesc& desc,
                                              CookHashPreflightRejectReason* reason = nullptr);
[[nodiscard]] bool preflight_hash_texture_import(const TextureImportDesc& desc,
                                                 CookHashPreflightRejectReason* reason = nullptr);
[[nodiscard]] bool preflight_hash_audio_import(const AudioImportDesc& desc,
                                               CookHashPreflightRejectReason* reason = nullptr);
[[nodiscard]] bool preflight_hash_manifest_entry(const CookManifestEntry& entry,
                                                 CookHashPreflightRejectReason* reason = nullptr);

/// Preflight guard before `combine_cook_cache_key` — true when the fold would be cacheable.
[[nodiscard]] bool preflight_combine_cook_cache_key(u64 source_hash,
                                                    u64 upstream_hash,
                                                    CookHashPreflightRejectReason* reason = nullptr);

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

/// Content hash over source bytes plus import descriptor knobs (identical inputs → identical hash).
[[nodiscard]] u64 hash_mesh_import(const MeshImportDesc& desc);
[[nodiscard]] u64 hash_texture_import(const TextureImportDesc& desc);
[[nodiscard]] u64 hash_audio_import(const AudioImportDesc& desc);
[[nodiscard]] u64 hash_manifest_entry(const CookManifestEntry& entry);

} // namespace fuse::project
