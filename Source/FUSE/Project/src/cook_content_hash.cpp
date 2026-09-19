#include <fuse/project/cook_content_hash.hpp>

#include <fuse/project/cook_cache.hpp>

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
}

bool preflight_hash_file_content(const std::string& path, CookHashPreflightRejectReason* reason) {
    if (path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyPath);
    }
    if (!path_exists(path)) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::MissingFile);
    }
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
}

bool preflight_hash_mesh_import(const MeshImportDesc& desc, CookHashPreflightRejectReason* reason) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyInputOrOutput);
    }
    if (!preflight_hash_file_content(desc.input_path, reason)) {
        return false;
    }
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
}

bool preflight_hash_texture_import(const TextureImportDesc& desc, CookHashPreflightRejectReason* reason) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyInputOrOutput);
    }
    if (!preflight_hash_file_content(desc.input_path, reason)) {
        return false;
    }
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
}

bool preflight_hash_audio_import(const AudioImportDesc& desc, CookHashPreflightRejectReason* reason) {
    if (desc.input_path.empty() || desc.output_path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyInputOrOutput);
    }
    if (!preflight_hash_file_content(desc.input_path, reason)) {
        return false;
    }
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
}

bool preflight_hash_manifest_entry(const CookManifestEntry& entry, CookHashPreflightRejectReason* reason) {
    if (entry.source_path.empty() || entry.output_path.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::EmptyInputOrOutput);
    }
    if (!preflight_hash_file_content(entry.source_path, reason)) {
        return false;
    }
    for (const std::string& dependency : entry.dependencies) {
        if (dependency.empty()) {
            continue;
        }
        if (!preflight_hash_file_content(dependency, reason)) {
            return false;
        }
    }
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
}

bool preflight_combine_cook_cache_key(u64 source_hash, u64 upstream_hash, CookHashPreflightRejectReason* reason) {
    if (source_hash == 0) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::ZeroSourceHash);
    }
    return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
}

bool preflight_hash_upstream_dependencies(const std::vector<std::string>& dependency_output_paths,
                                          const CookManifest& manifest,
                                          CookHashPreflightRejectReason* reason) {
    if (dependency_output_paths.empty()) {
        return set_preflight_reason(reason, CookHashPreflightRejectReason::None);
    }

    bool saw_non_empty = false;
    for (const std::string& dependency_output : dependency_output_paths) {
        if (dependency_output.empty()) {
            continue;
        saw_non_empty = true;

        bool found_manifest_entry = false;
        for (const CookManifestEntry& asset : manifest.assets) {
            if (asset.output_path != dependency_output) {
            found_manifest_entry = true;
            if (!preflight_hash_file_content(asset.source_path, reason)) {
                return false;
            break;

        if (!found_manifest_entry) {
            return set_preflight_reason(reason, CookHashPreflightRejectReason::MissingFile);

    if (!saw_non_empty) {

u64 fnv1a64_bytes(const u8* data, usize size) {
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
    case CookHashRejectReason::ZeroSourceHash:
        return "zero_source_hash";
    case CookHashRejectReason::ZeroContentHash:
        return "zero_content_hash";
    }
    return "unknown";

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
