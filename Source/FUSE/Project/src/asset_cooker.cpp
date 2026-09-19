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

CookRecord AssetCooker::cook_with_cache_(CookAssetKind kind,
                                         const std::string& source_path,
                                         const std::string& output_path,
                                         u64 content_hash,
                                         u64 upstream_hash,
                                         const char* stub_note) {
    const u64 cache_key = combine_cook_cache_key(content_hash, upstream_hash);
    const bool cacheable = is_cacheable_cook_cache_key(content_hash, upstream_hash);

    CookCacheEntry cached;
    if (cacheable && m_cache.lookup(cache_key, &cached) == CookCacheLookup::Hit) {
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

    if (record.ok && cacheable) {
        m_cache.invalidate_stale_content_for_source(source_path, cache_key);

        CookCacheEntry entry;
        entry.content_hash = cache_key;
        entry.upstream_hash = upstream_hash;
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
    return cook_with_cache_(CookAssetKind::Mesh, desc.input_path, desc.output_path, content_hash, 0, note.str().c_str());
}

CookRecord AssetCooker::cook_texture(const TextureImportDesc& desc) {
    const char* compression = desc.is_normal_map ? "BC5" : "BC7";
    std::ostringstream note;
    note << "stub texture cook (compression=" << compression
         << ", mipmaps=" << (desc.generate_mipmaps ? "on" : "off") << ")";
    const u64 content_hash = hash_texture_import(desc);
    return cook_with_cache_(CookAssetKind::Texture, desc.input_path, desc.output_path, content_hash, 0, note.str().c_str());
}

CookRecord AssetCooker::cook_audio(const AudioImportDesc& desc) {
    std::ostringstream note;
    note << "stub audio cook (rate=" << desc.target_sample_rate
         << ", format=" << (desc.format == AudioImportDesc::Format::OGG_VORBIS ? "ogg" : "pcm_f32") << ")";
    const u64 content_hash = hash_audio_import(desc);
    return cook_with_cache_(CookAssetKind::Audio, desc.input_path, desc.output_path, content_hash, 0, note.str().c_str());
}

CookRecord AssetCooker::cook_entry(const CookManifestEntry& entry) {
    return cook_entry(entry, CookManifest{});
}

CookRecord AssetCooker::cook_entry(const CookManifestEntry& entry, const CookManifest& manifest) {
    const u64 upstream_hash = hash_upstream_dependencies(entry.dependencies, manifest);

    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        const u64 content_hash = hash_mesh_import(desc);
        return cook_with_cache_(CookAssetKind::Mesh, entry.source_path, entry.output_path, content_hash,
                                upstream_hash, "stub mesh cook from manifest entry");
    }
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        const u64 content_hash = hash_texture_import(desc);
        return cook_with_cache_(CookAssetKind::Texture, entry.source_path, entry.output_path, content_hash,
                                upstream_hash, "stub texture cook from manifest entry");
    }
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        const u64 content_hash = hash_audio_import(desc);
        return cook_with_cache_(CookAssetKind::Audio, entry.source_path, entry.output_path, content_hash,
                                upstream_hash, "stub audio cook from manifest entry");
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
    return graph.execute(*this, manifest);
}

CookBatchResult AssetCooker::cook_manifest(const CookManifest& manifest) {
    return cookBatchFromJobGraphResult(cook_manifest_graph(manifest));
}

u32 AssetCooker::invalidate_upstream_dependency(const CookManifest& manifest, const std::string& changed_source) {
    if (!is_valid_cook_cache_path(changed_source) || m_cache.empty()) {
    if (m_cache.empty() || !is_valid_cook_cache_path(changed_source)) {
        return 0;
    }

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
    return estimate_upstream_invalidation(manifest, changed_source).total();
}

CookUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    const CookManifest& manifest, const std::string& changed_source) const {
    CookUpstreamInvalidationEstimate estimate;
    if (!is_valid_cook_cache_path(changed_source)) {
        return 0;
CookCacheReconcileEstimate AssetCooker::estimate_cache_reconcile() const {
    return m_cache.estimate_reconcile();
}

CookCacheInvalidationProbe AssetCooker::estimate_stale_dependency_invalidation(
    const CookManifest& manifest) const {
    CookCacheInvalidationProbe probe;
    probe.cache_empty = m_cache.empty();
    if (probe.cache_empty) {
        return probe;
u32 AssetCooker::estimate_stale_dependency_entries(const CookManifest& manifest) const {
u32 AssetCooker::estimate_stale_dependency_invalidations(const CookManifest& manifest) const {
u32 AssetCooker::estimate_stale_dependency_hash_invalidations(const CookManifest& manifest) const {
u32 AssetCooker::count_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_hashes(manifest).total_entries();

CookCacheInvalidationProbe AssetCooker::probe_upstream_dependency(const CookManifest& manifest,
                                                                  const std::string& changed_source) const {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);
    return m_cache.probe_upstream_invalidation(changed_source, graph.edges(), graph.jobs());

AssetCooker::CookDependencyReconcileEstimate AssetCooker::estimate_stale_dependency_hashes(
    return estimate_stale_dependency_invalidation(manifest);
}

u32 AssetCooker::estimate_stale_dependency_invalidation(const CookManifest& manifest) const {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));

    return m_cache.count_stale_upstream_entries(source_upstream);

bool AssetCooker::cache_needs_dependency_reconcile(const CookManifest& manifest) const {
    return estimate_stale_dependency_entries(manifest) > 0;

u32 AssetCooker::estimate_upstream_invalidation(const CookManifest& manifest,
    if (!is_valid_cook_cache_path(changed_source) || m_cache.empty()) {
        return estimate;
    }

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    estimate.direct = m_cache.count_by_source(changed_source);
    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            count += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
    return count;

bool AssetCooker::would_invalidate_upstream_dependency(const CookManifest& manifest,
    return count_upstream_invalidation(manifest, changed_source) != 0;

u32 AssetCooker::count_prune_removals() const {
    return m_cache.count_prunable_entries();
}

bool AssetCooker::would_reconcile_stale_dependencies(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) > 0;

u32 AssetCooker::count_stale_content_invalidation(const CookManifest& manifest) const {
    if (m_cache.empty() || manifest.assets.empty()) {
        return 0;


    u32 count = 0;
        if (!is_valid_cook_cache_path(job.source_path)) {
            continue;
            estimate.downstream += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

        u64 current_hash = 0;
        switch (job.kind) {
        case CookAssetKind::Mesh: {
            MeshImportDesc desc;
            desc.input_path = job.source_path;
            desc.output_path = job.output_path;
            current_hash = combine_cook_cache_key(hash_mesh_import(desc), hash_upstream_from_jobs(job, graph.jobs()));
            break;
        case CookAssetKind::Texture: {
            TextureImportDesc desc;
            current_hash =
                combine_cook_cache_key(hash_texture_import(desc), hash_upstream_from_jobs(job, graph.jobs()));
        case CookAssetKind::Audio: {
            AudioImportDesc desc;
                combine_cook_cache_key(hash_audio_import(desc), hash_upstream_from_jobs(job, graph.jobs()));
        case CookAssetKind::Shader:

        count += m_cache.count_stale_content_for_source(job.source_path, current_hash);
    return estimate;

u32 AssetCooker::estimate_prune_all() const {

u32 AssetCooker::estimate_reconcile_invalidation(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) + estimate_prune_all();

u32 AssetCooker::count_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_invalidation(manifest);

u32 AssetCooker::estimate_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_reconcile(manifest).total();

CookStaleDependencyReconcileEstimate AssetCooker::estimate_stale_dependency_reconcile(
    const CookManifest& manifest) const {
    CookStaleDependencyReconcileEstimate estimate;


    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));

    estimate.direct_stale = m_cache.count_stale_upstream_hashes(source_upstream);
    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_sources(source_upstream);
    for (const std::string& stale_source : stale_sources) {
            if (job.source_path == stale_source) {

bool AssetCooker::would_invalidate_stale_dependency_hashes(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;

CookCachePruneEstimate AssetCooker::estimate_prune_reconcile() const {
    return m_cache.estimate_prune_removals();

CookCacheReconcileEstimate AssetCooker::estimate_reconcile_invalidation(const CookManifest& manifest) const {
    CookCacheReconcileEstimate estimate;
    estimate.stale_dependency_entries = count_stale_dependency_invalidation(manifest);

    const CookCachePruneEstimate prune = m_cache.estimate_prune_removals();
    estimate.prune_invalid_entries = prune.invalid_entries;
    estimate.prune_stale_entries = prune.stale_entries;

CookCacheUpstreamReconcileEstimate AssetCooker::estimate_upstream_reconcile(
    const CookManifest& manifest, const std::string& changed_source) const {
    CookCacheUpstreamReconcileEstimate estimate;

    estimate.direct_source_entries = m_cache.count_by_source(changed_source);


            estimate.downstream_entries += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

bool AssetCooker::would_reconcile_invalidation(const CookManifest& manifest) const {
    return estimate_reconcile_invalidation(manifest).total() != 0;

std::vector<std::string> AssetCooker::probe_upstream_invalidation_sources(
        return {};


    std::vector<std::string> sources;
    if (m_cache.count_by_source(changed_source) != 0) {
        sources.push_back(changed_source);

        if (job.source_path != changed_source) {

        const std::vector<std::string> downstream =
            m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs());
        for (const std::string& source_path : downstream) {
            bool already_recorded = false;
            for (const std::string& recorded : sources) {
                if (recorded == source_path) {
                    already_recorded = true;
            if (!already_recorded) {
                sources.push_back(source_path);
    return sources;
u32 AssetCooker::prune_stale_cache() {
    return m_cache.prune_all();
CookCacheInvalidationProbe AssetCooker::probe_upstream_dependency(const CookManifest& manifest,
    return m_cache.probe_upstream_invalidation(changed_source, graph.edges(), graph.jobs());

AssetCooker::CookDependencyReconcileEstimate AssetCooker::estimate_stale_dependency_hashes(


    CookDependencyReconcileEstimate estimate;
    estimate.stale_source_paths = m_cache.probe_stale_upstream_hashes(source_upstream);
    estimate.stale_upstream_entries = m_cache.probe_stale_upstream_hash_entries(source_upstream);

    for (const std::string& stale_source : estimate.stale_source_paths) {
                estimate.downstream_cascade_entries +=
                    m_cache.probe_downstream_of(job.output_path, graph.edges(), graph.jobs());

    const std::vector<std::string> stale_sources = m_cache.probe_invalidate_stale_upstream_hashes(source_upstream);
    probe.would_invalidate_count = static_cast<u32>(stale_sources.size());

                probe.would_invalidate_count +=
                    m_cache.probe_invalidate_downstream_of(job.output_path, graph.edges(), graph.jobs())
                        .would_invalidate_count;
    return probe;
    u32 estimated = m_cache.count_source_entries(changed_source);
            estimated += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());


    u32 estimated = static_cast<u32>(stale_sources.size());
                estimated += m_cache.count_downstream_entries(job.output_path, graph.edges(), graph.jobs());
    return estimated;

    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_invalidation(source_upstream);

    u32 removed = m_cache.probe_stale_upstream_invalidation_count(source_upstream);
                removed += m_cache.probe_invalidate_downstream_of(job.output_path, graph.edges(), graph.jobs());
    return removed;

u32 AssetCooker::count_prune_invalidation() const {
    return m_cache.count_prune_all();

CookCacheReconcileEstimate AssetCooker::estimate_cache_reconcile(const CookManifest& manifest) const {
    estimate.stale_dependency_invalidations = count_stale_dependency_invalidation(manifest);
    estimate.prunable_entries = m_cache.estimate_prune_removals();
    estimate.invalid_entries = m_cache.count_invalid_entries();
    estimate.stale_entries = m_cache.count_stale_entries();

CookCacheReconcileEstimate AssetCooker::estimate_upstream_change_reconcile(const CookManifest& manifest,
                                                                           const std::string& changed_source) const {
    CookCacheReconcileEstimate estimate = estimate_cache_reconcile(manifest);
    estimate.upstream_invalidation = count_upstream_invalidation(manifest, changed_source);
                estimate.downstream_cascade +=
                    m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

bool AssetCooker::would_need_stale_dependency_reconcile(const CookManifest& manifest) const {
    return estimate_stale_dependency_reconcile(manifest).total() > 0;

}

u32 AssetCooker::invalidate_stale_dependency_hashes(const CookManifest& manifest) {
    if (manifest.assets.empty() || m_cache.empty()) {
    if (m_cache.empty() || manifest.assets.empty()) {
        return 0;
    }

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

CookCacheReconcileEstimate AssetCooker::estimate_invalidate_upstream_dependency(
    const CookManifest& manifest, const std::string& changed_source) const {
    CookCacheReconcileEstimate estimate;
    if (!is_valid_cook_cache_path(changed_source) || m_cache.empty()) {
        return estimate;
    }

    estimate.direct_count = m_cache.probe_invalidate_source(changed_source).affected_count;

u32 AssetCooker::estimate_upstream_dependency_invalidation(const CookManifest& manifest,
                                                           const std::string& changed_source) const {
        return 0;

    CookJobGraph graph;
    graph.build_from_manifest(manifest);
    if (graph.empty()) {
        return m_cache.estimate_invalidation_by_source(changed_source);

    u32 estimate = m_cache.estimate_invalidation_by_source(changed_source);
    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            estimate += m_cache.estimate_invalidation_downstream_of(job.output_path, graph.edges(), graph.jobs());

u32 AssetCooker::estimate_stale_dependency_hashes(const CookManifest& manifest) const {
        if (job.source_path != changed_source) {
            continue;
        estimate.downstream_count +=
            m_cache.probe_downstream_of(job.output_path, graph.edges(), graph.jobs()).affected_count;

CookCacheReconcileEstimate AssetCooker::estimate_invalidate_stale_dependency_hashes(
    const CookManifest& manifest) const {
    if (m_cache.empty()) {



u32 AssetCooker::estimate_stale_dependency_reconcile(const CookManifest& manifest) const {
CookCacheReconcileEstimate AssetCooker::estimate_stale_dependency_hashes(const CookManifest& manifest) const {







    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));
    }

    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_source_paths(source_upstream);
    estimate.direct_count = static_cast<u32>(stale_sources.size());
    u32 estimate = m_cache.estimate_stale_upstream_invalidation(source_upstream);
    const std::vector<std::string> stale_sources =
        m_cache.probe_stale_upstream_invalidation_sources(source_upstream);

    for (const std::string& stale_source : stale_sources) {
        for (const CookJob& job : graph.jobs()) {
            if (job.source_path == stale_source) {
                estimate.downstream_count +=
                    m_cache.probe_downstream_of(job.output_path, graph.edges(), graph.jobs()).affected_count;
                estimate += m_cache.estimate_invalidation_downstream_of(job.output_path, graph.edges(),
                                                                          graph.jobs());
                break;
            }
    return estimate;
    return m_cache.count_stale_upstream_entries(source_upstream);
    return m_cache.estimate_stale_upstream_invalidations(source_upstream);
        }
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
