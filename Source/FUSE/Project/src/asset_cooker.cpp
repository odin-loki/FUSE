#include <fuse/project/asset_cooker.hpp>

#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/cook/texture_cook.hpp>
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

u64 hash_upstream_from_jobs(const CookJob& job, const std::vector<CookJob>& jobs) {
    u64 hash = 0;
    for (const std::string& dependency_id : job.dependency_ids) {
        for (const CookJob& dependency : jobs) {
            if (dependency.id != dependency_id) {
                continue;
            }
            hash = fnv1a64_combine(hash, fnv1a64_bytes(reinterpret_cast<const u8*>(dependency.output_path.data()),
                                                       dependency.output_path.size()));
            hash = fnv1a64_combine(hash, hash_file_content(dependency.source_path));
            break;
        }
    }
    return hash;
}

} // namespace

u64 AssetCooker::effective_cache_key_(u64 content_hash, u64 upstream_hash) const {
    return combine_cook_cache_key(content_hash, upstream_hash);
}

bool AssetCooker::lookup_live_entry_(u64 cache_key, CookCacheEntry* out_entry) {
    CookCacheEntry cached;
    if (m_cache.lookup(cache_key, &cached) != CookCacheLookup::Hit) {
        return false;
    }
    bool live = path_exists(cached.output_path);
    if (live && strict_import()) {
        // Strict cooks only reuse outputs that load as real cooked assets (not lenient stubs or
        // truncated/corrupted files).
        if (cached.kind == CookAssetKind::Mesh) {
            fuse::cook::CookedMesh mesh;
            live = fuse::cook::load_cooked_mesh(cached.output_path, mesh);
        } else if (cached.kind == CookAssetKind::Texture) {
            fuse::cook::CookedTexture texture;
            live = fuse::cook::load_cooked_texture(cached.output_path, texture);
        }
    }
    if (!live) {
        // Cooked output deleted or damaged behind the cache's back: the entry is stale, re-cook.
        (void)m_cache.invalidate(cache_key);
        return false;
    }
    if (out_entry != nullptr) {
        *out_entry = cached;
    }
    return true;
}

CookRecord AssetCooker::cook_with_cache_(CookAssetKind kind,
                                         const std::string& source_path,
                                         const std::string& output_path,
                                         u64 content_hash,
                                         u64 upstream_hash,
                                         const char* stub_note,
                                         const CookWriteFn& write) {
    const u64 cache_key = effective_cache_key_(content_hash, upstream_hash);
    const bool cacheable = is_valid_cook_cache_key(cache_key);

    CookCacheEntry cached;
    if (cacheable && lookup_live_entry_(cache_key, &cached)) {
        CookRecord record;
        record.kind = kind;
        record.source_path = source_path;
        record.output_path = cached.output_path;
        record.status = CookStatus::Ok;
        record.ok = true;
        record.cache_hit = true;
        record.content_hash = cache_key;
        record.note = "cache hit";
        return record;
    }

    CookRecord record = makeStubRecord(kind, source_path, output_path, stub_note);
    record.content_hash = cache_key;
    record.cache_hit = false;
    if (!record.ok) {
        return record;
    }

    const CookWriteOutcome written = write();
    if (!written.ok) {
        record.ok = false;
        record.status = written.status;
        record.note = written.note;
        return record;
    }

    record.note = std::string(stub_note) + (cacheable ? " (cache miss)" : "") + " (" + written.note +
                  ", bytes=" + std::to_string(written.byte_count) + ")";
    if (cacheable) {
        m_cache.invalidate_stale_content_for_source(source_path, cache_key);

        CookCacheEntry entry;
        entry.content_hash = cache_key;
        entry.upstream_hash = upstream_hash;
        entry.output_path = output_path;
        entry.source_path = source_path;
        entry.kind = kind;
        m_cache.store(entry);
    }
    return record;
}

bool AssetCooker::probe_cook_cache_hit(const CookManifestEntry& entry, const CookManifest& manifest,
                                       CookRecord* out_record) {
    const u64 upstream_hash = hash_upstream_dependencies(entry.dependencies, manifest);
    u64 content_hash = 0;
    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        content_hash = hash_mesh_import(desc);
        break;
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        content_hash = hash_texture_import(desc);
        break;
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        content_hash = hash_audio_import(desc);
        break;
    }
    case CookAssetKind::Shader: {
        ShaderImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        content_hash = hash_shader_import(desc);
        break;
    }
    }

    const u64 cache_key = effective_cache_key_(content_hash, upstream_hash);
    if (!is_valid_cook_cache_key(cache_key)) {
        return false;
    }

    CookCacheEntry cached;
    if (!lookup_live_entry_(cache_key, &cached)) {
        return false;
    }

    if (out_record != nullptr) {
        out_record->kind = entry.kind;
        out_record->source_path = entry.source_path;
        out_record->output_path = cached.output_path;
        out_record->status = CookStatus::Ok;
        out_record->ok = true;
        out_record->cache_hit = true;
        out_record->content_hash = cache_key;
        out_record->note = "cache hit";
    }
    return true;
}

namespace {

CookStatus status_for_failure(fuse::cook::CookFailure failure) {
    switch (failure) {
    case fuse::cook::CookFailure::MalformedSource:
        return CookStatus::MalformedSource;
    case fuse::cook::CookFailure::InvalidGeometry:
        return CookStatus::InvalidGeometry;
    case fuse::cook::CookFailure::CorruptImage:
        return CookStatus::CorruptImage;
    case fuse::cook::CookFailure::InvalidImageDimensions:
        return CookStatus::InvalidImageDimensions;
    case fuse::cook::CookFailure::ImporterUnavailable:
        return CookStatus::ImporterUnavailable;
    case fuse::cook::CookFailure::WriteFailed:
        return CookStatus::OutputError;
    case fuse::cook::CookFailure::None:
    case fuse::cook::CookFailure::InvalidArgument:
        break;
    }
    return CookStatus::InvalidInput;
}

CookWriteOutcome to_outcome(const fuse::cook::CookStubWriteResult& written) {
    return {written.ok, written.byteCount, written.note,
            written.ok ? CookStatus::Ok : status_for_failure(written.failure)};
}

} // namespace

const char* importValidationName(ImportValidation mode) {
    return mode == ImportValidation::Strict ? "strict" : "lenient";
}

CookRecord AssetCooker::cook_mesh(const MeshImportDesc& desc) {
    std::ostringstream note;
    note << (strict_import() ? "mesh cook" : "stub mesh cook") << " (lods=" << (desc.generate_lods ? desc.lod_count : 0u)
         << ", compress=" << (desc.compress ? "on" : "off") << ")";
    const u64 content_hash = hash_mesh_import(desc);
    const bool strict = strict_import();
    return cook_with_cache_(CookAssetKind::Mesh, desc.input_path, desc.output_path, content_hash, 0,
                            note.str().c_str(), [&desc, strict]() {
                                if (strict) {
                                    fuse::cook::MeshCookOptions options;
                                    options.generate_normals = desc.generate_normals;
                                    options.import_tangents = desc.fmsh_v2_streams && desc.generate_tangents;
                                    options.import_uv1 = desc.fmsh_v2_streams;
                                    options.import_colors = desc.fmsh_v2_streams;
                                    options.import_skin = desc.fmsh_v2_streams;
                                    options.import_material_names = desc.fmsh_v2_streams;
                                    options.encoding.quantize_positions = desc.quantize_vertices;
                                    options.encoding.quantize_normals = desc.quantize_vertices;
                                    return to_outcome(
                                        fuse::cook::cook_mesh_file(desc.input_path, desc.output_path, options));
                                }
                                return to_outcome(fuse::cook::write_mesh_stub(
                                    desc.input_path, desc.output_path, desc.generate_lods ? desc.lod_count : 0u,
                                    desc.compress));
                            });
}

CookRecord AssetCooker::cook_texture(const TextureImportDesc& desc) {
    // Asset plan §1.3: normal maps → BC5 (linear), HDR → BC6H, otherwise the requested BC format.
    const char* compression = "BC7";
    if (desc.is_normal_map) {
        compression = "BC5";
    } else if (desc.is_hdr) {
        compression = "BC6H";
    } else {
        switch (desc.compression) {
        case TextureImportDesc::Compression::BC1:
            compression = "BC1";
            break;
        case TextureImportDesc::Compression::BC4:
            compression = "BC4";
            break;
        case TextureImportDesc::Compression::BC5:
            compression = "BC5";
            break;
        case TextureImportDesc::Compression::BC7:
            compression = "BC7";
            break;
        case TextureImportDesc::Compression::None:
            compression = "none";
            break;
        case TextureImportDesc::Compression::BC3:
            compression = "BC3";
            break;
        }
    }
    std::ostringstream note;
    note << (strict_import() ? "texture cook" : "stub texture cook") << " (compression=" << compression
         << ", mipmaps=" << (desc.generate_mipmaps ? "on" : "off") << ")";
    const u64 content_hash = hash_texture_import(desc);
    const bool strict = strict_import();
    return cook_with_cache_(CookAssetKind::Texture, desc.input_path, desc.output_path, content_hash, 0,
                            note.str().c_str(), [&desc, compression, strict]() {
                                if (strict) {
                                    fuse::cook::TextureCookOptions options;
                                    options.mipmaps = desc.generate_mipmaps;
                                    options.normal_map = desc.is_normal_map;
                                    options.srgb = desc.color_space == TextureImportDesc::ColorSpace::sRGB;
                                    if (!fuse::cook::parse_bc_format(compression, options.format)) {
                                        fuse::cook::CookStubWriteResult refused;
                                        refused.failure = fuse::cook::CookFailure::InvalidArgument;
                                        refused.note = std::string("texture compression ") + compression +
                                                       " is not a cook format (use BC1/BC4/BC5/BC6H/BC7)";
                                        return to_outcome(refused);
                                    }
                                    return to_outcome(
                                        fuse::cook::cook_texture_file(desc.input_path, desc.output_path, options));
                                }
                                return to_outcome(fuse::cook::write_texture_stub(
                                    desc.input_path, desc.output_path, compression, desc.generate_mipmaps));
                            });
}

CookRecord AssetCooker::cook_audio(const AudioImportDesc& desc) {
    const char* format = desc.format == AudioImportDesc::Format::OGG_VORBIS ? "ogg" : "pcm_f32";
    std::ostringstream note;
    note << "stub audio cook (rate=" << desc.target_sample_rate << ", format=" << format << ")";
    const u64 content_hash = hash_audio_import(desc);
    return cook_with_cache_(CookAssetKind::Audio, desc.input_path, desc.output_path, content_hash, 0,
                            note.str().c_str(), [&desc, format]() {
                                return to_outcome(fuse::cook::write_audio_stub(
                                    desc.input_path, desc.output_path, desc.target_sample_rate, format));
                            });
}

CookRecord AssetCooker::cook_shader(const ShaderImportDesc& desc) {
    const char* stage = desc.stage == ShaderImportDesc::Stage::Vertex
                            ? "vertex"
                            : (desc.stage == ShaderImportDesc::Stage::Compute ? "compute" : "fragment");
    std::ostringstream note;
    note << "stub shader cook (stage=" << stage << ", version=" << desc.target_version << ")";
    const u64 content_hash = hash_shader_import(desc);
    return cook_with_cache_(CookAssetKind::Shader, desc.input_path, desc.output_path, content_hash, 0,
                            note.str().c_str(), [&desc, stage]() {
                                return to_outcome(fuse::cook::write_shader_stub(desc.input_path, desc.output_path,
                                                                                stage, desc.target_version));
                            });
}

CookRecord AssetCooker::cook_entry(const CookManifestEntry& entry) {
    return cook_entry(entry, CookManifest{});
}

CookRecord AssetCooker::cook_entry(const CookManifestEntry& entry, const CookManifest& manifest) {
    const u64 upstream_hash = hash_upstream_dependencies(entry.dependencies, manifest);
    const bool strict = strict_import();

    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_with_cache_(CookAssetKind::Mesh, entry.source_path, entry.output_path, hash_mesh_import(desc),
                                upstream_hash, strict ? "mesh cook from manifest entry" : "stub mesh cook from manifest entry",
                                [&desc, strict]() {
                                    if (strict) {
                                        return to_outcome(fuse::cook::cook_mesh_file(desc.input_path, desc.output_path));
                                    }
                                    return to_outcome(
                                        fuse::cook::write_mesh_stub(desc.input_path, desc.output_path, 0u, false));
                                });
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_with_cache_(CookAssetKind::Texture, entry.source_path, entry.output_path,
                                hash_texture_import(desc), upstream_hash,
                                strict ? "texture cook from manifest entry" : "stub texture cook from manifest entry",
                                [&desc, strict]() {
                                    if (strict) {
                                        return to_outcome(fuse::cook::cook_texture_bc7_file(
                                            desc.input_path, desc.output_path, desc.generate_mipmaps));
                                    }
                                    return to_outcome(fuse::cook::write_texture_stub(
                                        desc.input_path, desc.output_path, "BC7", desc.generate_mipmaps));
                                });
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_with_cache_(CookAssetKind::Audio, entry.source_path, entry.output_path, hash_audio_import(desc),
                                upstream_hash, "stub audio cook from manifest entry", [&desc]() {
                                    return to_outcome(fuse::cook::write_audio_stub(
                                        desc.input_path, desc.output_path, desc.target_sample_rate, "ogg"));
                                });
    }
    case CookAssetKind::Shader: {
        ShaderImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return cook_with_cache_(CookAssetKind::Shader, entry.source_path, entry.output_path,
                                hash_shader_import(desc), upstream_hash, "stub shader cook from manifest entry",
                                [&desc]() {
                                    return to_outcome(fuse::cook::write_shader_stub(desc.input_path, desc.output_path,
                                                                                    "fragment", desc.target_version));
                                });
    }
    }
    return {};
}

CookJobGraphExecuteResult AssetCooker::cook_manifest_graph(const CookManifest& manifest) {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);
    return graph.execute(*this, manifest);
}

CookBatchResult AssetCooker::cook_manifest(const CookManifest& manifest) {
    return cookBatchFromJobGraphResult(cook_manifest_graph(manifest));
}

u32 AssetCooker::invalidate_upstream_dependency(const CookManifest& manifest, const std::string& changed_source) {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    u32 removed = m_cache.invalidate_source(changed_source);
    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            removed += m_cache.invalidate_downstream_of(job.output_path, graph.edges(), graph.jobs());
        }
    }
    return removed;
}

u32 AssetCooker::count_upstream_invalidation(const CookManifest& manifest,
                                             const std::string& changed_source) const {
    if (!is_valid_cook_cache_path(changed_source)) {
        return 0;
    }

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    u32 count = m_cache.count_by_source(changed_source);
    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            count += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
        }
    }
    return count;
}

bool AssetCooker::would_invalidate_upstream_dependency(const CookManifest& manifest,
                                                       const std::string& changed_source) const {
    return count_upstream_invalidation(manifest, changed_source) != 0;
}

u32 AssetCooker::count_stale_dependency_invalidation(const CookManifest& manifest) const {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));
    }

    u32 count = m_cache.count_stale_upstream_hashes(source_upstream);
    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_sources(source_upstream);
    for (const std::string& stale_source : stale_sources) {
        for (const CookJob& job : graph.jobs()) {
            if (job.source_path == stale_source) {
                count += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
                break;
            }
        }
    }
    return count;
}

bool AssetCooker::would_invalidate_stale_dependency_hashes(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;
}

CookCachePruneEstimate AssetCooker::estimate_prune_reconcile() const {
    return m_cache.estimate_prune_removals();
}

CookCacheReconcileEstimate AssetCooker::estimate_reconcile_invalidation(const CookManifest& manifest) const {
    CookCacheReconcileEstimate estimate;
    estimate.stale_dependency_entries = count_stale_dependency_invalidation(manifest);

    const CookCachePruneEstimate prune = m_cache.estimate_prune_removals();
    estimate.prune_invalid_entries = prune.invalid_entries;
    estimate.prune_stale_entries = prune.stale_entries;
    return estimate;
}

CookCacheUpstreamReconcileEstimate AssetCooker::estimate_upstream_reconcile(
    const CookManifest& manifest, const std::string& changed_source) const {
    CookCacheUpstreamReconcileEstimate estimate;
    if (!is_valid_cook_cache_path(changed_source)) {
        return estimate;
    }

    estimate.direct_source_entries = m_cache.count_by_source(changed_source);

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            estimate.downstream_entries += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
        }
    }
    return estimate;
}

bool AssetCooker::would_reconcile_invalidation(const CookManifest& manifest) const {
    return estimate_reconcile_invalidation(manifest).total() != 0;
}

std::vector<std::string> AssetCooker::probe_upstream_invalidation_sources(
    const CookManifest& manifest, const std::string& changed_source) const {
    if (!is_valid_cook_cache_path(changed_source)) {
        return {};
    }

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::string> sources;
    if (m_cache.count_by_source(changed_source) != 0) {
        sources.push_back(changed_source);
    }

    for (const CookJob& job : graph.jobs()) {
        if (job.source_path != changed_source) {
            continue;
        }

        const std::vector<std::string> downstream =
            m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs());
        for (const std::string& source_path : downstream) {
            bool already_recorded = false;
            for (const std::string& recorded : sources) {
                if (recorded == source_path) {
                    already_recorded = true;
                    break;
                }
            }
            if (!already_recorded) {
                sources.push_back(source_path);
            }
        }
    }
    return sources;
}

u32 AssetCooker::invalidate_stale_dependency_hashes(const CookManifest& manifest) {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));
    }

    const std::vector<std::string> stale_sources = m_cache.invalidate_stale_upstream_hashes(source_upstream);

    u32 removed = static_cast<u32>(stale_sources.size());
    for (const std::string& stale_source : stale_sources) {
        for (const CookJob& job : graph.jobs()) {
            if (job.source_path == stale_source) {
                removed += m_cache.invalidate_downstream_of(job.output_path, graph.edges(), graph.jobs());
                break;
            }
        }
    }
    return removed;
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
