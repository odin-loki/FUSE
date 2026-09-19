#include <fuse/project/asset_cooker.hpp>

#include <fuse/project/cook_content_hash.hpp>
#include <fuse/log/logger.hpp>

#include <sstream>
#include <vector>

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

void append_unique_source_for_probe_(std::vector<std::string>& sources, const std::string& source_path) {
    if (!is_valid_cook_cache_path(source_path)) {
        return;
    }
    for (const std::string& recorded : sources) {
        if (recorded == source_path) {
            return;
        }
    }
    sources.push_back(source_path);
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
u32 AssetCooker::count_output_invalidation(const CookManifest& manifest,
                                           const std::string& changed_output) const {
    if (!is_valid_cook_cache_path(changed_output)) {
        return 0;
CookCacheReconcileEstimate AssetCooker::estimate_cache_reconcile() const {
    return m_cache.estimate_reconcile();

CookCacheInvalidationProbe AssetCooker::estimate_stale_dependency_invalidation(
    const CookManifest& manifest) const {
    CookCacheInvalidationProbe probe;
    probe.cache_empty = m_cache.empty();
    if (probe.cache_empty) {
        return probe;
u32 AssetCooker::estimate_stale_dependency_entries(const CookManifest& manifest) const {
u32 AssetCooker::estimate_stale_dependency_invalidations(const CookManifest& manifest) const {
u32 AssetCooker::estimate_stale_dependency_hash_invalidations(const CookManifest& manifest) const {
namespace {

void append_probe_source_(std::vector<std::string>& sources, const std::string& source_path) {
    if (!is_valid_cook_cache_path(source_path)) {
        return;
    for (const std::string& existing : sources) {
        if (existing == source_path) {
    sources.push_back(source_path);

void probe_downstream_sources_(const CookCache& cache,
                               const std::string& output_path,
                               const std::vector<CookJobDependencyEdge>& edges,
                               const std::vector<CookJob>& jobs,
                               std::vector<std::string>& sources) {
    if (!is_valid_cook_cache_path(output_path) || cache.empty()) {

    if (cache.count_by_source(output_path) > 0) {
        append_probe_source_(sources, output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = nullptr;
        for (const CookJob& job : jobs) {
            if (job.id == edge.from_job_id) {
                from_job = &job;
                break;
        if (!from_job || from_job->output_path != output_path) {
            continue;

        const CookJob* to_job = nullptr;
            if (job.id == edge.to_job_id) {
                to_job = &job;
        if (!to_job) {

        if (cache.count_by_source(to_job->source_path) > 0) {
            append_probe_source_(sources, to_job->source_path);
        probe_downstream_sources_(cache, to_job->output_path, edges, jobs, sources);

} // namespace

std::vector<std::string> AssetCooker::probe_upstream_invalidation_sources(
        return {};
u32 AssetCooker::count_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_reconcile(manifest).total();

AssetCooker::CookUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
        return estimate;
    return estimate_stale_dependency_reconcile(manifest).total;

CookReconcileEstimate AssetCooker::estimate_upstream_invalidation(const CookManifest& manifest,
    CookReconcileEstimate estimate;

    estimate.direct_source_entries = m_cache.count_by_source(changed_source);

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::string> sources;
    if (m_cache.count_by_source(changed_source) > 0) {
        append_probe_source_(sources, changed_source);

    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            probe_downstream_sources_(m_cache, job.output_path, graph.edges(), graph.jobs(), sources);

    return sources;

bool AssetCooker::would_upstream_invalidation(const CookManifest& manifest,
    return count_upstream_invalidation(manifest, changed_source) > 0;

u32 AssetCooker::count_prunable_cache_entries() const {
    return m_cache.count_prunable_entries();

bool AssetCooker::would_stale_dependency_invalidation(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) > 0;

u32 AssetCooker::estimate_cache_prune() const {
    return m_cache.estimate_prune_all();
bool AssetCooker::would_invalidate_upstream(const CookManifest& manifest,

bool AssetCooker::would_reconcile_stale_dependencies(const CookManifest& manifest) const {

u32 AssetCooker::count_prune_reconcile() const {

    return estimate_stale_dependency_hashes(manifest).total_entries();

CookCacheInvalidationProbe AssetCooker::probe_upstream_dependency(const CookManifest& manifest,
    return m_cache.probe_upstream_invalidation(changed_source, graph.edges(), graph.jobs());

AssetCooker::CookDependencyReconcileEstimate AssetCooker::estimate_stale_dependency_hashes(
    return estimate_stale_dependency_invalidation(manifest);

u32 AssetCooker::estimate_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_reconciliation(manifest).total();

CookCacheStaleUpstreamEstimate AssetCooker::estimate_stale_dependency_reconciliation(
    estimate.source_direct = m_cache.count_by_source(changed_source);
            estimate.downstream_cascade +=
                m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

AssetCooker::CookStaleDependencyEstimate AssetCooker::estimate_stale_dependency_reconcile(
    CookStaleDependencyEstimate estimate;
    estimate.direct = m_cache.count_by_source(changed_source);
            estimate.downstream += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
    estimate.total = estimate.direct + estimate.downstream;

AssetCooker::CookDependencyReconcileEstimate AssetCooker::estimate_stale_dependency_reconcile(
    CookDependencyReconcileEstimate estimate;

    estimate.direct_entries = m_cache.count_by_source(changed_source);
            estimate.downstream_entries +=


CookReconcileEstimate AssetCooker::estimate_stale_dependency_reconcile(const CookManifest& manifest) const {

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));

    return m_cache.count_stale_upstream_entries(source_upstream);

bool AssetCooker::cache_needs_dependency_reconcile(const CookManifest& manifest) const {
    return estimate_stale_dependency_entries(manifest) > 0;

u32 AssetCooker::estimate_upstream_invalidation(const CookManifest& manifest,
    if (!is_valid_cook_cache_path(changed_source) || m_cache.empty()) {
CookCacheUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    CookCacheUpstreamInvalidationEstimate estimate;
CookCacheUpstreamInvalidateEstimate AssetCooker::estimate_upstream_invalidation(
    CookCacheUpstreamInvalidateEstimate estimate;
            estimate.downstream_entries += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

bool AssetCooker::would_upstream_invalidate(const CookManifest& manifest,
    return estimate_upstream_invalidation(manifest, changed_source).total() != 0;


void append_unique_upstream_source_(std::vector<std::string>& sources, const std::string& source_path) {
    for (const std::string& recorded : sources) {
        if (recorded == source_path) {


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
    estimate.direct_source_entries = m_cache.count_by_source(changed_source);
            estimate.downstream_entries += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
        }
    return estimate;
    estimate.direct_entries = m_cache.count_by_source(changed_source);
            estimate.downstream_entries +=
                m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

bool AssetCooker::would_upstream_invalidation(const CookManifest& manifest,
                                              const std::string& changed_source) const {
    return estimate_upstream_invalidation(manifest, changed_source).total() != 0;

    return estimate_upstream_invalidation(manifest, changed_source).total() > 0;
    return m_cache.count_downstream_of(changed_output, graph.edges(), graph.jobs());

CookUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    const CookManifest& manifest, const std::string& changed_source) const {
    CookUpstreamInvalidationEstimate estimate;
    if (!is_valid_cook_cache_path(changed_source)) {

    CookJobGraph graph;
    graph.build_from_manifest(manifest);


bool AssetCooker::would_reconcile_invalidation(const CookManifest& manifest) const {
    return estimate_reconcile_invalidation(manifest).total() != 0;

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

        if (job.source_path != changed_source) {
            continue;
    append_unique_upstream_source_(sources, changed_source);

    }

    for (const CookJob& job : graph.jobs()) {

        const std::vector<std::string> downstream =
            m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs());
        for (const std::string& source_path : downstream) {
            bool already_recorded = false;
            for (const std::string& recorded : sources) {
                if (recorded == source_path) {
                    already_recorded = true;
                    break;
            if (!already_recorded) {
                sources.push_back(source_path);
    return sources;

bool AssetCooker::would_upstream_invalidate(const CookManifest& manifest,

bool AssetCooker::would_stale_dependency_invalidate(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;

bool AssetCooker::would_reconcile_invalidate(const CookManifest& manifest) const {
    return estimate_reconcile_invalidation(manifest).total() != 0;
    auto append_unique = [&](const std::string& source_path) {
        if (!is_valid_cook_cache_path(source_path)) {
            return;
    };

    append_unique(changed_source);

        const std::vector<std::string> downstream = m_cache.probe_downstream_sources(
            job.output_path, graph.edges(), graph.jobs());
            append_unique(source_path);
            append_unique_upstream_source_(sources, source_path);

u32 AssetCooker::count_upstream_invalidation(const CookManifest& manifest,
    return estimate_upstream_invalidation(manifest, changed_source).total();

    return m_cache.count_downstream_of(changed_output, graph.edges(), graph.jobs());

CookUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    CookUpstreamInvalidationEstimate estimate;


                }
}

bool AssetCooker::would_upstream_invalidate(const CookManifest& manifest,
                                          const std::string& changed_source) const {
bool AssetCooker::would_invalidate_upstream_dependency(const CookManifest& manifest,
    return count_upstream_invalidation(manifest, changed_source) != 0;
}

std::vector<std::string> AssetCooker::probe_upstream_invalidation_sources(
    const CookManifest& manifest, const std::string& changed_source) const {
    if (!is_valid_cook_cache_path(changed_source)) {
    if (!is_valid_cook_cache_path(changed_source) || m_cache.empty()) {
        return {};
    }

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::string> sources;
    if (m_cache.count_by_source(changed_source) != 0) {
        sources.push_back(changed_source);

        if (job.source_path != changed_source) {
            continue;

        const std::vector<std::string> downstream =
            m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs());
        for (const std::string& source_path : downstream) {
            bool already_recorded = false;
            for (const std::string& recorded : sources) {
                if (recorded == source_path) {
                    already_recorded = true;
                    break;
            if (!already_recorded) {
                sources.push_back(source_path);
    return sources;
    return m_cache.count_downstream_of(changed_output, graph.edges(), graph.jobs());

CookUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    CookUpstreamInvalidationEstimate estimate;


            estimate.downstream_entries +=
                m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

bool AssetCooker::would_upstream_invalidate(const CookManifest& manifest,
    }

    for (const CookJob& job : graph.jobs()) {

        for (const std::string& path : downstream) {
                if (recorded == path) {
                sources.push_back(path);

bool AssetCooker::would_stale_dependency_invalidation(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;

bool AssetCooker::would_reconcile_stale_dependencies(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) > 0;

u32 AssetCooker::count_stale_content_invalidation(const CookManifest& manifest) const {
    if (m_cache.empty() || manifest.assets.empty()) {
        return 0;


    u32 count = 0;
        if (!is_valid_cook_cache_path(job.source_path)) {
            estimate.downstream += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());

        u64 current_hash = 0;
        switch (job.kind) {
        case CookAssetKind::Mesh: {
            MeshImportDesc desc;
            desc.input_path = job.source_path;
            desc.output_path = job.output_path;
            current_hash = combine_cook_cache_key(hash_mesh_import(desc), hash_upstream_from_jobs(job, graph.jobs()));
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
    if (m_cache.would_invalidate_source(changed_source)) {
        append_unique_source_for_probe_(sources, changed_source);

        for (const std::string& source : downstream) {
            append_unique_source_for_probe_(sources, source);

                                            const std::string& changed_source) const {
    return count_upstream_invalidation(manifest, changed_source) != 0;


        for (const std::string& downstream_source :
             m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs())) {
                if (recorded == downstream_source) {
                sources.push_back(downstream_source);



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
    estimate.direct_upstream_stale = m_cache.count_stale_upstream_hashes(source_upstream);
    estimate.direct_entries = m_cache.count_stale_upstream_hashes(source_upstream);
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

    return m_cache.estimate_stale_upstream_reconciliation(source_upstream, graph.edges(), graph.jobs());
}

bool AssetCooker::would_invalidate_upstream_dependency(const CookManifest& manifest,
    return count_upstream_invalidation(manifest, changed_source) > 0;

bool AssetCooker::would_reconcile_stale_dependencies(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) > 0;


bool AssetCooker::would_stale_dependency_invalidation(const CookManifest& manifest) const {
                break;
    return estimate;
                estimate.downstream_stale +=
    estimate.total = estimate.direct_stale + estimate.downstream_stale;

u32 AssetCooker::count_upstream_invalidation(const CookManifest& manifest,
    return estimate_upstream_invalidation(manifest, changed_source).total;
                estimate.downstream_entries +=

u32 AssetCooker::count_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_reconcile(manifest).total();

    return estimate_upstream_invalidation(manifest, changed_source).would_invalidate();

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));
    }

    CookCacheReconcileEstimate estimate = m_cache.estimate_reconcile(source_upstream);

    estimate.stale_upstream_entries = m_cache.count_stale_upstream_hashes(source_upstream);
    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_sources(source_upstream);
    u32 downstream_cascade = 0;
    for (const std::string& stale_source : stale_sources) {
        for (const CookJob& job : graph.jobs()) {
            if (job.source_path == stale_source) {
                downstream_cascade +=
                    m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
                break;
            }
        }
    }

    estimate.stale_upstream_entries += downstream_cascade;
    return estimate;
}

u32 AssetCooker::count_stale_content_invalidation(const CookManifest& manifest) const {
    if (manifest.assets.empty()) {
        return 0;
    }

    u32 count = m_cache.count_stale_entries();
    const std::vector<std::string> stale_sources = m_cache.probe_stale_content_sources();
    if (stale_sources.empty()) {
        return count;
    }

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    for (const std::string& stale_source : stale_sources) {
        for (const CookJob& job : graph.jobs()) {
            if (job.source_path == stale_source) {
                estimate.downstream_cascade_entries +=
                    m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
                break;
            }
        }
    }
    return estimate;
}

u32 AssetCooker::count_stale_dependency_invalidation(const CookManifest& manifest) const {
    return estimate_stale_dependency_reconcile(manifest).total();
}

u32 AssetCooker::estimate_reconcile_removals(const CookManifest& manifest) const {
    if (manifest.assets.empty()) {
        return 0;
    }

    const u32 stale_content = count_stale_content_invalidation(manifest);
    const u32 stale_dependencies = count_stale_dependency_invalidation(manifest);
    if (stale_content == 0) {
        return stale_dependencies;
    if (stale_dependencies == 0) {
        return stale_content;
    return stale_content + stale_dependencies;
u32 AssetCooker::count_prune_reconcile() const {
    return m_cache.estimate_prune_reconcile();

u32 AssetCooker::estimate_manifest_cache_reconcile(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) + count_prune_reconcile();
}

std::vector<std::string> AssetCooker::probe_stale_dependency_sources(const CookManifest& manifest) const {
    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));
    }

    return m_cache.probe_unique_stale_upstream_sources(source_upstream);
}

bool AssetCooker::would_stale_dependency_invalidate(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;
}

bool AssetCooker::would_stale_dependency_invalidation(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;
}

std::vector<std::string> AssetCooker::probe_stale_dependency_sources(
    const CookManifest& manifest) const {
    if (m_cache.empty()) {
        return {};
    }

    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
    for (const CookJob& job : graph.jobs()) {
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));
    }

    std::vector<std::string> sources;
    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_sources(source_upstream);
    for (const std::string& stale_source : stale_sources) {
        bool already_recorded = false;
        for (const std::string& recorded : sources) {
            if (recorded == stale_source) {
                already_recorded = true;
                break;
            }
        }
        if (!already_recorded) {
            sources.push_back(stale_source);
        }

        for (const CookJob& job : graph.jobs()) {
            if (job.source_path != stale_source) {
                continue;
            }

            const std::vector<std::string> downstream =
                m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs());
            for (const std::string& path : downstream) {
                bool downstream_recorded = false;
                for (const std::string& recorded : sources) {
                    if (recorded == path) {
                        downstream_recorded = true;
                        break;
                    }
                }
                if (!downstream_recorded) {
                    sources.push_back(path);
                }
            }
            break;
        }
    }
    return sources;
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

u32 AssetCooker::estimate_prune_reconcile() const {
    return m_cache.estimate_prune_all();
}

u32 AssetCooker::estimate_full_cache_reconcile(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) + estimate_prune_reconcile();
bool AssetCooker::would_reconcile_invalidation(const CookManifest& manifest) const {
    return estimate_reconcile_invalidation(manifest).total() != 0;

namespace {

void append_unique_upstream_source_(std::vector<std::string>& sources, const std::string& source_path) {
    if (!is_valid_cook_cache_path(source_path)) {
        return;
    for (const std::string& recorded : sources) {
        if (recorded == source_path) {
    sources.push_back(source_path);

} // namespace

bool AssetCooker::would_upstream_invalidation(const CookManifest& manifest,
                                              const std::string& changed_source) const {
    return count_upstream_invalidation(manifest, changed_source) != 0;

bool AssetCooker::would_upstream_invalidate(const CookManifest& manifest,
CookCacheUpstreamReconcileEstimate AssetCooker::estimate_upstream_reconcile(
    const CookManifest& manifest, const std::string& changed_source) const {
    CookCacheUpstreamReconcileEstimate estimate;
    if (!is_valid_cook_cache_path(changed_source)) {
        return estimate;

    estimate.direct_source_entries = m_cache.count_by_source(changed_source);
CookUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    CookUpstreamInvalidationEstimate estimate;

bool AssetCooker::would_invalidate_upstream(const CookManifest& manifest,
    return count_upstream_invalidation(manifest, changed_source) > 0;

bool AssetCooker::would_invalidate_stale_dependencies(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) > 0;

std::vector<std::string> AssetCooker::probe_upstream_invalidation_sources(
        return {};

CookCacheUpstreamInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    CookCacheUpstreamInvalidationEstimate estimate;

bool AssetCooker::would_prune_reconcile() const {
    return estimate_prune_reconcile().total() != 0;


CookUpstreamReconcileEstimate AssetCooker::estimate_upstream_invalidation(const CookManifest& manifest,
    CookUpstreamReconcileEstimate estimate;

u32 AssetCooker::count_prune_reconcile() const {
    return estimate_prune_reconcile().total();

CookCacheReconcileEstimate AssetCooker::estimate_reconcile_invalidation(
    CookCacheReconcileEstimate estimate = estimate_reconcile_invalidation(manifest);
    if (is_valid_cook_cache_path(changed_source)) {
        estimate.upstream_invalidation_entries = count_upstream_invalidation(manifest, changed_source);








    CookJobGraph graph;
    graph.build_from_manifest(manifest);

    for (const CookJob& job : graph.jobs()) {
        if (job.source_path == changed_source) {
            estimate.downstream_entries += m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());




CookCacheUpstreamInvalidateEstimate AssetCooker::estimate_upstream_invalidation(
    CookCacheUpstreamInvalidateEstimate estimate;
bool AssetCooker::would_reconcile_invalidate(const CookManifest& manifest) const {

std::vector<std::string> AssetCooker::probe_reconcile_stale_sources(const CookManifest& manifest) const {
    std::vector<std::string> stale_sources = m_cache.probe_stale_content_sources();
    if (count_stale_dependency_invalidation(manifest) == 0) {
        return stale_sources;


    std::vector<std::pair<std::string, u64>> source_upstream;
    source_upstream.reserve(graph.jobs().size());
        source_upstream.emplace_back(job.source_path, hash_upstream_from_jobs(job, graph.jobs()));

    const std::vector<std::string> upstream_stale = m_cache.probe_stale_upstream_sources(source_upstream);
    for (const std::string& source_path : upstream_stale) {
        bool already_recorded = false;
        for (const std::string& recorded : stale_sources) {
                already_recorded = true;
                break;
        if (!already_recorded) {
            stale_sources.push_back(source_path);

CookCacheInvalidationEstimate AssetCooker::estimate_upstream_invalidation(
    CookCacheInvalidationEstimate estimate;



    std::vector<std::string> sources;
    append_unique_upstream_source_(sources, changed_source);
    if (m_cache.would_invalidate_source(changed_source)) {
    estimate.direct_entries = m_cache.count_by_source(changed_source);
            const u32 downstream = m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());
            estimate.downstream_entries += downstream;



    if (m_cache.count_by_source(changed_source) != 0) {
        sources.push_back(changed_source);

        if (job.source_path != changed_source) {
            continue;




    if (!is_valid_cook_cache_path(changed_source) || m_cache.empty()) {





        const std::vector<std::string> downstream =
            m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs());
        for (const std::string& source_path : downstream) {
            append_unique_upstream_source_(sources, source_path);

    return sources;

bool AssetCooker::would_stale_dependency_invalidation(const CookManifest& manifest) const {

    return estimate_reconcile_invalidation(manifest).total() > 0;

        for (const std::string& path : downstream) {
                if (recorded == path) {
                sources.push_back(path);



std::vector<std::string> AssetCooker::probe_stale_dependency_sources(const CookManifest& manifest) const {


    return m_cache.probe_stale_upstream_sources_dedup(source_upstream);


            estimate.downstream_entries +=
                m_cache.count_downstream_of(job.output_path, graph.edges(), graph.jobs());



    return estimate_upstream_invalidation(manifest, changed_source).total() != 0;
    estimate.source_entries = m_cache.count_by_source(changed_source);


std::vector<std::string> AssetCooker::probe_upstream_invalidation_closure(


    if (m_cache.count_by_source(changed_source) > 0) {

    auto append_unique = [](std::vector<std::string>& out, const std::string& path) {
        if (!is_valid_cook_cache_path(path)) {
        for (const std::string& existing : out) {
            if (existing == path) {
        out.push_back(path);
    };

    append_unique(sources, changed_source);

        const std::vector<std::string> downstream = m_cache.probe_downstream_sources(
            job.output_path, graph.edges(), graph.jobs());
            append_unique(sources, path);




std::vector<std::string> AssetCooker::probe_upstream_invalidation_sources(const CookManifest& manifest,


    auto append_unique = [&](const std::string& path) {

    append_unique(changed_source);
            for (const std::string& probed :
                 m_cache.probe_downstream_sources(job.output_path, graph.edges(), graph.jobs())) {
                append_unique(probed);



bool AssetCooker::would_stale_dependency_invalidate(const CookManifest& manifest) const {
    return count_stale_dependency_invalidation(manifest) != 0;
    const auto append_unique = [&](const std::string& source_path) {



        for (const std::string& downstream_source :
            append_unique(downstream_source);









    return estimate_reconcile_invalidation(manifest).can_reconcile();

bool AssetCooker::should_skip_reconcile_invalidation(const CookManifest& manifest) const {
    return estimate_reconcile_invalidation(manifest).should_skip();

CookStaleDependencyEstimate AssetCooker::estimate_stale_dependency_reconcile(const CookManifest& manifest) const {
    CookStaleDependencyEstimate estimate;
    if (m_cache.empty()) {



    estimate.stale_upstream_entries = m_cache.count_stale_upstream_hashes(source_upstream);

    const std::vector<std::string> stale_sources = m_cache.probe_stale_upstream_sources(source_upstream);
    for (const std::string& stale_source : stale_sources) {
            if (job.source_path == stale_source) {
                estimate.downstream_cascade_entries +=
    return estimate_reconcile_invalidation(manifest).would_reconcile();




    auto append_if_cached = [&](const std::string& source_path) {
        if (!is_valid_cook_cache_path(source_path) || m_cache.count_by_source(source_path) == 0) {

    append_if_cached(changed_source);


        for (const std::string& downstream : m_cache.probe_downstream_sources(
                 job.output_path, graph.edges(), graph.jobs())) {
            append_if_cached(downstream);

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
