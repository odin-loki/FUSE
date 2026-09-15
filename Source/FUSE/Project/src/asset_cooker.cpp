#include <fuse/project/asset_cooker.hpp>

#include <fuse/project/cook_content_hash.hpp>
#include <fuse/log/logger.hpp>

#include <sstream>

#include <filesystem>

namespace fuse::project {

namespace {

bool path_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

CookRecord makeStubRecord(CookAssetKind kind,
                          const std::string& source_path,
                          const std::string& output_path,
                          const char* importer_note) {
    CookRecord record;
    record.kind = kind;
    record.source_path = source_path;
    record.output_path = output_path;

    if (source_path.empty() || output_path.empty()) {
        record.status = CookStatus::InvalidInput;
        record.ok = false;
        record.note = "missing input or output path";
        return record;
    }

    if (!path_exists(source_path)) {
        record.status = CookStatus::SourceMissing;
        record.ok = false;
        record.note = "source file not found (stub cook accepts dry-run planning only)";
        return record;
    }

    record.status = CookStatus::Ok;
    record.ok = true;
    record.note = importer_note;
    return record;
}

} // namespace

CookRecord AssetCooker::cook_with_cache_(CookAssetKind kind,
                                         const std::string& source_path,
                                         const std::string& output_path,
                                         u64 content_hash,
                                         const char* stub_note) {
    CookCacheEntry cached;
    if (m_cache.lookup(content_hash, &cached) == CookCacheLookup::Hit) {
        CookRecord record;
        record.kind = kind;
        record.source_path = source_path;
        record.output_path = cached.output_path;
        record.status = CookStatus::Ok;
        record.ok = true;
        record.cache_hit = true;
        record.content_hash = content_hash;
        record.note = "cache hit";
        return record;
    }

    CookRecord record = makeStubRecord(kind, source_path, output_path, stub_note);
    record.content_hash = content_hash;
    record.cache_hit = false;

    if (record.ok) {
        CookCacheEntry entry;
        entry.content_hash = content_hash;
        entry.output_path = output_path;
        entry.source_path = source_path;
        entry.kind = kind;
        m_cache.store(entry);
        record.note = std::string(stub_note) + " (cache miss)";
    }

    return record;
}

CookRecord AssetCooker::cook_mesh(const MeshImportDesc& desc) {
    std::ostringstream note;
    note << "stub mesh cook (lods=" << (desc.generate_lods ? desc.lod_count : 0u)
         << ", compress=" << (desc.compress ? "on" : "off") << ")";
    const u64 content_hash = hash_mesh_import(desc);
    return cook_with_cache_(CookAssetKind::Mesh, desc.input_path, desc.output_path, content_hash, note.str().c_str());
}

CookRecord AssetCooker::cook_texture(const TextureImportDesc& desc) {
    const char* compression = desc.is_normal_map ? "BC5" : "BC7";
    std::ostringstream note;
    note << "stub texture cook (compression=" << compression
         << ", mipmaps=" << (desc.generate_mipmaps ? "on" : "off") << ")";
    const u64 content_hash = hash_texture_import(desc);
    return cook_with_cache_(CookAssetKind::Texture, desc.input_path, desc.output_path, content_hash, note.str().c_str());
}

CookRecord AssetCooker::cook_audio(const AudioImportDesc& desc) {
    std::ostringstream note;
    note << "stub audio cook (rate=" << desc.target_sample_rate
         << ", format=" << (desc.format == AudioImportDesc::Format::OGG_VORBIS ? "ogg" : "pcm_f32") << ")";
    const u64 content_hash = hash_audio_import(desc);
    return cook_with_cache_(CookAssetKind::Audio, desc.input_path, desc.output_path, content_hash, note.str().c_str());
}

CookRecord AssetCooker::cook_entry(const CookManifestEntry& entry) {
    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_mesh(desc);
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_texture(desc);
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_audio(desc);
    }
    case CookAssetKind::Shader: {
        CookRecord record;
        record.kind = CookAssetKind::Shader;
        record.source_path = entry.source_path;
        record.output_path = entry.output_path;
        record.status = CookStatus::UnsupportedKind;
        record.ok = false;
        record.note = "shader cook deferred";
        return record;
    }
    }
    return {};
}

CookJobGraphExecuteResult AssetCooker::cook_manifest_graph(const CookManifest& manifest) {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);
    return graph.execute(*this);
}

CookBatchResult AssetCooker::cook_manifest(const CookManifest& manifest) {
    return cookBatchFromJobGraphResult(cook_manifest_graph(manifest));
}

CookBatchResult AssetCooker::cook_dirty(AssetGraph& graph, const std::string& project_dir) {
    graph.scan_for_changes();
    const std::vector<std::string> dirty = graph.dirty_assets();

    CookBatchResult result;
    result.records.reserve(dirty.size());

    for (const std::string& output_path : dirty) {
        if (const std::string* source_path = graph.source_path_for(output_path)) {
            m_cache.invalidate_source(*source_path);
        }

        CookRecord record;
        record.output_path = output_path;
        record.status = CookStatus::Ok;
        record.ok = true;
        record.note = "dirty asset reimport stub";
        result.records.push_back(std::move(record));
    }

    graph.reimport_dirty(project_dir);
    result.ok = true;
    result.summary = "reimported " + std::to_string(dirty.size()) + " dirty assets (stub)";
    return result;
}

} // namespace fuse::project
