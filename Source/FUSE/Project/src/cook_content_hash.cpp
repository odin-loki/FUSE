#include <fuse/project/cook_content_hash.hpp>

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
    }
    return preflight;
}

} // namespace

CookImportHashPreflight preflight_mesh_import(const MeshImportDesc& desc) {
    return preflight_import_paths(desc.input_path, desc.output_path);
}

CookImportHashPreflight preflight_texture_import(const TextureImportDesc& desc) {
    return preflight_import_paths(desc.input_path, desc.output_path);
}

CookImportHashPreflight preflight_audio_import(const AudioImportDesc& desc) {
    return preflight_import_paths(desc.input_path, desc.output_path);
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
