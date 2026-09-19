#include <fuse/project/cook_content_hash.hpp>

#include <fuse/project/cook_cache.hpp>
#include <fuse/project/import_desc.hpp>

#include <filesystem>
#include <fstream>

namespace fuse::project {

namespace {

constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

bool set_preflight_reason(CookHashPreflightRejectReason* reason, CookHashPreflightRejectReason value) {
    if (reason) {
        *reason = value;
    }
    return value == CookHashPreflightRejectReason::None;
}

bool path_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

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

const char* cookHashPreflightRejectReasonLabel(CookHashPreflightRejectReason reason) {
    switch (reason) {
    case CookHashPreflightRejectReason::None:
        return "None";
    case CookHashPreflightRejectReason::EmptyPath:
        return "EmptyPath";
    case CookHashPreflightRejectReason::MissingFile:
        return "MissingFile";
    case CookHashPreflightRejectReason::EmptyInputOrOutput:
        return "EmptyInputOrOutput";
    case CookHashPreflightRejectReason::ZeroSourceHash:
        return "ZeroSourceHash";
    }
    return "Unknown";

bool preflight_hash_file_content(const std::string& path, CookHashPreflightRejectReason* reason) {
    if (path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyPath);
    if (!path_exists(path)) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::MissingFile);
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);

bool preflight_hash_mesh_import(const MeshImportDesc& desc, CookHashPreflightRejectReason* reason) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyInputOrOutput);
    if (!preflight_hash_file_content(desc.input_path, reason)) {
        return false;

bool preflight_hash_texture_import(const TextureImportDesc& desc, CookHashPreflightRejectReason* reason) {

bool preflight_hash_audio_import(const AudioImportDesc& desc, CookHashPreflightRejectReason* reason) {

bool preflight_hash_manifest_entry(const CookManifestEntry& entry, CookHashPreflightRejectReason* reason) {
    if (entry.source_path.empty() || entry.output_path.empty()) {
    if (!preflight_hash_file_content(entry.source_path, reason)) {
    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        if (!preflight_hash_file_content(dependency, reason)) {

bool preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash, CookHashPreflightRejectReason* reason) {
    if (source_hash == 0) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::ZeroSourceHash);

bool preflight_hash_upstream_dependencies(const std::vector<std::string>& dependency_output_paths,
                                          const CookManifest& manifest,
                                          CookHashPreflightRejectReason* reason) {
    if (dependency_output_paths.empty()) {

    bool saw_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (dependency_output.empty()) {
        saw_non_empty = true;

        bool found_manifest_entry = false;
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path != dependency_output) {
            found_manifest_entry = true;
            if (!preflight_hash_file_content(asset.source_path, reason)) {
            break;

        if (!found_manifest_entry) {

    if (!saw_non_empty) {

CookFnvInputPreflight preflight_fnv1a64_input(const u8* data, usize size) {
    CookFnvInputPreflight preflight;
    preflight.null_data = size > 0 && data == nullptr;
    return preflight;

u64 fnv1a64_bytes(const u8* data, usize size) {
    if (!is_valid_fnv1a64_input(data, size)) {
        return kFnvOffset;
        return 0;
    }
    if (size == 0) {
        return kFnvOffset;
    if (data == nullptr && size > 0) {
        return 0;
    if (data == nullptr) {
        return size == 0 ? kFnvOffset : 0;
    }

    u64 hash = kFnvOffset;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

bool is_readable_cook_source_path(const std::string& path) {
    return !path.empty() && !is_zero_cook_hash(hash_file_content(path));
CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size) {
    CookHashPreflight preflight;
    if (!is_valid_fnv1a64_input(data, size)) {
        preflight.reason = CookHashRejectReason::NullData;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
}

u64 fnv1a64_combine(u64 left, u64 right) {
    return fnv1a64_bytes(reinterpret_cast<const u8*>(&right), sizeof(right)) ^ (left * kFnvPrime);
}

CookHashPreflight preflight_hash_file_content(const std::string& path) {
    CookHashPreflight result;
    if (path.empty()) {
        result.reject = CookHashPreflightReject::EmptyPath;
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        result.reject = CookHashPreflightReject::MissingFile;
        return result;
    }

    result.ok = true;
    result.reject = CookHashPreflightReject::None;
    return result;
}

CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return {false, CookHashPreflightReject::EmptyPath};
    }
    return preflight_hash_file_content(desc.input_path);
}

CookHashPreflight preflight_texture_import(const TextureImportDesc& desc) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return {false, CookHashPreflightReject::EmptyPath};
    }
    return preflight_hash_file_content(desc.input_path);
}

CookHashPreflight preflight_audio_import(const AudioImportDesc& desc) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return {false, CookHashPreflightReject::EmptyPath};
    }
    return preflight_hash_file_content(desc.input_path);
}

CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry) {
    if (entry.source_path.empty() || entry.output_path.empty()) {
        return {false, CookHashPreflightReject::EmptyPath};
    }

    CookHashPreflight result = preflight_hash_file_content(entry.source_path);
    if (!result.ok) {
        return result;
    }

    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        }
        const CookHashPreflight dependency_preflight = preflight_hash_file_content(dependency);
        if (!dependency_preflight.ok) {
            return dependency_preflight;
        }
    }

    return result;
}

u64 file_mtime_ns(const std::string& path) {
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) {
        return 0;
    }
    return static_cast<u64>(ftime.time_since_epoch().count());
}

CookHashPreflight preflight_file_mtime_ns(const std::string& path) {
    CookHashPreflight preflight;
    if (path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
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

CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookCacheKeyPreflight preflight;
    preflight.zero_source_hash = source_hash == 0;
    return preflight;
}

namespace {

CookImportHashPreflight preflight_import_paths(const std::string& input_path, const std::string& output_path) {
    CookImportHashPreflight preflight;
    preflight.empty_input_path = input_path.empty();
    preflight.empty_output_path = output_path.empty();
    if (!preflight.empty_input_path && !preflight.empty_output_path) {
        preflight.unreadable_source = hash_file_content(input_path) == 0;

} // namespace

CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
    return preflight_import_paths(desc.input_path, desc.output_path);

CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc) {

CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc) {
CookHashPreflight preflight_hash_file_content(const std::string& path) {
    CookHashPreflight result;
    if (path.empty()) {
        result.empty_path = true;
        return result;

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        result.source_missing = true;

    result.ok = true;

CookHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
    if (desc.input_path.empty()) {
        result.empty_input_path = true;
    if (desc.output_path.empty()) {
        result.empty_output_path = true;

    result = preflight_hash_file_content(desc.input_path);

CookHashPreflight preflight_texture_import(const TextureImportDesc& desc) {


CookHashPreflight preflight_audio_import(const AudioImportDesc& desc) {


CookHashPreflight preflight_manifest_entry(const CookManifestEntry& entry) {
    if (entry.source_path.empty()) {
    if (entry.output_path.empty()) {

    result = preflight_hash_file_content(entry.source_path);
    if (!result.ok) {

    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        const CookHashPreflight dependency_preflight = preflight_hash_file_content(dependency);
        if (!dependency_preflight.ok) {
            return dependency_preflight;

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

CookCacheKeyPreflight preflight_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookCacheKeyPreflight preflight{};
    if (source_hash == 0) {
        preflight.reason = CookCacheKeyRejectReason::ZeroSourceHash;
        return preflight;
    }

    preflight.combined_key = combine_cook_cache_key(source_hash, upstream_hash);
    if (preflight.combined_key == 0) {
        preflight.reason = CookCacheKeyRejectReason::UncacheableFold;

    preflight.valid = true;
    preflight.reason = CookCacheKeyRejectReason::None;

CookFileHashPreflight preflight_file_content_hash(const std::string& path) {
    CookFileHashPreflight preflight{};
    if (path.empty()) {
        preflight.reason = CookFileHashRejectReason::EmptyPath;

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        preflight.reason = CookFileHashRejectReason::UnreadableSource;

    std::ifstream file(path, std::ios::binary);
    if (!file) {

    preflight.reason = CookFileHashRejectReason::None;
const char* cookCacheKeyRejectReasonLabel(CookCacheKeyRejectReason reason) {
    switch (reason) {
    case CookCacheKeyRejectReason::None:
        return "none";
    case CookCacheKeyRejectReason::ZeroSource:
        return "zero_source";
    case CookCacheKeyRejectReason::ZeroFold:
        return "zero_fold";
    return "unknown";

bool preflight_cook_cache_key(u64 source_hash, u64 upstream_hash, CookCacheKeyRejectReason* reason) {
        if (reason) {
            *reason = CookCacheKeyRejectReason::ZeroSource;
        return false;

    const u64 folded = combine_cook_cache_key(source_hash, upstream_hash);
    if (folded == 0) {
            *reason = CookCacheKeyRejectReason::ZeroFold;

        *reason = CookCacheKeyRejectReason::None;
    return true;

const char* cookHashRejectReasonLabel(CookHashRejectReason reason) {
    case CookHashRejectReason::None:
    case CookHashRejectReason::EmptyPath:
        return "empty_path";
    case CookHashRejectReason::Unreadable:
        return "unreadable";

bool preflight_hash_file_content(const std::string& path, u64* out_hash, CookHashRejectReason* reason) {
            *reason = CookHashRejectReason::EmptyPath;

    const u64 hash = hash_file_content(path);
    if (hash == 0) {
            *reason = CookHashRejectReason::Unreadable;

    if (out_hash) {
        *out_hash = hash;
        *reason = CookHashRejectReason::None;

bool preflight_mesh_import_hash(const MeshImportDesc& desc, u64* out_hash, CookHashRejectReason* reason) {
    if (desc.input_path.empty() || desc.output_path.empty()) {

    const u64 hash = hash_mesh_import(desc);

    CookCacheKeyPreflight result;
    result.zero_source = source_hash == 0;
    const u64 combined = combine_cook_cache_key(source_hash, upstream_hash);
    result.zero_combined = combined == 0;
    result.cacheable = combined != 0;
    return result;
CookHashPreflight preflight_hash_file_content(const std::string& path) {
    CookHashPreflight preflight;

    preflight.empty_path = false;

        preflight.missing_file = true;

        preflight.unreadable = true;

bool should_skip_hash_file_content(const std::string& path) {
    return preflight_hash_file_content(path).should_skip();

CookCacheKeyPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    CookCacheKeyPreflight preflight;
    preflight.zero_source_hash = source_hash == 0;
    preflight.zero_upstream_hash = upstream_hash == 0;

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();

CookHashPreflight preflight_hash_mesh_import(const MeshImportDesc& desc) {
        preflight.empty_path = true;
    return preflight_hash_file_content(desc.input_path);

CookHashPreflight preflight_hash_texture_import(const TextureImportDesc& desc) {

CookHashPreflight preflight_hash_audio_import(const AudioImportDesc& desc) {

CookHashPreflight preflight_hash_manifest_entry(const CookManifestEntry& entry) {
    if (entry.source_path.empty() || entry.output_path.empty()) {

    CookHashPreflight preflight = preflight_hash_file_content(entry.source_path);
    if (preflight.should_skip()) {

    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        const CookHashPreflight dependency_preflight = preflight_hash_file_content(dependency);
        if (dependency_preflight.should_skip()) {
            return dependency_preflight;
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
    case CookHashRejectReason::UnknownDependencyOutput:
        return "unknown_dependency_output";
    case CookHashRejectReason::UnresolvedDependency:
        return "unresolved_dependency";
    case CookHashRejectReason::MissingManifestDependency:
        return "missing_manifest_dependency";
    case CookHashRejectReason::ZeroSourceHash:
        return "zero_source_hash";
    case CookHashRejectReason::ZeroContentHash:
        return "zero_content_hash";
    case CookHashRejectReason::NonCacheableKey:
        return "non_cacheable_key";
    case CookHashRejectReason::UnresolvedDependency:
        return "unresolved_dependency";
    case CookHashRejectReason::NonCacheableCombinedKey:
        return "non_cacheable_combined_key";
    case CookHashRejectReason::InvalidCacheEntry:
        return "invalid_cache_entry";
    case CookHashRejectReason::InvalidCacheKey:
        return "invalid_cache_key";
    case CookHashRejectReason::UnresolvedDependencyOutput:
        return "unresolved_dependency_output";
    case CookHashRejectReason::UnsupportedAssetKind:
        return "unsupported_asset_kind";
    case CookHashRejectReason::UnknownDependency:
        return "unknown_dependency";
    case CookHashRejectReason::UnsupportedKind:
        return "unsupported_kind";
    }
    return "unknown";

CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size) {
    CookHashPreflight preflight;
    if (!is_valid_fnv1a64_input(data, size)) {
        preflight.reason = CookHashRejectReason::NullData;
    if (path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
        return preflight;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        preflight.reason = CookHashRejectReason::SourceUnreadable;

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;

CookHashPreflight preflight_import_paths(const std::string& input_path, const std::string& output_path) {
    CookHashPreflight preflight;
    if (input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
    if (desc.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
    return preflight_file_content_hash(desc.input_path);

CookHashPreflight preflight_texture_import_hash(const TextureImportDesc& desc) {

CookHashPreflight preflight_audio_import_hash(const AudioImportDesc& desc) {
        return preflight;
    }
    if (output_path.empty()) {
    return preflight_file_content_hash(input_path);

CookHashPreflight preflight_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_import_paths(desc.input_path, desc.output_path);



CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry) {
    if (entry.source_path.empty()) {
    if (entry.output_path.empty()) {
    return preflight_file_content_hash(entry.source_path);

CookHashPreflight preflight_manifest_entry_dependencies(const CookManifestEntry& entry) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    }

    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        }
        const CookHashPreflight dependency_preflight = preflight_file_content_hash(dependency);
        const CookHashPreflight dependency_preflight =
            preflight_upstream_dependency_path(dependency_output, manifest);
        if (!dependency_preflight.can_hash) {
            return dependency_preflight;
        }
    }

    CookHashPreflight preflight;
    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_import_paths(const std::string& input_path, const std::string& output_path) {
    CookHashPreflight preflight;
    if (input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        return preflight;
    }
    if (output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
    return preflight_file_content_hash(desc.input_path);

CookHashPreflight preflight_texture_import_hash(const TextureImportDesc& desc) {

CookHashPreflight preflight_audio_import_hash(const AudioImportDesc& desc) {
        return preflight;
    }
    return preflight_file_content_hash(input_path);

CookHashPreflight preflight_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_import_paths(desc.input_path, desc.output_path);



CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry) {
    if (entry.source_path.empty()) {
    if (entry.output_path.empty()) {
    return preflight_file_content_hash(entry.source_path);

CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                         const CookManifest& manifest) {
    CookHashPreflight preflight = preflight_manifest_entry_hash(entry);
    if (!preflight.can_hash) {

    bool has_non_empty_dependency = false;
    for (const std::string& dependency_output : entry.dependencies) {
        if (!dependency_output.empty()) {
            has_non_empty_dependency = true;
            break;
    if (!has_non_empty_dependency) {

    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);

CookHashPreflight preflight_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                                     const CookManifest& manifest) {
    bool has_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (!dependency_output.empty()) {
            has_non_empty = true;
            break;
    if (!has_non_empty) {
        preflight.reason = CookHashRejectReason::EmptyDependencyList;

    const CookHashPreflight coverage = preflight_manifest_dependency_coverage(dependency_output_paths, manifest);
    if (!coverage.can_hash) {
        return coverage;

        if (dependency_output.empty()) {
            continue;
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path == dependency_output) {
                const CookHashPreflight source_preflight = preflight_file_content_hash(asset.source_path);
                if (!source_preflight.can_hash) {
                    return source_preflight;

CookHashPreflight preflight_upstream_dependency_path(const std::string& dependency_output_path,
CookHashPreflight preflight_manifest_dependency_outputs(const CookManifestEntry& entry,
    CookHashPreflight preflight;
    for (const std::string& dependency_output : entry.dependencies) {
        }
        const CookHashPreflight dependency_preflight =
            preflight_upstream_dependency_path(dependency_output, manifest);
        if (!dependency_preflight.can_hash) {
            return dependency_preflight;
        }

        bool resolved = false;
            if (asset.output_path != dependency_output) {
            resolved = true;
        if (!resolved) {
            preflight.reason = CookHashRejectReason::UnresolvedDependencyOutput;
            return preflight;


    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;

CookHashPreflight preflight_file_mtime(const std::string& path) {
    if (path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;







CookHashPreflight preflight_upstream_dependency_path(const std::string& dependency_output_path,
                                                     const CookManifest& manifest) {
    CookHashPreflight preflight;
    if (dependency_output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
        return preflight;
    }

    for (const CookManifestEntry& asset : manifest.assets) {
        if (asset.output_path == dependency_output_path) {
            return preflight_file_content_hash(asset.source_path);
        }
    }

    preflight.reason = CookHashRejectReason::UnresolvedDependency;
    return preflight;
}

CookHashPreflight preflight_file_mtime_ns(const std::string& path) {
    CookHashPreflight preflight;
    if (path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_fnv1a64_combine(u64 left, u64 /*right*/) {
    CookHashPreflight preflight;
    if (left == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    }
    if (desc.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }
    return preflight_file_content_hash(desc.input_path);

CookHashPreflight preflight_texture_import_hash(const TextureImportDesc& desc) {
    CookHashPreflight preflight;
    if (desc.input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;

CookHashPreflight preflight_audio_import_hash(const AudioImportDesc& desc) {

CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry) {
    if (entry.kind == CookAssetKind::Shader) {
        return preflight_shader_manifest_hash(entry);

    }

    CookHashPreflight preflight;
    if (entry.source_path.empty()) {
    if (entry.output_path.empty()) {
    return preflight_file_content_hash(entry.source_path);

CookHashPreflight preflight_shader_manifest_hash(const CookManifestEntry& entry) {
    if (entry.kind != CookAssetKind::Shader) {
        preflight.can_hash = true;
        preflight.reason = CookHashRejectReason::None;
    (void)entry;
    preflight.reason = CookHashRejectReason::UnsupportedKind;

CookHashPreflight preflight_shader_manifest_hash(const CookManifestEntry& entry) {
    CookHashPreflight preflight;
    if (entry.kind != CookAssetKind::Shader) {
        preflight.can_hash = true;
        preflight.reason = CookHashRejectReason::None;
        return preflight;
    }
    (void)entry;
    preflight.reason = CookHashRejectReason::UnsupportedKind;

    const CookHashPreflight source_preflight = preflight_file_content_hash(entry.source_path);
    if (!source_preflight.can_hash) {
        return source_preflight;
    }

    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        const CookHashPreflight dependency_preflight = preflight_file_content_hash(dependency);
        if (!dependency_preflight.can_hash) {
            return dependency_preflight;
        }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                                     const CookManifest& manifest) {
    bool has_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (!dependency_output.empty()) {
            has_non_empty = true;
            break;
    if (!has_non_empty) {
        preflight.reason = CookHashRejectReason::EmptyDependencyList;

        if (dependency_output.empty()) {
            continue;
        }
        bool found = false;
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path == dependency_output) {
                found = true;
                const CookHashPreflight source_preflight = preflight_file_content_hash(asset.source_path);
                if (!source_preflight.can_hash) {
                    return source_preflight;
        if (!found) {
            preflight.reason = CookHashRejectReason::UnknownDependency;
                }
                break;
            return preflight;

            if (asset.output_path != dependency_output) {
                continue;


            preflight.reason = CookHashRejectReason::UnknownDependencyOutput;




    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {
    CookHashPreflight preflight;
    if (dependency_output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
        return preflight;
    }

        if (asset.output_path == dependency_output_path) {
            return preflight_file_content_hash(asset.source_path);

    preflight.reason = CookHashRejectReason::UnresolvedDependency;

CookHashPreflight preflight_file_mtime_ns(const std::string& path) {
    if (path.empty()) {

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;

CookHashPreflight preflight_fnv1a64_combine(u64 left, u64 /*right*/) {
    if (left == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;


CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {
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

CookHashPreflight preflight_file_content_hash(const std::string& path) {
    CookHashPreflight preflight;
    if (path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyPath;
CookContentHashPreflight preflight_file_content_hash(const std::string& path) {
    CookContentHashPreflight preflight;
        preflight.empty_path = true;
        return preflight;

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        preflight.missing_file = true;
        return preflight;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        preflight.reason = CookHashRejectReason::SourceUnreadable;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;

CookHashPreflight preflight_mesh_import_hash(const MeshImportDesc& desc) {
    CookHashPreflight preflight;
    if (desc.input_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
    if (desc.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
    return preflight_file_content_hash(desc.input_path);

CookHashPreflight preflight_texture_import_hash(const TextureImportDesc& desc) {

CookHashPreflight preflight_audio_import_hash(const AudioImportDesc& desc) {

CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry) {
    if (entry.source_path.empty()) {
    if (entry.output_path.empty()) {
    return preflight_file_content_hash(entry.source_path);

CookHashPreflight preflight_manifest_entry_hash(const CookManifestEntry& entry, const CookManifest& manifest) {
    const CookHashPreflight source_preflight = preflight_manifest_entry_hash(entry);
    if (!source_preflight.can_hash) {
        return source_preflight;
    }

    if (entry.dependencies.empty()) {
        return source_preflight;
    }

    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);
}

CookHashPreflight preflight_manifest_cook_hash(const CookManifestEntry& entry, const CookManifest& manifest) {
    CookHashPreflight preflight = preflight_manifest_entry_hash(entry);
    if (!preflight.can_hash) {
        return preflight;
    }

    if (entry.dependencies.empty()) {
        return preflight;
    }

    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);
    const CookHashPreflight source_preflight = preflight_file_content_hash(entry.source_path);
    if (!source_preflight.can_hash) {
        return source_preflight;

    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        const CookHashPreflight dependency_preflight = preflight_file_content_hash(dependency);
        if (!dependency_preflight.can_hash) {
            return dependency_preflight;

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
}

CookHashPreflight preflight_manifest_entry_with_upstream_hash(const CookManifestEntry& entry,
                                                              const CookManifest& manifest) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    }
    if (entry.dependencies.empty()) {
        return entry_preflight;
    }
    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);
}

CookHashPreflight preflight_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                                     const CookManifest& manifest) {
    bool has_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (!dependency_output.empty()) {
            has_non_empty = true;
            break;
    if (!has_non_empty) {
        preflight.reason = CookHashRejectReason::EmptyDependencyList;

        if (dependency_output.empty()) {
            continue;
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path == dependency_output) {
                const CookHashPreflight source_preflight = preflight_file_content_hash(asset.source_path);
                if (!source_preflight.can_hash) {
                    return source_preflight;
        }

        bool resolved = false;
            if (asset.output_path != dependency_output) {
                continue;

            resolved = true;
            break;

        if (!resolved) {
            preflight.reason = CookHashRejectReason::UnresolvedDependency;
            return preflight;
        }

        bool found = false;
            if (asset.output_path != dependency_output) {
                continue;
            found = true;
            break;
        if (!found) {
            preflight.reason = CookHashRejectReason::UnknownDependencyOutput;
            return preflight;


CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {
    if (source_hash == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;


CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size) {
    if (!is_valid_fnv1a64_input(data, size)) {
        preflight.reason = CookHashRejectReason::NullData;


CookHashPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    (void)upstream_hash;
    return preflight_cook_cache_key(source_hash, upstream_hash);

CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::ZeroContentHash;

        preflight.unreadable_file = true;

CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
    CookImportHashPreflight preflight;
        preflight.empty_input_path = true;
        preflight.empty_output_path = true;
    if (preflight.can_hash()) {
        preflight.source_unhashable = !preflight_file_content_hash(desc.input_path).can_hash();

CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc) {

CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc) {

CookImportHashPreflight preflight_manifest_entry(const CookManifestEntry& entry) {
        preflight.source_unhashable = !preflight_file_content_hash(entry.source_path).can_hash();
    return preflight;
}

CookHashPreflight preflight_fnv1a64_input(const u8* data, usize size) {
CookHashPreflight preflight_fnv1a64_bytes(const u8* data, usize size) {
    CookHashPreflight preflight;
    if (!is_valid_fnv1a64_input(data, size)) {
        preflight.reason = CookHashRejectReason::NullData;
CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    }
    if (entry.source_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
    if (entry.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
    if (!is_valid_cook_cache_entry(entry)) {
        preflight.reason = CookHashRejectReason::InvalidCacheEntry;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;

CookHashPreflight preflight_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {
CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    const CookHashPreflight fold_preflight = preflight_combine_cook_cache_key(source_hash, upstream_hash);
    if (!fold_preflight.can_hash) {
        return fold_preflight;

    if (combine_cook_cache_key(source_hash, upstream_hash) == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::InvalidCacheKey;
    if (entry.source_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
    if (entry.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
CookHashPreflight preflight_manifest_dependency_coverage(
    const std::vector<std::string>& dependency_output_paths, const CookManifest& manifest) {
    bool has_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (dependency_output.empty()) {
            continue;
        has_non_empty = true;

        bool found = false;
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path == dependency_output) {
                found = true;
                break;
        if (!found) {
            preflight.reason = CookHashRejectReason::MissingManifestDependency;

    if (!has_non_empty) {
        preflight.reason = CookHashRejectReason::EmptyDependencyList;
    const CookHashPreflight combined = preflight_combine_cook_cache_key(source_hash, upstream_hash);
    if (!combined.can_hash) {
        return combined;

CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry) {
        preflight.reason = CookHashRejectReason::ZeroContentHash;

    const CookHashPreflight source_preflight = preflight_cook_cache_key(source_hash, upstream_hash);
    if (!source_preflight.can_hash) {
        return source_preflight;



    return preflight_combine_cook_cache_key(source_hash, 0);

CookHashPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    if (source_hash == 0) {





CookHashPreflight preflight_combine_cook_cache_key(u64 source_hash, u64 /*upstream_hash*/) {




    const CookHashPreflight key_preflight = preflight_cook_cache_key(source_hash, upstream_hash);
    if (!key_preflight.can_hash) {
        return key_preflight;
CookHashPreflight preflight_cacheable_cook_key(u64 source_hash, u64 upstream_hash) {

        preflight.reason = CookHashRejectReason::NonCacheableKey;




CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                       const CookManifest& manifest) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    if (entry.kind == CookAssetKind::Shader) {
        preflight = preflight_shader_entry_hash(entry);
    } else {
        preflight = preflight_manifest_entry_hash(entry);
    if (!preflight.can_hash) {

    bool has_non_empty_dependency = false;
    for (const std::string& dependency : entry.dependencies) {
        if (!dependency.empty()) {
            has_non_empty_dependency = true;
    if (!has_non_empty_dependency) {

    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);
        preflight.reason = CookHashRejectReason::NonCacheableCombinedKey;







    if (!is_valid_cook_cache_path(entry.source_path)) {
    if (!is_valid_cook_cache_path(entry.output_path)) {









CookHashPreflight preflight_manifest_entry_dependencies(const CookManifestEntry& entry) {

        if (dependency.empty()) {

        const CookHashPreflight dependency_preflight = preflight_file_content_hash(dependency);
        if (!dependency_preflight.can_hash) {
            preflight.reason = dependency_preflight.reason == CookHashRejectReason::EmptyPath
                                   ? CookHashRejectReason::UnresolvedDependency
                                   : dependency_preflight.reason;




            return dependency_preflight;



    (void)upstream_hash;
    return preflight_cook_cache_key(source_hash, upstream_hash);

CookHashPreflight preflight_manifest_entry_with_dependencies_hash(const CookManifestEntry& entry,

    if (entry.dependencies.empty()) {


    const CookHashPreflight upstream_preflight =
        preflight_upstream_dependencies_hash(entry.dependencies, manifest);
    if (!upstream_preflight.can_hash) {
        return upstream_preflight;

        preflight.reason = CookHashRejectReason::UnsupportedAssetKind;
    return preflight_file_content_hash(entry.source_path);

CookHashPreflight preflight_manifest_entry_with_dependencies(const CookManifestEntry& entry,


    return preflight;
}

CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                         const CookManifest& manifest) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    }

    if (entry.dependencies.empty()) {
        return entry_preflight;
    }

    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);
}

CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_entry(entry)) {
        if (!is_valid_cook_cache_key(entry.content_hash)) {
            preflight.reason = CookHashRejectReason::ZeroSourceHash;
        } else if (!is_valid_cook_cache_path(entry.source_path)) {
            preflight.reason = CookHashRejectReason::EmptyInputPath;
        } else if (!is_valid_cook_cache_path(entry.output_path)) {
            preflight.reason = CookHashRejectReason::EmptyOutputPath;
        } else {
            preflight.reason = CookHashRejectReason::InvalidCacheEntry;
        }
        return preflight;
    }

    if (entry.kind == CookAssetKind::Shader) {
        preflight.can_hash = true;
        preflight.reason = CookHashRejectReason::None;
        return preflight;
    }

    return preflight_file_content_hash(entry.source_path);
}

CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    const CookHashPreflight fold_preflight = preflight_combine_cook_cache_key(source_hash, upstream_hash);
    if (!fold_preflight.can_hash) {
        return fold_preflight;
    }

    CookHashPreflight preflight;
    if (!is_cacheable_cook_cache_key(source_hash, upstream_hash)) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry) {
    CookHashPreflight preflight;
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

CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                         const CookManifest& manifest) {
    CookHashPreflight preflight;
    if (entry.kind == CookAssetKind::Shader) {
        preflight = preflight_shader_entry_hash(entry);
    } else {
        preflight = preflight_manifest_entry_hash(entry);
    }
    if (!preflight.can_hash) {
        return preflight;
    }

    bool has_non_empty_dependency = false;
    for (const std::string& dependency : entry.dependencies) {
        if (!dependency.empty()) {
            has_non_empty_dependency = true;
            break;
        }
    }
    if (!has_non_empty_dependency) {
        return preflight;
    }

    const CookHashPreflight upstream_preflight =
        preflight_upstream_dependencies_hash(entry.dependencies, manifest);
    if (!upstream_preflight.can_hash) {
        return upstream_preflight;
    }

    return preflight;
}

CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry) {
    CookHashPreflight preflight;
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

CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                         const CookManifest& manifest) {
    CookHashPreflight preflight;
    if (entry.kind == CookAssetKind::Shader) {
        preflight = preflight_shader_entry_hash(entry);
    } else {
        preflight = preflight_manifest_entry_hash(entry);
    }
    if (!preflight.can_hash) {
        return preflight;
    }

    bool has_non_empty_dependency = false;
    for (const std::string& dependency : entry.dependencies) {
        if (!dependency.empty()) {
            has_non_empty_dependency = true;
            break;
        }
    }
    if (!has_non_empty_dependency) {
        return preflight;
    }

    const CookHashPreflight upstream_preflight =
        preflight_upstream_dependencies_hash(entry.dependencies, manifest);
    if (!upstream_preflight.can_hash) {
        return upstream_preflight;
    }

    return preflight;
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

CookHashPreflight preflight_manifest_cook_key(const CookManifestEntry& entry, const CookManifest& manifest) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    }

    bool has_non_empty_dependency = false;
    for (const std::string& dependency : entry.dependencies) {
        if (!dependency.empty()) {
            has_non_empty_dependency = true;
            break;
        }
    }
    if (!entry.dependencies.empty() && !has_non_empty_dependency) {
        CookHashPreflight preflight;
        preflight.reason = CookHashRejectReason::EmptyDependencyList;
        return preflight;
    }

    u64 upstream_hash = 0;
    if (has_non_empty_dependency) {
        const CookHashPreflight upstream_preflight =
            preflight_upstream_dependencies_hash(entry.dependencies, manifest);
        if (!upstream_preflight.can_hash) {
            return upstream_preflight;
        }
        upstream_hash = hash_upstream_dependencies(entry.dependencies, manifest);
    }

    u64 source_hash = 0;
    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        source_hash = hash_mesh_import(desc);
        break;
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        source_hash = hash_texture_import(desc);
        break;
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        source_hash = hash_audio_import(desc);
        break;
    }
    case CookAssetKind::Shader:
        return entry_preflight;
    }

    return preflight_combine_cook_cache_key(source_hash, upstream_hash);
}

namespace {

CookHashPreflight preflight_import_cook_key_(const CookHashPreflight& import_preflight, u64 source_hash,
                                             u64 upstream_hash) {
    if (!import_preflight.can_hash) {
        return import_preflight;
    }
    return preflight_combine_cook_cache_key(source_hash, upstream_hash);
}

} // namespace

CookHashPreflight preflight_mesh_import_cook_key(const MeshImportDesc& desc, u64 upstream_hash) {
    const CookHashPreflight import_preflight = preflight_mesh_import_hash(desc);
    if (!import_preflight.can_hash) {
        return import_preflight;
    }
    return preflight_import_cook_key_(import_preflight, hash_mesh_import(desc), upstream_hash);
}

CookHashPreflight preflight_texture_import_cook_key(const TextureImportDesc& desc, u64 upstream_hash) {
    const CookHashPreflight import_preflight = preflight_texture_import_hash(desc);
    if (!import_preflight.can_hash) {
        return import_preflight;
    }
    return preflight_import_cook_key_(import_preflight, hash_texture_import(desc), upstream_hash);
}

CookHashPreflight preflight_audio_import_cook_key(const AudioImportDesc& desc, u64 upstream_hash) {
    const CookHashPreflight import_preflight = preflight_audio_import_hash(desc);
    if (!import_preflight.can_hash) {
        return import_preflight;
    }
    return preflight_import_cook_key_(import_preflight, hash_audio_import(desc), upstream_hash);
}

CookHashPreflight preflight_manifest_cook_key(const CookManifestEntry& entry, const CookManifest& manifest) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    }

    bool has_non_empty_dependency = false;
    for (const std::string& dependency : entry.dependencies) {
        if (!dependency.empty()) {
            has_non_empty_dependency = true;
            break;
        }
    }
    if (!entry.dependencies.empty() && !has_non_empty_dependency) {
        CookHashPreflight preflight;
        preflight.reason = CookHashRejectReason::EmptyDependencyList;
        return preflight;
    }

    u64 upstream_hash = 0;
    if (has_non_empty_dependency) {
        const CookHashPreflight upstream_preflight =
            preflight_upstream_dependencies_hash(entry.dependencies, manifest);
        if (!upstream_preflight.can_hash) {
            return upstream_preflight;
        }
        upstream_hash = hash_upstream_dependencies(entry.dependencies, manifest);
    }

    u64 source_hash = 0;
    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        source_hash = hash_mesh_import(desc);
        break;
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        source_hash = hash_texture_import(desc);
        break;
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        source_hash = hash_audio_import(desc);
        break;
    }
    case CookAssetKind::Shader:
        return entry_preflight;
    }

    return preflight_combine_cook_cache_key(source_hash, upstream_hash);
}

CookHashPreflight preflight_mesh_import_cache_key(const MeshImportDesc& desc, u64 upstream_hash) {
    const CookHashPreflight import_preflight = preflight_mesh_import_hash(desc);
    if (!import_preflight.can_hash) {
        return import_preflight;
    }

    const u64 source_hash = hash_mesh_import(desc);
    if (source_hash == 0) {
        CookHashPreflight preflight;
        preflight.reason = CookHashRejectReason::SourceUnreadable;
        return preflight;
    }

    return preflight_combine_cook_cache_key(source_hash, upstream_hash);
}

CookHashPreflight preflight_texture_import_cache_key(const TextureImportDesc& desc, u64 upstream_hash) {
    const CookHashPreflight import_preflight = preflight_texture_import_hash(desc);
    if (!import_preflight.can_hash) {
        return import_preflight;
    }

    const u64 source_hash = hash_texture_import(desc);
    if (source_hash == 0) {
        CookHashPreflight preflight;
        preflight.reason = CookHashRejectReason::SourceUnreadable;
        return preflight;
    }

    return preflight_combine_cook_cache_key(source_hash, upstream_hash);
}

CookHashPreflight preflight_audio_import_cache_key(const AudioImportDesc& desc, u64 upstream_hash) {
    const CookHashPreflight import_preflight = preflight_audio_import_hash(desc);
    if (!import_preflight.can_hash) {
        return import_preflight;
    }

    const u64 source_hash = hash_audio_import(desc);
    if (source_hash == 0) {
        CookHashPreflight preflight;
        preflight.reason = CookHashRejectReason::SourceUnreadable;
        return preflight;
    }

    return preflight_combine_cook_cache_key(source_hash, upstream_hash);
}

CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_entry(entry)) {
        if (!is_valid_cook_cache_key(entry.content_hash)) {
            preflight.reason = CookHashRejectReason::ZeroSourceHash;
            return preflight;
        }
        if (!is_valid_cook_cache_path(entry.source_path)) {
            preflight.reason = CookHashRejectReason::EmptyInputPath;
            return preflight;
        }
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
        return preflight;
    }

    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_mesh_import_hash(desc);
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_texture_import_hash(desc);
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_audio_import_hash(desc);
    }
    case CookAssetKind::Shader:
        preflight.reason = CookHashRejectReason::SourceUnreadable;
        return preflight;
    }
    return preflight;
}

bool tryPreflightMeshImportHash(const MeshImportDesc& desc, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_mesh_import_hash(desc);
    reason = preflight.reason;
    return preflight.can_hash;
}

bool tryPreflightTextureImportHash(const TextureImportDesc& desc, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_texture_import_hash(desc);
    reason = preflight.reason;
    return preflight.can_hash;
}

bool tryPreflightAudioImportHash(const AudioImportDesc& desc, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_audio_import_hash(desc);
    reason = preflight.reason;
    return preflight.can_hash;
}

bool tryPreflightManifestEntryHash(const CookManifestEntry& entry, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_manifest_entry_hash(entry);
    reason = preflight.reason;
    return preflight.can_hash;
}

bool tryPreflightCookCacheEntry(const CookCacheEntry& entry, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_cook_cache_entry(entry);
    reason = preflight.reason;
    return preflight.can_hash;
}

CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_entry(entry)) {
        preflight.reason = CookHashRejectReason::InvalidCacheEntry;
        return preflight;
    }

    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_mesh_import_hash(desc);
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_texture_import_hash(desc);
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_audio_import_hash(desc);
    }
    case CookAssetKind::Shader:
        preflight.reason = CookHashRejectReason::UnsupportedAssetKind;
        return preflight;
    }

    preflight.reason = CookHashRejectReason::UnsupportedAssetKind;
    return preflight;
}

CookHashRejectReason classifyCookHashReject(const CookHashPreflight& preflight) {
    return preflight.reason;
}

bool wouldHashFileContent(const std::string& path) {
    return preflight_file_content_hash(path).ok();
}

bool wouldHashMeshImport(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).ok();
}

bool wouldHashTextureImport(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).ok();
}

bool wouldHashAudioImport(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).ok();
}

bool wouldHashManifestEntry(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).ok();
}

bool wouldHashUpstreamDependencies(const std::vector<std::string>& dependency_output_paths,
                                   const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).ok();
}

bool wouldHashCookCacheKey(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).ok();
}

bool tryPreflightFileContentHash(const std::string& path, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_file_content_hash(path);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

bool tryPreflightMeshImportHash(const MeshImportDesc& desc, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_mesh_import_hash(desc);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

bool tryPreflightTextureImportHash(const TextureImportDesc& desc, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_texture_import_hash(desc);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

bool tryPreflightAudioImportHash(const AudioImportDesc& desc, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_audio_import_hash(desc);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

bool tryPreflightManifestEntryHash(const CookManifestEntry& entry, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_manifest_entry_hash(entry);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

bool tryPreflightUpstreamDependenciesHash(const std::vector<std::string>& dependency_output_paths,
                                          const CookManifest& manifest, CookHashRejectReason& reason) {
    const CookHashPreflight preflight =
        preflight_upstream_dependencies_hash(dependency_output_paths, manifest);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

bool tryPreflightCookCacheKey(u64 source_hash, u64 upstream_hash, CookHashRejectReason& reason) {
    const CookHashPreflight preflight = preflight_cook_cache_key(source_hash, upstream_hash);
    reason = classifyCookHashReject(preflight);
    return preflight.ok();
}

CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    const CookHashPreflight key_preflight = preflight_cook_cache_key(source_hash, upstream_hash);
    if (!key_preflight.can_hash) {
        return key_preflight;
    }

    CookHashPreflight preflight;
    if (combine_cook_cache_key(source_hash, upstream_hash) == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
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

bool should_skip_cook_cache_store(const CookCacheEntry& entry) {
    return !is_valid_cook_cache_entry(entry);
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

CookHashPreflight preflight_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    const CookHashPreflight combined = preflight_combine_cook_cache_key(source_hash, upstream_hash);
    if (!combined.can_hash) {
        return combined;
    }

    CookHashPreflight preflight;
    if (combine_cook_cache_key(source_hash, upstream_hash) == 0) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    }

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    return preflight;
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

CookHashPreflight preflight_shader_entry_hash(const CookManifestEntry& entry) {
    CookHashPreflight preflight;
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

CookHashPreflight preflight_manifest_entry_with_upstream(const CookManifestEntry& entry,
                                                         const CookManifest& manifest) {
    const CookHashPreflight entry_preflight = preflight_manifest_entry_hash(entry);
    if (!entry_preflight.can_hash) {
        return entry_preflight;
    }

    if (entry.dependencies.empty()) {
        return entry_preflight;
    }

    return preflight_upstream_dependencies_hash(entry.dependencies, manifest);
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return should_skip_cook_hash_preflight(preflight_mesh_import_hash(desc));
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return should_skip_cook_hash_preflight(preflight_texture_import_hash(desc));
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return should_skip_cook_hash_preflight(preflight_audio_import_hash(desc));
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return should_skip_cook_hash_preflight(preflight_manifest_entry_hash(entry));
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return should_skip_cook_hash_preflight(
        preflight_upstream_dependencies_hash(dependency_output_paths, manifest));
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return should_skip_cook_hash_preflight(preflight_cook_cache_key(source_hash, upstream_hash));
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                          const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_file_content_hash(const std::string& path) {
    return preflight_file_content_hash(path).should_skip();
}

bool should_skip_mesh_import_hash(const MeshImportDesc& desc) {
    return preflight_mesh_import_hash(desc).should_skip();
}

bool should_skip_texture_import_hash(const TextureImportDesc& desc) {
    return preflight_texture_import_hash(desc).should_skip();
}

bool should_skip_audio_import_hash(const AudioImportDesc& desc) {
    return preflight_audio_import_hash(desc).should_skip();
}

bool should_skip_manifest_entry_hash(const CookManifestEntry& entry) {
    return preflight_manifest_entry_hash(entry).should_skip();
}

bool should_skip_upstream_dependencies_hash(const std::vector<std::string>& dependency_output_paths,
                                            const CookManifest& manifest) {
    return preflight_upstream_dependencies_hash(dependency_output_paths, manifest).should_skip();
}

bool should_skip_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_cook_cache_key(source_hash, upstream_hash).should_skip();
}

bool should_skip_fnv1a64_bytes(const u8* data, usize size) {
    return preflight_fnv1a64_bytes(data, size).should_skip();
}

bool should_skip_combine_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return preflight_combine_cook_cache_key(source_hash, upstream_hash).should_skip();
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
