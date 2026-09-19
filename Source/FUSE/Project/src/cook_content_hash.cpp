#include <fuse/project/cook_content_hash.hpp>

#include <fuse/project/cook_cache.hpp>

#include <filesystem>
#include <fstream>

namespace fuse::project {

namespace {

constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 hash_u64(u64 value) {
    return fnv1a64_bytes(reinterpret_cast<const u8*>(&value), sizeof(value));
}

u64 hash_string(const std::string& text) {
    return fnv1a64_bytes(reinterpret_cast<const u8*>(text.data()), text.size());
}

u64 hash_bool(bool value) {
    return hash_u64(value ? 1u : 0u);
}

} // namespace

u64 fnv1a64_bytes(const u8* data, usize size) {
    if (size == 0) {
        return kFnvOffset;
    }

    u64 hash = kFnvOffset;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

u64 fnv1a64_combine(u64 left, u64 right) {
    return fnv1a64_bytes(reinterpret_cast<const u8*>(&right), sizeof(right)) ^ (left * kFnvPrime);
}

u64 file_mtime_ns(const std::string& path) {
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) {
        return 0;
    }
    return static_cast<u64>(ftime.time_since_epoch().count());
}

u64 hash_file_content(const std::string& path) {
    if (path.empty()) {
        return 0;
    }

    u64 hash = hash_string(path);
    hash = fnv1a64_combine(hash, hash_u64(file_mtime_ns(path)));

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return 0;
    }

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (contents.empty()) {
        return fnv1a64_combine(hash, 0);
    }
    return fnv1a64_combine(hash, fnv1a64_bytes(reinterpret_cast<const u8*>(contents.data()), contents.size()));
}

u64 hash_upstream_dependencies(const std::vector<std::string>& dependency_output_paths,
                               const CookManifest& manifest) {
    if (dependency_output_paths.empty()) {
        return 0;
    }

    u64 hash = 0;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (dependency_output.empty()) {
            continue;
        }
        hash = fnv1a64_combine(hash, hash_string(dependency_output));
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path == dependency_output) {
                hash = fnv1a64_combine(hash, hash_file_content(asset.source_path));
                break;
            }
        }
    }
    return hash;
}

u64 combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    if (source_hash == 0) {
        return 0;
    }
    if (upstream_hash == 0) {
        return source_hash;
    }
    return fnv1a64_combine(source_hash, upstream_hash);
}

u64 hash_mesh_import(const MeshImportDesc& desc) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return 0;
    }

    const u64 file_hash = hash_file_content(desc.input_path);
    if (file_hash == 0) {
        return 0;
    }

    u64 hash = hash_string(desc.input_path);
    hash = fnv1a64_combine(hash, hash_string(desc.output_path));
    hash = fnv1a64_combine(hash, file_hash);
    hash = fnv1a64_combine(hash, hash_bool(desc.generate_tangents));
    hash = fnv1a64_combine(hash, hash_bool(desc.generate_normals));
    hash = fnv1a64_combine(hash, hash_bool(desc.optimise_vertex_cache));
    hash = fnv1a64_combine(hash, hash_bool(desc.generate_lods));
    hash = fnv1a64_combine(hash, hash_u64(desc.lod_count));
    hash = fnv1a64_combine(hash, hash_u64(static_cast<u64>(desc.lod_error_target * 1000000.0f)));
    hash = fnv1a64_combine(hash, hash_bool(desc.compress));
    return hash;
}

u64 hash_texture_import(const TextureImportDesc& desc) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return 0;
    }

    const u64 file_hash = hash_file_content(desc.input_path);
    if (file_hash == 0) {
        return 0;
    }

    u64 hash = hash_string(desc.input_path);
    hash = fnv1a64_combine(hash, hash_string(desc.output_path));
    hash = fnv1a64_combine(hash, file_hash);
    hash = fnv1a64_combine(hash, hash_u64(static_cast<u64>(desc.color_space)));
    hash = fnv1a64_combine(hash, hash_bool(desc.generate_mipmaps));
    hash = fnv1a64_combine(hash, hash_u64(static_cast<u64>(desc.compression)));
    hash = fnv1a64_combine(hash, hash_bool(desc.is_normal_map));
    hash = fnv1a64_combine(hash, hash_bool(desc.is_hdr));
    return hash;
}

u64 hash_audio_import(const AudioImportDesc& desc) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return 0;
    }

    const u64 file_hash = hash_file_content(desc.input_path);
    if (file_hash == 0) {
        return 0;
    }

    u64 hash = hash_string(desc.input_path);
    hash = fnv1a64_combine(hash, hash_string(desc.output_path));
    hash = fnv1a64_combine(hash, file_hash);
    hash = fnv1a64_combine(hash, hash_u64(desc.target_sample_rate));
    hash = fnv1a64_combine(hash, hash_bool(desc.normalise));
    hash = fnv1a64_combine(hash, hash_bool(desc.trim_silence));
    hash = fnv1a64_combine(hash, hash_u64(static_cast<u64>(desc.format)));
    hash = fnv1a64_combine(hash, hash_u64(static_cast<u64>(desc.ogg_quality * 1000000.0f)));
    return hash;
}

const char* cookHashRejectReasonLabel(CookHashRejectReason reason) {
    switch (reason) {
    case CookHashRejectReason::None:
        return "none";
    case CookHashRejectReason::NullData:
        return "null_data";
    case CookHashRejectReason::EmptyPath:
        return "empty_path";
    case CookHashRejectReason::EmptyInputPath:
        return "empty_input_path";
    case CookHashRejectReason::EmptyOutputPath:
        return "empty_output_path";
    case CookHashRejectReason::SourceUnreadable:
        return "source_unreadable";
    case CookHashRejectReason::EmptyDependencyList:
        return "empty_dependency_list";
    case CookHashRejectReason::ZeroSourceHash:
        return "zero_source_hash";
    case CookHashRejectReason::ZeroContentHash:
        return "zero_content_hash";
    }
    return "unknown";
}

CookHashPreflight preflight_file_content_hash(const std::string& path) {
    CookHashPreflight preflight;
    if (path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
        return preflight;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        preflight.reason = CookHashRejectReason::SourceUnreadable;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_mesh_import_hash(const MeshImportDesc& desc) {
    CookHashPreflight preflight;
    if (desc.input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        return preflight;
    }
    if (desc.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }
    return preflight_file_content_hash(desc.input_path);
}

CookHashPreflight preflight_texture_import_hash(const TextureImportDesc& desc) {
    CookHashPreflight preflight;
    if (desc.input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        return preflight;
    }
    if (desc.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }
    return preflight_file_content_hash(desc.input_path);
}

CookHashPreflight preflight_audio_import_hash(const AudioImportDesc& desc) {
    CookHashPreflight preflight;
    if (desc.input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        return preflight;
    }
    if (desc.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }
    return preflight_file_content_hash(desc.input_path);
}

CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry) {
    CookHashPreflight preflight;
    if (entry.source_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        return preflight;
    }
    if (entry.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }
    return preflight_file_content_hash(entry.source_path);
}

CookHashPreflight preflight_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                                     const CookManifest& manifest) {
    CookHashPreflight preflight;
    bool has_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (!dependency_output.empty()) {
            has_non_empty = true;
            break;
        }
    }
    if (!has_non_empty) {
        preflight.reason = CookHashRejectReason::EmptyDependencyList;
        return preflight;
    }

    for (const std::string& dependency_output : dependency_output_paths) {
        if (dependency_output.empty()) {
            continue;
        }
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path == dependency_output) {
                const CookHashPreflight source_preflight = preflight_file_content_hash(asset.source_path);
                if (!source_preflight.can_hash) {
                    return source_preflight;
                }
                break;
            }
        }
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {
    CookHashPreflight preflight;
    if (source_hash == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size) {
    CookHashPreflight preflight;
    if (!is_valid_fnv1a64_input(data, size)) {
        preflight.reason = CookHashRejectReason::NullData;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    (void)upstream_hash;
    return preflight_cook_cache_key(source_hash, upstream_hash);
}

CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::ZeroContentHash;
        return preflight;
    }
    if (entry.source_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        return preflight;
    }
    if (entry.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

u64 hash_manifest_entry(const CookManifestEntry& entry) {
    if (entry.source_path.empty() || entry.output_path.empty()) {
        return 0;
    }

    const u64 file_hash = hash_file_content(entry.source_path);
    if (file_hash == 0) {
        return 0;
    }

    u64 hash = hash_u64(static_cast<u64>(entry.kind));
    hash = fnv1a64_combine(hash, hash_string(entry.source_path));
    hash = fnv1a64_combine(hash, hash_string(entry.output_path));
    hash = fnv1a64_combine(hash, file_hash);
    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        }
        hash = fnv1a64_combine(hash, hash_string(dependency));
        hash = fnv1a64_combine(hash, hash_file_content(dependency));
    }
    return hash;
}

} // namespace fuse::project

// --- deepen additive from b79-cooker-hash-guards-111f ---
CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookCacheKeyPreflight preflight;
CookImportHashPreflight preflight_import_paths(const std::string& input_path, const std::string& output_path) {
    CookImportHashPreflight preflight;
CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc) {
CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc) {

// --- deepen additive from deepen-b79-cooker-hash-guards-7fd3 ---
    CookCacheKeyPreflight preflight{};
        preflight.reason = CookCacheKeyRejectReason::ZeroSourceHash;
        preflight.reason = CookCacheKeyRejectReason::UncacheableFold;
    preflight.reason = CookCacheKeyRejectReason::None;
CookFileHashPreflight preflight_file_content_hash(const std::string& path) {
    CookFileHashPreflight preflight{};
        preflight.reason = CookFileHashRejectReason::EmptyPath;
        preflight.reason = CookFileHashRejectReason::UnreadableSource;
    preflight.reason = CookFileHashRejectReason::None;

// --- deepen additive from b79-hash-preflight-probes-fd33 ---
bool set_preflight_reason(CookHashPreflightRejectReason* reason, CookHashPreflightRejectReason value) {
    return value == CookHashPreflightRejectReason::None;
const char* cookHashPreflightRejectReasonLabel(CookHashPreflightRejectReason reason) {
    case CookHashPreflightRejectReason::None:
    case CookHashPreflightRejectReason::EmptyPath:
    case CookHashPreflightRejectReason::MissingFile:
    case CookHashPreflightRejectReason::EmptyInputOrOutput:
    case CookHashPreflightRejectReason::ZeroSourceHash:
bool preflight_hash_file_content(const std::string& path, CookHashPreflightRejectReason* reason) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyPath);
        return set_preflight_reason(reason, CookHashPreflightRejectReason::MissingFile);
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
bool preflight_hash_mesh_import(const MeshImportDesc& desc, CookHashPreflightRejectReason* reason) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyInputOrOutput);
bool preflight_hash_texture_import(const TextureImportDesc& desc, CookHashPreflightRejectReason* reason) {
bool preflight_hash_audio_import(const AudioImportDesc& desc, CookHashPreflightRejectReason* reason) {
bool preflight_hash_manifest_entry(const CookManifestEntry& entry, CookHashPreflightRejectReason* reason) {
bool preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash, CookHashPreflightRejectReason* reason) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::ZeroSourceHash);

// --- deepen additive from deepen-b79-cooker-hash-preflight-27fe ---
CookContentHashPreflight preflight_file_content_hash(const std::string& path) {
    CookContentHashPreflight preflight;
CookImportHashPreflight preflight_manifest_entry(const CookManifestEntry& entry) {

// --- deepen additive from deepen-b79-cooker-hash-7359 ---
const char* cookCacheKeyRejectReasonLabel(CookCacheKeyRejectReason reason) {
    case CookCacheKeyRejectReason::None:
    case CookCacheKeyRejectReason::ZeroSource:
    case CookCacheKeyRejectReason::ZeroFold:
bool preflight_cook_cache_key(u64 source_hash, u64 upstream_hash, CookCacheKeyRejectReason* reason) {
            *reason = CookCacheKeyRejectReason::ZeroSource;
            *reason = CookCacheKeyRejectReason::ZeroFold;
        *reason = CookCacheKeyRejectReason::None;
    case CookHashRejectReason::Unreadable:
bool preflight_hash_file_content(const std::string& path, u64* out_hash, CookHashRejectReason* reason) {
            *reason = CookHashRejectReason::EmptyPath;
            *reason = CookHashRejectReason::Unreadable;
        *reason = CookHashRejectReason::None;
bool preflight_mesh_import_hash(const MeshImportDesc& desc, u64* out_hash, CookHashRejectReason* reason) {

// --- deepen additive from deepen-b79-cooker-hash-preflight-0c1f ---
CookHashPreflight preflight_hash_file_content(const std::string& path) {
    CookHashPreflight result;
        result.reject = CookHashPreflightReject::EmptyPath;
        result.reject = CookHashPreflightReject::MissingFile;
    result.reject = CookHashPreflightReject::None;
CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
        return {false, CookHashPreflightReject::EmptyPath};
CookHashPreflight preflight_texture_import(const TextureImportDesc& desc) {
CookHashPreflight preflight_audio_import(const AudioImportDesc& desc) {
CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry) {
    CookHashPreflight result = preflight_hash_file_content(entry.source_path);
        const CookHashPreflight dependency_preflight = preflight_hash_file_content(dependency);

// --- deepen additive from deepen-b79-cooker-hash-preflight-633e ---
    CookCacheKeyPreflight result;

// --- deepen additive from deepen-b79-cooker-hash-preflight-e529 ---
bool should_skip_hash_file_content(const std::string& path) {
    return preflight_hash_file_content(path).should_skip();
CookCacheKeyPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
CookHashPreflight preflight_hash_mesh_import(const MeshImportDesc& desc) {
CookHashPreflight preflight_hash_texture_import(const TextureImportDesc& desc) {
CookHashPreflight preflight_hash_audio_import(const AudioImportDesc& desc) {
CookHashPreflight preflight_hash_manifest_entry(const CookManifestEntry& entry) {
    CookHashPreflight preflight = preflight_hash_file_content(entry.source_path);
    if (preflight.should_skip()) {
        if (dependency_preflight.should_skip()) {

// --- deepen additive from deepen-b79-cooker-hash-0896 ---
CookHashPreflight preflight_fnv1a64_input(const u8* data, usize size) {

// --- deepen additive from deepen-b79-cooker-hash-314f ---
    case CookHashRejectReason::NonCacheableKey:
CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    const CookHashPreflight key_preflight = preflight_cook_cache_key(source_hash, upstream_hash);
        preflight.reason = CookHashRejectReason::NonCacheableKey;

// --- deepen additive from deepen-b79-cooker-hash-reconcile-b4f0 ---
CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);

// --- deepen additive from deepen-b79-cooker-hash-guards-2061 ---
CookHashPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {

// --- deepen additive from deepen-b79-cooker-hash-preflight-6a72 ---
CookFnvInputPreflight preflight_fnv1a64_input(const u8* data, usize size) {
    CookFnvInputPreflight preflight;

// --- deepen additive from deepen-b79-cooker-hash-be66 ---
    case CookHashRejectReason::NonCacheableCombinedKey:
CookHashPreflight preflight_cacheable_cook_key(u64 source_hash, u64 upstream_hash) {
    const CookHashPreflight source_preflight = preflight_cook_cache_key(source_hash, upstream_hash);
        preflight.reason = CookHashRejectReason::NonCacheableCombinedKey;

// --- deepen additive from deepen-fuse-b79-cooker-hash-fdd2 ---
    case CookHashRejectReason::UnknownDependencyOutput:
CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry, const CookManifest& manifest) {
    const CookHashPreflight source_preflight = preflight_manifest_entry_hash(entry);
            preflight.reason = CookHashRejectReason::UnknownDependencyOutput;

// --- deepen additive from deepen-b79-cooker-hash-ff33 ---
CookHashPreflight preflight_manifest_cook_hash(const CookManifestEntry& entry, const CookManifest& manifest) {
    CookHashPreflight preflight = preflight_manifest_entry_hash(entry);

// --- deepen additive from deepen-b79-cooker-hash-3e1a ---
    const CookHashPreflight source_preflight = preflight_file_content_hash(entry.source_path);
        const CookHashPreflight dependency_preflight = preflight_file_content_hash(dependency);

// --- deepen additive from deepen-b79-cooker-hash-b4e0 ---
CookHashPreflight preflight_manifest_entry_with_upstream_hash(const CookManifestEntry& entry,

// --- deepen additive from deepen-b79-cooker-hash-reconcile-5e9c ---
    case CookHashRejectReason::UnresolvedDependency:
CookHashPreflight preflight_manifest_entry_dependencies(const CookManifestEntry& entry) {
            preflight.reason = dependency_preflight.reason == CookHashRejectReason::EmptyPath
                                   ? CookHashRejectReason::UnresolvedDependency

// --- deepen additive from deepen-b79-cooker-hash-a61f ---
            preflight.reason = CookHashRejectReason::UnresolvedDependency;

// --- deepen additive from deepen-b79-cooker-hash-83b8 ---
    case CookHashRejectReason::InvalidCacheEntry:
    const CookHashPreflight fold_preflight = preflight_combine_cook_cache_key(source_hash, upstream_hash);
CookHashPreflight preflight_manifest_entry_with_dependencies_hash(const CookManifestEntry& entry,

// --- deepen additive from deepen-b79-cooker-hash-guards-3cee ---
    case CookHashRejectReason::InvalidCacheKey:

// --- deepen additive from deepen-cooker-hash-b79-2ae1 ---
        preflight.reason = CookHashRejectReason::InvalidCacheKey;

// --- deepen additive from deepen-b79-cooker-hash-88c3 ---
    case CookHashRejectReason::MissingManifestDependency:
    const CookHashPreflight coverage = preflight_manifest_dependency_coverage(dependency_output_paths, manifest);
CookHashPreflight preflight_manifest_dependency_coverage(
            preflight.reason = CookHashRejectReason::MissingManifestDependency;

// --- deepen additive from deepen-b79-cooker-hash-guards-e0db ---
    const CookHashPreflight combined = preflight_combine_cook_cache_key(source_hash, upstream_hash);

// --- deepen additive from deepen-b79-cooker-hash-4ea3 ---
CookHashPreflight preflight_upstream_dependency_path(const std::string& dependency_output_path,
CookHashPreflight preflight_file_mtime_ns(const std::string& path) {
CookHashPreflight preflight_fnv1a64_combine(u64 left, u64 /*right*/) {

// --- deepen additive from deepen-b79-cooker-hash-0e64 ---
    case CookHashRejectReason::UnresolvedDependencyOutput:
CookHashPreflight preflight_manifest_dependency_outputs(const CookManifestEntry& entry,
            preflight.reason = CookHashRejectReason::UnresolvedDependencyOutput;
CookHashPreflight preflight_file_mtime(const std::string& path) {

// --- deepen additive from deepen-fuse-b79-cooker-hash-1101 ---
CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry) {
    const CookHashPreflight upstream_preflight =

// --- deepen additive from deepen-b79-cooker-hash-2ca8 ---
CookHashPreflight preflight_import_paths(const std::string& input_path, const std::string& output_path) {

// --- deepen additive from deepen-b79-cooker-hash-4af9 ---
    case CookHashRejectReason::UnsupportedAssetKind:
        preflight.reason = CookHashRejectReason::UnsupportedAssetKind;
CookHashPreflight preflight_manifest_entry_with_dependencies(const CookManifestEntry& entry,

// --- deepen additive from b79-cooker-hash-guards-89cd ---
        preflight.reason = CookHashRejectReason::InvalidCacheEntry;
