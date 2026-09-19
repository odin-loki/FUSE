#include <fuse/project/cook_cache.hpp>

#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/import_desc.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fuse::project {

namespace {

u64 recompute_cache_key_for_entry_(const CookCacheEntry& entry) {
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
        return 0;
    }
    return combine_cook_cache_key(source_hash, entry.upstream_hash);
}

bool source_exists_for_entry_(const CookCacheEntry& entry) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(entry.source_path), ec);
bool is_invalid_cache_entry_(const CookCacheEntry& entry) {
    return !is_valid_cook_cache_entry(entry);
}

bool is_stale_cache_entry_(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_entry(entry)) {
        return false;

    if (entry.kind == CookAssetKind::Shader) {
        return true;

    if (!source_exists_for_entry_(entry)) {
bool is_stale_cook_cache_entry_(const CookCacheEntry& entry) {

    const u64 current_key = recompute_cache_key_for_entry_(entry);
    return !is_valid_cook_cache_key(current_key) || entry.content_hash != current_key;
bool is_prunable_cache_entry_(const CookCacheEntry& entry) {
    return is_invalid_cook_cache_entry(entry) || is_stale_cook_cache_entry(entry);
}

bool is_prunable_cache_entry_(const CookCacheEntry& entry) {
    return !is_valid_cook_cache_entry(entry) || is_stale_cache_entry_(entry);
    return is_invalid_cache_entry_(entry) || is_stale_cache_entry_(entry);
}

bool is_prunable_cache_entry_(const CookCacheEntry& entry) {
    return is_invalid_cook_cache_entry(entry) || is_stale_cook_cache_entry_(entry);
}

bool output_exists_for_entry_(const CookCacheEntry& entry) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(entry.output_path), ec);

} // namespace

CookCacheEntryPreflight preflight_cache_entry(const CookCacheEntry& entry) {
    CookCacheEntryPreflight result;
    result.zero_key = !is_valid_cook_cache_key(entry.content_hash);
    result.empty_path = !is_valid_cook_cache_path(entry.source_path) ||
                        !is_valid_cook_cache_path(entry.output_path);
    result.structurally_valid = is_valid_cook_cache_entry(entry);
    result.shader_kind = entry.kind == CookAssetKind::Shader;

    if (!result.structurally_valid) {
        return result;

    result.source_missing = !source_exists_for_entry_(entry);
    result.output_missing = !output_exists_for_entry_(entry);
    result.stale_content = is_stale_cache_entry_(entry);

bool is_valid_cook_cache_entry_on_disk(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_entry(entry)) {
        return false;
    return output_exists_for_entry_(entry);

namespace {

bool is_stale_only_cache_entry_(const CookCacheEntry& entry) {
    return is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry);

std::string escapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

} // namespace

bool is_stale_cook_cache_entry(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_entry(entry)) {
        return false;
    }

    const u64 current_key = recompute_cache_key_for_entry_(entry);
    return !is_valid_cook_cache_key(current_key) || entry.content_hash != current_key;
    return is_stale_cook_cache_entry_(entry);
CookCacheLookupPreflight preflight_cook_cache_lookup(const CookCache& cache, u64 content_hash) {
    CookCacheLookupPreflight preflight;
    preflight.zero_key = !is_valid_cook_cache_key(content_hash);
    if (preflight.zero_key) {
        return preflight;

    preflight.empty_cache = cache.empty();
    if (preflight.empty_cache) {

    preflight.has_entry = cache.contains(content_hash);
    }

        return preflight;


CookCacheStorePreflight preflight_cook_cache_store(const CookCacheEntry& entry) {
    CookCacheStorePreflight preflight;
    preflight.zero_key = !is_valid_cook_cache_key(entry.content_hash);
    preflight.empty_source_path = !is_valid_cook_cache_path(entry.source_path);
    preflight.empty_output_path = !is_valid_cook_cache_path(entry.output_path);
CookCacheKeyPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookCacheKeyPreflight preflight{};
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookCacheKeyRejectReason::ZeroSourceHash;

    if (!is_valid_cook_cache_path(entry.source_path) || !is_valid_cook_cache_path(entry.output_path)) {
        preflight.reason = CookCacheKeyRejectReason::UncacheableFold;

    preflight.valid = true;
    preflight.combined_key = entry.content_hash;
    return is_stale_cache_entry_(entry);

        preflight.zero_content_hash = true;
    if (!is_valid_cook_cache_path(entry.source_path)) {
        preflight.empty_source_path = true;
    if (!is_valid_cook_cache_path(entry.output_path)) {
        preflight.empty_output_path = true;

    return cache.preflight_lookup(content_hash);
CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        preflight.reason = CookHashRejectReason::EmptyInputPath;
        preflight.reason = CookHashRejectReason::EmptyOutputPath;

    switch (entry.kind) {
    case CookAssetKind::Mesh: {
        MeshImportDesc desc;
        desc.input_path = entry.source_path;
        desc.output_path = entry.output_path;
        return preflight_mesh_import_hash(desc);
    case CookAssetKind::Texture: {
        TextureImportDesc desc;
        return preflight_texture_import_hash(desc);
    case CookAssetKind::Audio: {
        AudioImportDesc desc;
        return preflight_audio_import_hash(desc);
    case CookAssetKind::Shader:
        preflight.can_hash = true;
        preflight.reason = CookHashRejectReason::None;
    return preflight;
}

CookCacheEntry* CookCache::find_entry_(u64 content_hash) {
    for (CookCacheEntry& entry : m_entries) {
        if (entry.content_hash == content_hash) {
            return &entry;
        }
    }
    return nullptr;
}

const CookCacheEntry* CookCache::find_entry_(u64 content_hash) const {
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.content_hash == content_hash) {
            return &entry;
        }
    }
    return nullptr;
}

CookCacheLookup CookCache::lookup(u64 content_hash, CookCacheEntry* out_entry) {
    if (!is_valid_cook_cache_key(content_hash)) {
        return CookCacheLookup::Miss;
    }
    if (m_entries.empty()) {
        ++m_stats.misses;
        return CookCacheLookup::Miss;
    }

    const CookCacheEntry* entry = find_entry_(content_hash);
    if (!entry) {
        ++m_stats.misses;
        return CookCacheLookup::Miss;
    }

    ++m_stats.hits;
    if (out_entry) {
        *out_entry = *entry;
    }
    return CookCacheLookup::Hit;
}

void CookCache::store(const CookCacheEntry& entry) {
    if (!is_valid_cook_cache_entry(entry)) {
        return;
    }

    if (CookCacheEntry* existing = find_entry_(entry.content_hash)) {
        *existing = entry;
        return;
    }
    m_entries.push_back(entry);
}

bool CookCache::invalidate(u64 content_hash) {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
        return false;
    }

    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->content_hash == content_hash) {
            m_entries.erase(it);
            ++m_stats.invalidations;
            return true;
        }
    }
    return false;
}

namespace {

const CookJob* find_job_by_id(const std::vector<CookJob>& jobs, const std::string& job_id) {
    for (const CookJob& job : jobs) {
        if (job.id == job_id) {
            return &job;
        }
    }
    return nullptr;
}

} // namespace

u32 CookCache::invalidate_downstream_of(const std::string& output_path,
                                        const std::vector<CookJobDependencyEdge>& edges,
                                        const std::vector<CookJob>& jobs) {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
        return 0;
    }

    u32 removed = invalidate_source(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {
            continue;
        }

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {
            continue;
        }

        removed += invalidate_source(to_job->source_path);
        removed += invalidate_downstream_of(to_job->output_path, edges, jobs);
    }

    return removed;
}

std::vector<std::string> CookCache::probe_stale_upstream_sources(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return {};
    }

    std::vector<std::string> stale_sources;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                stale_sources.push_back(source_path);
            }
        }
    }
    return stale_sources;
}

std::vector<std::string> CookCache::invalidate_stale_upstream_hashes(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return {};
    }

    std::vector<std::string> invalidated;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (it->source_path == source_path && it->upstream_hash != current_upstream) {
                invalidated.push_back(source_path);
                it = m_entries.erase(it);
                ++m_stats.invalidations;
            } else {
                ++it;
            }
        }
    }
    return invalidated;
}

u32 CookCache::invalidate_source(const std::string& source_path) {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {
        return 0;
    }

    u32 removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->source_path == source_path) {
            it = m_entries.erase(it);
            ++removed;
            ++m_stats.invalidations;
        } else {
            ++it;
        }
    }
    return removed;
}

u32 CookCache::invalidate_stale_content_for_source(const std::string& source_path, u64 current_content_hash) {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {
        return 0;
    }

    u32 removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->source_path != source_path) {
            ++it;
            continue;
        }

        if (it->content_hash != current_content_hash) {
            it = m_entries.erase(it);
            ++removed;
            ++m_stats.invalidations;
        } else {
            ++it;
        }
    }
    return removed;
}

u32 CookCache::invalidate_output(const std::string& output_path) {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
        return 0;
    }

    u32 removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->output_path == output_path) {
            it = m_entries.erase(it);
            ++removed;
            ++m_stats.invalidations;
        } else {
            ++it;
        }
    }
    return removed;
}

void CookCache::invalidate_all() {
    if (m_entries.empty()) {
        return;
    }

    const u32 removed = static_cast<u32>(m_entries.size());
    m_stats.invalidations += removed;
    m_entries.clear();
}

u32 CookCache::prune_stale_entries() {
    if (m_entries.empty() || !has_stale_entries()) {
        return 0;
    }

    u32 removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (is_stale_cache_entry_(*it)) {
        if (is_stale_cook_cache_entry(*it)) {
            it = m_entries.erase(it);
            ++removed;
            ++m_stats.invalidations;
        } else {
            ++it;
        }
    }
    return removed;
}

u32 CookCache::prune_invalid_entries() {
    if (m_entries.empty() || !has_invalid_entries()) {
        return 0;
    }

    u32 removed = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (is_invalid_cache_entry_(*it)) {
        if (is_invalid_cook_cache_entry(*it)) {
            it = m_entries.erase(it);
            ++removed;
            ++m_stats.invalidations;
        } else {
            ++it;
        }
    }
    return removed;
}

u32 CookCache::prune_all() {
    if (m_entries.empty() || !has_prunable_entries()) {
        return 0;
    }
    return prune_invalid_entries() + prune_stale_entries();

bool CookCache::has_invalid_entries() const {
    if (m_entries.empty()) {
        return false;

    for (const CookCacheEntry& entry : m_entries) {
        if (is_invalid_cache_entry_(entry)) {
            return true;

bool CookCache::would_invalidate(u64 content_hash) const {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
    return find_entry_(content_hash) != nullptr;

bool CookCache::would_invalidate_source(const std::string& source_path) const {
    return count_by_source(source_path) != 0;

bool CookCache::would_invalidate_output(const std::string& output_path) const {
    return count_by_output(output_path) != 0;

bool CookCache::would_invalidate_stale_content_for_source(const std::string& source_path,
                                                          u64 current_content_hash) const {
    return count_stale_content_for_source(source_path, current_content_hash) != 0;

bool CookCache::would_invalidate_stale_upstream_hashes(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    return count_stale_upstream_hashes(source_upstream_by_path) != 0;

bool CookCache::would_invalidate_downstream_of(const std::string& output_path,
                                               const std::vector<CookJobDependencyEdge>& edges,
                                               const std::vector<CookJob>& jobs) const {
    return count_downstream_of(output_path, edges, jobs) != 0;

bool CookCache::would_invalidate_source(const std::string& source_path) const {
    return count_by_source(source_path) > 0;
}

bool CookCache::would_invalidate_output(const std::string& output_path) const {
    return count_by_output(output_path) > 0;
}

bool CookCache::would_invalidate_stale_content_for_source(const std::string& source_path,
                                                          u64 current_content_hash) const {
    return count_stale_content_for_source(source_path, current_content_hash) > 0;
}

bool CookCache::would_invalidate_downstream_of(const std::string& output_path,
                                               const std::vector<CookJobDependencyEdge>& edges,
                                               const std::vector<CookJob>& jobs) const {
    return count_downstream_of(output_path, edges, jobs) > 0;
}

bool CookCache::would_invalidate_source(const std::string& source_path) const {
    return count_by_source(source_path) > 0;
}

bool CookCache::would_invalidate_output(const std::string& output_path) const {
    return count_by_output(output_path) > 0;
}

bool CookCache::would_invalidate_source(const std::string& source_path) const {
    return count_by_source(source_path) > 0;
}

bool CookCache::would_invalidate_output(const std::string& output_path) const {
    return count_by_output(output_path) > 0;
}

bool CookCache::would_invalidate_downstream_of(const std::string& output_path,
                                               const std::vector<CookJobDependencyEdge>& edges,
                                               const std::vector<CookJob>& jobs) const {
    return count_downstream_of(output_path, edges, jobs) > 0;
}

u32 CookCache::count_by_source(const std::string& source_path) const {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {

    u32 count = 0;
        if (entry.source_path == source_path) {
            ++count;
    return count;

u32 CookCache::count_by_output(const std::string& output_path) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {

        if (entry.output_path == output_path) {

u32 CookCache::count_stale_content_for_source(const std::string& source_path, u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {

        if (entry.source_path == source_path && entry.content_hash != current_content_hash) {

u32 CookCache::count_stale_upstream_hashes(
    if (m_entries.empty() || source_upstream_by_path.empty()) {

    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        const u64 current_upstream = pair.second;

            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {

std::vector<std::string> CookCache::probe_stale_upstream_sources(
        return {};

    std::vector<std::string> stale_sources;

                stale_sources.push_back(source_path);
    return stale_sources;

namespace {

u32 count_downstream_of_(const CookCache& cache,
                         const std::string& output_path,
                         const std::vector<CookJob>& jobs) {
    if (!is_valid_cook_cache_path(output_path) || cache.empty()) {

    u32 count = cache.count_by_source(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {

        count += cache.count_by_source(to_job->source_path);
        count += count_downstream_of_(cache, to_job->output_path, edges, jobs);


} // namespace

u32 CookCache::count_downstream_of(const std::string& output_path,
    return count_downstream_of_(*this, output_path, edges, jobs);

u32 CookCache::count_prunable_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_prunable_cache_entry_(entry)) {
            ++count;
        }
    return count;

u32 CookCache::count_invalid_entries() const {
    if (m_entries.empty()) {
        return 0;

        if (!is_valid_cook_cache_entry(entry)) {

u32 CookCache::count_stale_entries() const {
    return estimate_prune_removals().stale_entries;

CookCachePruneEstimate CookCache::estimate_prune_removals() const {
    CookCachePruneEstimate estimate;
        return estimate;

            ++estimate.invalid_entries;
            continue;
        if (is_stale_cache_entry_(entry)) {
            ++estimate.stale_entries;

bool CookCache::would_prune_all() const {
    return estimate_prune_removals().total() != 0;

std::vector<std::string> CookCache::probe_stale_content_sources() const {
        return {};

    std::vector<std::string> stale_sources;
        if (!is_valid_cook_cache_entry(entry) || !is_stale_cache_entry_(entry)) {

        bool already_recorded = false;
        for (const std::string& recorded : stale_sources) {
            if (recorded == entry.source_path) {
                already_recorded = true;
                break;
        if (!already_recorded) {
            stale_sources.push_back(entry.source_path);
    return stale_sources;

namespace {

void append_unique_source_(std::vector<std::string>& sources, const std::string& source_path) {
    if (!is_valid_cook_cache_path(source_path)) {
        return;
    for (const std::string& recorded : sources) {
        if (recorded == source_path) {
    sources.push_back(source_path);

void probe_downstream_sources_(const CookCache& cache,
                               const std::string& output_path,
                               const std::vector<CookJobDependencyEdge>& edges,
                               const std::vector<CookJob>& jobs,
                               std::vector<std::string>& sources) {
    if (!is_valid_cook_cache_path(output_path) || cache.empty()) {

    append_unique_source_(sources, output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {

        append_unique_source_(sources, to_job->source_path);
        probe_downstream_sources_(cache, to_job->output_path, edges, jobs, sources);

} // namespace

std::vector<std::string> CookCache::probe_downstream_sources(
    const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
    const std::vector<CookJob>& jobs) const {
    std::vector<std::string> sources;
    probe_downstream_sources_(*this, output_path, edges, jobs, sources);
    return sources;
    u32 removed = prune_invalid_entries();
    removed += prune_stale_entries();
    const u32 removed = prune_stale_entries() + prune_invalid_entries();
    return removed;
}

bool CookCache::has_stale_entries() const {
bool CookCache::has_prunable_entries() const {
    return count_prunable_entries() > 0;
}

bool CookCache::has_invalid_entries() const {
u32 CookCache::count_prunable_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_prunable_cache_entry_(entry)) {
            ++count;
    return count;

u32 CookCache::count_stale_content_for_source(const std::string& source_path, u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {

        if (entry.source_path == source_path && entry.content_hash != current_content_hash) {

u32 CookCache::count_source_entries(const std::string& source_path) const {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {

        if (entry.source_path == source_path) {

u32 CookCache::count_downstream_of(const std::string& output_path,
                                   const std::vector<CookJobDependencyEdge>& edges,
                                   const std::vector<CookJob>& jobs) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {

    u32 count = count_source_entries(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {
            continue;

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {

        count += count_source_entries(to_job->source_path);
        count += count_downstream_of(to_job->output_path, edges, jobs);


u32 CookCache::count_stale_upstream_entries(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {

    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
        const u64 current_upstream = pair.second;

            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {

CookCacheLookup CookCache::preflight_lookup(u64 content_hash, LookupRejectReason* reason) const {
    if (!is_valid_cook_cache_key(content_hash)) {
        if (reason) {
            *reason = LookupRejectReason::ZeroKey;
        return CookCacheLookup::Miss;
            *reason = LookupRejectReason::EmptyCache;

    if (find_entry_(content_hash) == nullptr) {
            *reason = LookupRejectReason::NotFound;

        *reason = LookupRejectReason::None;
    return CookCacheLookup::Hit;

bool CookCache::preflight_store(const CookCacheEntry& entry, StoreRejectReason* reason) const {
    if (!is_valid_cook_cache_entry(entry)) {
            *reason = StoreRejectReason::InvalidEntry;
        return false;

        if (is_stale_cache_entry_(entry)) {
        if (is_invalid_cook_cache_entry(entry)) {
            return true;
        *reason = StoreRejectReason::None;

bool CookCache::has_stale_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_prunable_cache_entry_(entry)) {
            ++count;
        }
    }
    return count;
}

CookCachePruneEstimate CookCache::estimate_prune_all() const {
    CookCachePruneEstimate estimate;
    if (m_entries.empty()) {
        return estimate;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (is_stale_cook_cache_entry(entry)) {
            return true;

bool CookCache::has_prunable_entries() const {
    return has_invalid_entries() || has_stale_entries();
        if (is_stale_cook_cache_entry_(entry)) {

u32 CookCache::count_prunable_entries() const {
        return 0;

    u32 count = 0;
        if (is_prunable_cache_entry_(entry)) {
            ++count;
    return count;

u32 CookCache::probe_invalidate_source(const std::string& source_path) const {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {

        if (entry.source_path == source_path) {

u32 CookCache::probe_stale_content_for_source(const std::string& source_path, u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {

        if (entry.source_path == source_path && entry.content_hash != current_content_hash) {

std::vector<std::string> CookCache::probe_stale_upstream_hashes(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return {};
CookCachePruneEstimate CookCache::estimate_prune_all() const {
    CookCachePruneEstimate estimate;
        return estimate;

        if (!is_valid_cook_cache_entry(entry)) {
            ++estimate.invalid_count;
        } else if (is_stale_cache_entry_(entry)) {
            ++estimate.stale_count;

u32 CookCache::estimate_prunable_entries() const {
    return estimate_prune_all().total();

CookCacheInvalidationProbe CookCache::probe_stale_content_for_source(const std::string& source_path,
                                                                     u64 current_content_hash) const {
    CookCacheInvalidationProbe probe;
        return probe;

            ++probe.affected_count;

CookCacheInvalidationProbe CookCache::probe_stale_upstream_hashes(

u32 CookCache::estimate_invalidation_by_hash(u64 content_hash) const {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
    return find_entry_(content_hash) != nullptr ? 1u : 0u;

u32 CookCache::estimate_invalidation_by_source(const std::string& source_path) const {


u32 CookCache::estimate_invalidation_by_output(const std::string& output_path) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {

        if (entry.output_path == output_path) {

u32 CookCache::estimate_stale_content_invalidation(const std::string& source_path,

        if (entry.source_path != source_path) {
            continue;
        if (entry.content_hash != current_content_hash) {

u32 CookCache::estimate_stale_upstream_invalidation(

    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
        const u64 current_upstream = pair.second;

            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {

std::vector<std::string> CookCache::probe_stale_upstream_source_paths(
        }

        for (const CookCacheEntry& entry : m_entries) {

std::vector<std::string> CookCache::probe_stale_upstream_invalidation_sources(

    std::vector<std::string> stale_sources;

                stale_sources.push_back(source_path);
                break;
    return stale_sources;

u32 CookCache::probe_stale_upstream_hash_entries(



u32 CookCache::probe_downstream_of(const std::string& output_path,
                                   const std::vector<CookJobDependencyEdge>& edges,
                                   const std::vector<CookJob>& jobs) const {

    u32 count = probe_invalidate_source(output_path);


CookCacheInvalidationProbe CookCache::probe_invalidate_source(const std::string& source_path) const {


CookCacheInvalidationProbe CookCache::probe_invalidate_output(const std::string& output_path) const {


CookCacheInvalidationProbe CookCache::probe_downstream_of(const std::string& output_path,

    CookCacheInvalidationProbe probe = probe_invalidate_source(output_path);


u32 CookCache::estimate_invalidation_downstream_of(const std::string& output_path,

    u32 estimate = estimate_invalidation_by_source(output_path);
        if (is_stale_cache_entry_(entry)) {
    return false;

    if (m_entries.empty()) {


CookCacheReconcileEstimate CookCache::estimate_reconcile() const {
    CookCacheReconcileEstimate estimate;

        if (is_invalid_cook_cache_entry(entry)) {
            ++estimate.invalid_entry_count;
            ++estimate.stale_entry_count;

CookCacheStorePreflight CookCache::preflight_store(const CookCacheEntry& entry) const {
    return preflight_cook_cache_store(entry);

CookCacheLookupPreflight CookCache::preflight_lookup(u64 content_hash) const {
    CookCacheLookupPreflight preflight;
    if (!is_valid_cook_cache_key(content_hash)) {
        preflight.zero_content_hash = true;
        return preflight;

    preflight.cache_empty = m_entries.empty();
    if (!preflight.cache_empty) {
        preflight.entry_present = find_entry_(content_hash) != nullptr;

CookCacheInvalidationProbe CookCache::probe_invalidate(u64 content_hash) const {
    probe.cache_empty = m_entries.empty();
    if (!is_valid_cook_cache_key(content_hash) || probe.cache_empty) {
        probe.invalid_args = !is_valid_cook_cache_key(content_hash);

    if (find_entry_(content_hash) != nullptr) {
        probe.would_invalidate_count = 1;

    if (!is_valid_cook_cache_path(source_path) || probe.cache_empty) {
        probe.invalid_args = !is_valid_cook_cache_path(source_path);

            ++probe.would_invalidate_count;

CookCacheInvalidationProbe CookCache::probe_invalidate_stale_content_for_source(
    const std::string& source_path, u64 current_content_hash) const {
        probe.cache_empty) {
        probe.invalid_args = !is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash);


    if (!is_valid_cook_cache_path(output_path) || probe.cache_empty) {
        probe.invalid_args = !is_valid_cook_cache_path(output_path);


std::vector<std::string> CookCache::probe_invalidate_stale_upstream_hashes(

    std::vector<std::string> would_invalidate;

                would_invalidate.push_back(source_path);
    return would_invalidate;

CookCacheInvalidationProbe CookCache::probe_invalidate_downstream_of(
    const std::string& output_path,

    probe.would_invalidate_count = probe_invalidate_source(output_path).would_invalidate_count;

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {

        count += probe_invalidate_source(to_job->source_path);
        count += probe_downstream_of(to_job->output_path, edges, jobs);


CookCacheInvalidationProbe CookCache::probe_upstream_invalidation(
    const std::string& changed_source,
    if (!is_valid_cook_cache_path(changed_source) || m_entries.empty()) {

    probe.direct_entries = probe_invalidate_source(changed_source);
    for (const CookJob& job : jobs) {
        if (job.source_path == changed_source) {
            probe.downstream_entries += probe_downstream_of(job.output_path, edges, jobs);


        probe.affected_count += probe_invalidate_source(to_job->source_path).affected_count;
        const CookCacheInvalidationProbe downstream =
            probe_downstream_of(to_job->output_path, edges, jobs);
        probe.affected_count += downstream.affected_count;



        estimate += estimate_invalidation_by_source(to_job->source_path);
        estimate += estimate_invalidation_downstream_of(to_job->output_path, edges, jobs);


bool CookCache::probe_would_invalidate_hash(u64 content_hash) const {
    return estimate_invalidation_by_hash(content_hash) > 0;

bool CookCache::probe_would_invalidate_source(const std::string& source_path) const {
    return estimate_invalidation_by_source(source_path) > 0;

u32 CookCache::estimate_prune_invalid_entries() const {


u32 CookCache::estimate_prune_stale_entries() const {

        if (is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry)) {

u32 CookCache::estimate_prune_all() const {


    estimate.invalid_entries = estimate_prune_invalid_entries();
    estimate.stale_entries = estimate_prune_stale_entries();
    estimate.prunable_entries = estimate_prune_all();


        probe.would_invalidate_count += probe_invalidate_source(to_job->source_path).would_invalidate_count;
        probe.would_invalidate_count +=
            probe_invalidate_downstream_of(to_job->output_path, edges, jobs).would_invalidate_count;


u32 CookCache::count_stale_entries() const {


u32 CookCache::count_invalid_entries() const {

            ++estimate.invalid_entries;
            ++estimate.stale_entries;
}

u32 CookCache::count_stale_upstream_entries(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                ++count;
            }
        }
    }
    return count;
}

u32 CookCache::probe_stale_content_for_source(const std::string& source_path, u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {
u32 CookCache::count_downstream_entries(const std::string& output_path,
                                         const std::vector<CookJobDependencyEdge>& edges,
                                         const std::vector<CookJob>& jobs) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path != source_path) {
            continue;
        }
        if (entry.content_hash != current_content_hash) {
            ++count;
    return count;

u32 CookCache::estimate_prune_all() const {
    if (m_entries.empty() || !has_prunable_entries()) {
        return 0;
    return count_invalid_entries() + count_stale_entries();
        if (entry.source_path == output_path) {

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == to_job->source_path) {
        count += count_downstream_entries(to_job->output_path, edges, jobs);

    return estimate;
}

bool CookCache::probe_would_invalidate_hash(u64 content_hash) const {
    return estimate_invalidation_by_hash(content_hash) > 0;

bool CookCache::probe_would_invalidate_source(const std::string& source_path) const {
    return estimate_invalidation_by_source(source_path) > 0;

bool CookCache::probe_would_invalidate_output(const std::string& output_path) const {
    return estimate_invalidation_by_output(output_path) > 0;

bool CookCache::probe_would_invalidate_stale_content(const std::string& source_path,
                                                     u64 current_content_hash) const {
    return estimate_stale_content_invalidation(source_path, current_content_hash) > 0;

u32 CookCache::estimate_prune_invalid_entries() const {
    if (m_entries.empty()) {
        return 0;

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (!is_valid_cook_cache_entry(entry)) {
            ++count;
    return count;

u32 CookCache::estimate_prune_stale_entries() const {

        if (is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry)) {

u32 CookCache::estimate_prune_all() const {

        if (is_prunable_cache_entry_(entry)) {

CookCacheReconcileEstimate CookCache::estimate_reconcile() const {
    CookCacheReconcileEstimate estimate;
    estimate.invalid_entries = estimate_prune_invalid_entries();
    estimate.stale_entries = estimate_prune_stale_entries();
    estimate.prunable_entries = estimate_prune_all();
}

CookCacheInvalidationProbe CookCache::probe_invalidate(u64 content_hash) const {
    CookCacheInvalidationProbe probe;
    probe.empty_cache = m_entries.empty();
    probe.zero_hash = !is_valid_cook_cache_key(content_hash);
    if (probe.empty_cache || probe.zero_hash) {
        return probe;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (entry.content_hash == content_hash) {
            ++probe.affected_entries;
            break;
        }
    }
    return probe;
}

CookCacheInvalidationProbe CookCache::probe_invalidate_source(const std::string& source_path) const {
    CookCacheInvalidationProbe probe;
    probe.empty_cache = m_entries.empty();
    probe.empty_path = !is_valid_cook_cache_path(source_path);
    if (probe.empty_cache || probe.empty_path) {
        return probe;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path == source_path) {
            ++probe.affected_entries;
        }
    }
    return probe;
}

CookCacheInvalidationProbe CookCache::probe_invalidate_output(const std::string& output_path) const {
    CookCacheInvalidationProbe probe;
    probe.empty_cache = m_entries.empty();
    probe.empty_path = !is_valid_cook_cache_path(output_path);
    if (probe.empty_cache || probe.empty_path) {
        return probe;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (entry.output_path == output_path) {
            ++probe.affected_entries;
        }
    }
    return probe;
}

CookCacheInvalidationProbe CookCache::probe_invalidate_stale_content_for_source(
    const std::string& source_path, u64 current_content_hash) const {
    CookCacheInvalidationProbe probe;
    probe.empty_cache = m_entries.empty();
    probe.empty_path = !is_valid_cook_cache_path(source_path);
    probe.zero_hash = !is_valid_cook_cache_key(current_content_hash);
    if (probe.empty_cache || probe.empty_path || probe.zero_hash) {
        return probe;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path == source_path && entry.content_hash != current_content_hash) {
            ++probe.affected_entries;
        }
    }
    return probe;
}

CookCacheReconcileEstimate CookCache::estimate_prune_stale_entries() const {
    CookCacheReconcileEstimate estimate;
    estimate.empty_cache = m_entries.empty();
    if (estimate.empty_cache) {
        return estimate;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (is_stale_cache_entry_(entry)) {
            ++estimate.stale_entries;
        }
    }
    estimate.total_removable = estimate.stale_entries;
    return estimate;
}

CookCacheReconcileEstimate CookCache::estimate_prune_invalid_entries() const {
    CookCacheReconcileEstimate estimate;
    estimate.empty_cache = m_entries.empty();
    if (estimate.empty_cache) {
        return estimate;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (!is_valid_cook_cache_entry(entry)) {
            ++estimate.invalid_entries;
        }
    }
    estimate.total_removable = estimate.invalid_entries;
    return estimate;
}

CookCacheReconcileEstimate CookCache::estimate_prune_all() const {
    CookCacheReconcileEstimate estimate;
    estimate.empty_cache = m_entries.empty();
    if (estimate.empty_cache || !has_prunable_entries()) {
        return estimate;
    }

    for (const CookCacheEntry& entry : m_entries) {
        if (!is_valid_cook_cache_entry(entry)) {
            ++estimate.invalid_entries;
        } else if (is_stale_cache_entry_(entry)) {
            ++estimate.stale_entries;
        }
    }
    estimate.total_removable = estimate.invalid_entries + estimate.stale_entries;
    return estimate;
}

CookCacheReconcileEstimate CookCache::estimate_stale_upstream_invalidations(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    CookCacheReconcileEstimate estimate;
    estimate.empty_cache = m_entries.empty();
    if (estimate.empty_cache || source_upstream_by_path.empty()) {
        return estimate;
    }

    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                ++estimate.upstream_stale_entries;
            }
        }
    }
    estimate.total_removable = estimate.upstream_stale_entries;
    return estimate;
}

u32 CookCache::estimate_invalidation_by_hash(u64 content_hash) const {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
        return 0;
    }
    return find_entry_(content_hash) != nullptr ? 1u : 0u;
}

u32 CookCache::estimate_invalidation_by_source(const std::string& source_path) const {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path == source_path) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_invalidation_by_output(const std::string& output_path) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.output_path == output_path) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_stale_content_invalidation(const std::string& source_path,
                                                   u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path != source_path) {
            continue;
        }
        if (entry.content_hash != current_content_hash) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_stale_upstream_invalidation(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                ++count;
            }
        }
    }
    return count;
}

std::vector<std::string> CookCache::probe_stale_upstream_invalidation_sources(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return {};
    }

    std::vector<std::string> stale_sources;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                stale_sources.push_back(source_path);
            }
        }
    }
    return stale_sources;
}

u32 CookCache::estimate_invalidation_downstream_of(const std::string& output_path,
                                                 const std::vector<CookJobDependencyEdge>& edges,
                                                 const std::vector<CookJob>& jobs) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
std::vector<std::string> CookCache::probe_unique_stale_upstream_sources(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    const std::vector<std::string> stale_sources = probe_stale_upstream_sources(source_upstream_by_path);
    if (stale_sources.empty()) {
        return {};
    }

    std::vector<std::string> unique_sources;
    unique_sources.reserve(stale_sources.size());
    for (const std::string& source_path : stale_sources) {
        if (std::find(unique_sources.begin(), unique_sources.end(), source_path) == unique_sources.end()) {
            unique_sources.push_back(source_path);
    return unique_sources;

u32 CookCache::count_stale_entries() const {
    if (m_entries.empty()) {
        return 0;

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry)) {
            ++count;
    return count;

namespace {

u32 count_downstream_of_(const CookCache& cache,
                         const std::string& output_path,
                         const std::vector<CookJob>& jobs) {
    if (!is_valid_cook_cache_path(output_path) || cache.empty()) {
        return 0;
    }

    u32 estimate = estimate_invalidation_by_source(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {
            continue;
        }

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {
            continue;
        }

        estimate += estimate_invalidation_by_source(to_job->source_path);
        estimate += estimate_invalidation_downstream_of(to_job->output_path, edges, jobs);
    }

    return estimate;
}

bool CookCache::probe_would_invalidate_hash(u64 content_hash) const {
    return estimate_invalidation_by_hash(content_hash) > 0;
}

bool CookCache::probe_would_invalidate_source(const std::string& source_path) const {
    return estimate_invalidation_by_source(source_path) > 0;
}

u32 CookCache::estimate_prune_invalid_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (!is_valid_cook_cache_entry(entry)) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_prune_stale_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry)) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_prune_all() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_prunable_cache_entry_(entry)) {
            ++count;
        }
    }
    return count;
}

CookCacheReconcileEstimate CookCache::estimate_reconcile() const {
    CookCacheReconcileEstimate estimate;
    estimate.invalid_entries = estimate_prune_invalid_entries();
    estimate.stale_entries = estimate_prune_stale_entries();
    estimate.prunable_entries = estimate_prune_all();
    return estimate;
}

u32 CookCache::estimate_invalidation_by_hash(u64 content_hash) const {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
        return 0;
    }
    return find_entry_(content_hash) != nullptr ? 1u : 0u;
}

u32 CookCache::estimate_invalidation_by_source(const std::string& source_path) const {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path == source_path) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_invalidation_by_output(const std::string& output_path) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.output_path == output_path) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_stale_content_invalidation(const std::string& source_path,
                                                   u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (entry.source_path != source_path) {
            continue;
        }
        if (entry.content_hash != current_content_hash) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_stale_upstream_invalidation(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                ++count;
            }
        }
    }
    return count;
}

std::vector<std::string> CookCache::probe_stale_upstream_invalidation_sources(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {
        return {};
    }

    std::vector<std::string> stale_sources;
    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
            continue;
        }
        const u64 current_upstream = pair.second;

        for (const CookCacheEntry& entry : m_entries) {
            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {
                stale_sources.push_back(source_path);
            }
        }
    }
    return stale_sources;
}

u32 CookCache::estimate_invalidation_downstream_of(const std::string& output_path,
                                                 const std::vector<CookJobDependencyEdge>& edges,
                                                 const std::vector<CookJob>& jobs) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {
        return 0;
    }

    u32 estimate = estimate_invalidation_by_source(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {
            continue;
        }

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {
            continue;
        }

        estimate += estimate_invalidation_by_source(to_job->source_path);
        estimate += estimate_invalidation_downstream_of(to_job->output_path, edges, jobs);
    }

    return estimate;
}

bool CookCache::probe_would_invalidate_hash(u64 content_hash) const {
    return estimate_invalidation_by_hash(content_hash) > 0;
}

bool CookCache::probe_would_invalidate_source(const std::string& source_path) const {
    return estimate_invalidation_by_source(source_path) > 0;
}

u32 CookCache::estimate_prune_invalid_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (!is_valid_cook_cache_entry(entry)) {
            ++count;
        }
    }
    return count;
}

u32 CookCache::estimate_prune_stale_entries() const {
u32 CookCache::count_stale_entries() const {
    if (m_entries.empty()) {
        return 0;
    }

    u32 count = 0;
    for (const CookCacheEntry& entry : m_entries) {
        if (is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry)) {
            ++count;
    return count;

u32 CookCache::estimate_prune_all() const {

        if (is_prunable_cache_entry_(entry)) {

bool CookCache::would_invalidate_source(const std::string& source_path) const {
    return count_by_source(source_path) > 0;

bool CookCache::would_invalidate_output(const std::string& output_path) const {
    return count_by_output(output_path) > 0;
        }



bool CookCache::would_invalidate_stale_content_for_source(const std::string& source_path,
                                                          u64 current_content_hash) const {
    return count_stale_content_for_source(source_path, current_content_hash) > 0;

u32 CookCache::estimate_prune_invalid_entries() const {
    return count_invalid_entries();

    return count_stale_entries();

    if (m_entries.empty() || !has_prunable_entries()) {
    return count_prunable_entries();


u32 CookCache::estimate_prune_stale_entries() const {

        return 0;

CookCacheReconcileEstimate CookCache::estimate_reconcile() const {
    CookCacheReconcileEstimate estimate;
    estimate.invalid_entries = estimate_prune_invalid_entries();
    estimate.stale_entries = estimate_prune_stale_entries();
    estimate.prunable_entries = estimate_prune_all();
    return estimate;

u32 CookCache::estimate_invalidation_by_hash(u64 content_hash) const {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
    return find_entry_(content_hash) != nullptr ? 1u : 0u;

u32 CookCache::estimate_invalidation_by_source(const std::string& source_path) const {
    if (!is_valid_cook_cache_path(source_path) || m_entries.empty()) {

        if (entry.source_path == source_path) {

u32 CookCache::estimate_invalidation_by_output(const std::string& output_path) const {
    if (!is_valid_cook_cache_path(output_path) || m_entries.empty()) {

        if (entry.output_path == output_path) {

u32 CookCache::estimate_stale_content_invalidation(const std::string& source_path,
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) ||
        m_entries.empty()) {

        if (entry.source_path != source_path) {
            continue;
        if (entry.content_hash != current_content_hash) {

u32 CookCache::estimate_stale_upstream_invalidation(
    const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const {
    if (m_entries.empty() || source_upstream_by_path.empty()) {

    for (const auto& pair : source_upstream_by_path) {
        const std::string& source_path = pair.first;
        if (!is_valid_cook_cache_path(source_path)) {
        const u64 current_upstream = pair.second;

            if (entry.source_path == source_path && entry.upstream_hash != current_upstream) {

std::vector<std::string> CookCache::probe_stale_upstream_invalidation_sources(
        return {};

    std::vector<std::string> stale_sources;

                stale_sources.push_back(source_path);
    return stale_sources;

u32 CookCache::estimate_invalidation_downstream_of(const std::string& output_path,
                                                 const std::vector<CookJobDependencyEdge>& edges,
                                                 const std::vector<CookJob>& jobs) const {

    u32 estimate = estimate_invalidation_by_source(output_path);

    for (const CookJobDependencyEdge& edge : edges) {
        const CookJob* from_job = find_job_by_id(jobs, edge.from_job_id);
        if (!from_job || from_job->output_path != output_path) {

        const CookJob* to_job = find_job_by_id(jobs, edge.to_job_id);
        if (!to_job) {

        estimate += estimate_invalidation_by_source(to_job->source_path);
        estimate += estimate_invalidation_downstream_of(to_job->output_path, edges, jobs);


bool CookCache::probe_would_invalidate_hash(u64 content_hash) const {
    return estimate_invalidation_by_hash(content_hash) > 0;

bool CookCache::probe_would_invalidate_source(const std::string& source_path) const {
    return estimate_invalidation_by_source(source_path) > 0;


        if (!is_valid_cook_cache_entry(entry)) {


































u32 CookCache::probe_stale_content_invalidation(const std::string& source_path, u64 current_content_hash) const {
    if (!is_valid_cook_cache_path(source_path) || !is_valid_cook_cache_key(current_content_hash) || m_entries.empty()) {

    u32 matches = 0;
        if (entry.source_path == source_path && entry.content_hash != current_content_hash) {
            ++matches;
    return matches;

std::vector<std::string> CookCache::probe_stale_upstream_invalidation(



u32 CookCache::probe_stale_upstream_invalidation_count(
    return static_cast<u32>(probe_stale_upstream_invalidation(source_upstream_by_path).size());

u32 CookCache::probe_invalidate_downstream_of(const std::string& output_path,

        if (entry.source_path == output_path) {



bool CookCache::would_invalidate_stale_upstream_hashes(
    return count_stale_upstream_hashes(source_upstream_by_path) > 0;

bool CookCache::would_invalidate_downstream_of(const std::string& output_path,
    return count_downstream_of(output_path, edges, jobs) > 0;

namespace {

void collect_downstream_sources_(const CookCache& cache,
                                 const std::string& output_path,
                                 const std::vector<CookJob>& jobs,
                                 std::vector<std::string>& out_sources) {
    if (!is_valid_cook_cache_path(output_path) || cache.empty()) {
        return;

    const u32 source_count = cache.count_by_source(output_path);
    for (u32 i = 0; i < source_count; ++i) {
        out_sources.push_back(output_path);



            if (entry.source_path == to_job->source_path) {
        matches += probe_invalidate_downstream_of(to_job->output_path, edges, jobs);






    return estimate_prune_invalid_entries() + estimate_prune_stale_entries();


    estimate.invalid_count = estimate_prune_invalid_entries();
    estimate.stale_count = estimate_prune_stale_entries();
























































        const u32 dependent_count = cache.count_by_source(to_job->source_path);
        for (u32 i = 0; i < dependent_count; ++i) {
            out_sources.push_back(to_job->source_path);
        collect_downstream_sources_(cache, to_job->output_path, edges, jobs, out_sources);

} // namespace

std::vector<std::string> CookCache::probe_downstream_sources(
    const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
    std::vector<std::string> sources;
    collect_downstream_sources_(*this, output_path, edges, jobs, sources);
    return sources;
u32 CookCache::count_prune_all() const {


u32 CookCache::estimate_prune_removals() const {

std::vector<std::string> CookCache::probe_stale_content_sources() const {

        if (!is_valid_cook_cache_entry(entry) || !is_stale_cache_entry_(entry)) {

        bool already_recorded = false;
        for (const std::string& recorded : stale_sources) {
            if (recorded == entry.source_path) {
                already_recorded = true;
                break;
        if (!already_recorded) {
            stale_sources.push_back(entry.source_path);


        if (is_stale_only_cache_entry_(entry)) {





        if (is_stale_cache_entry_(entry)) {
u32 CookCache::probe_invalidate_source(const std::string& source_path) const {
    return count_by_source(source_path);

u32 CookCache::probe_stale_content_for_source(const std::string& source_path,
    return count_stale_content_for_source(source_path, current_content_hash);

std::vector<std::string> CookCache::probe_stale_upstream_hashes(



u32 CookCache::probe_stale_upstream_hash_entries(
    return count_stale_upstream_hashes(source_upstream_by_path);

u32 CookCache::probe_downstream_of(const std::string& output_path,
    return count_downstream_of(output_path, edges, jobs);

CookCacheInvalidationProbe CookCache::probe_upstream_invalidation(
    const std::string& changed_source,
    CookCacheInvalidationProbe probe;
    if (!is_valid_cook_cache_path(changed_source) || m_entries.empty()) {
        return probe;

    probe.direct_entries = probe_invalidate_source(changed_source);
    for (const CookJob& job : jobs) {
        if (job.source_path == changed_source) {
            probe.downstream_entries += probe_downstream_of(job.output_path, edges, jobs);

CookCachePruneEstimate CookCache::estimate_prune_removals() const {
    CookCachePruneEstimate estimate;

            ++estimate.invalid_entries;
        } else if (is_stale_cache_entry_(entry)) {
            ++estimate.stale_entries;

    if (m_entries.empty()) {
    return count_invalid_entries() + count_stale_entries();




    for (const CookCacheEntry& entry : m_entries) {
        if (is_valid_cook_cache_entry(entry) && is_stale_cache_entry_(entry)) {
}

bool CookCache::contains(u64 content_hash) const {
    if (!is_valid_cook_cache_key(content_hash) || m_entries.empty()) {
        return false;
    }
    return find_entry_(content_hash) != nullptr;
}

void CookCache::clear() {
    if (m_entries.empty()) {
        return;
    }

    m_entries.clear();
    m_stats = {};
}

bool CookCache::save(const std::string& path) const {
    if (path.empty()) {
        return false;
    }

    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"entries\": [\n";

    for (usize i = 0; i < m_entries.size(); ++i) {
        const CookCacheEntry& entry = m_entries[i];
        out << "    {\n";
        out << "      \"contentHash\": " << entry.content_hash << ",\n";
        out << "      \"upstreamHash\": " << entry.upstream_hash << ",\n";
        out << "      \"outputPath\": \"" << escapeJson(entry.output_path) << "\",\n";
        out << "      \"sourcePath\": \"" << escapeJson(entry.source_path) << "\",\n";
        out << "      \"kind\": \"" << cookAssetKindName(entry.kind) << "\"\n";
        out << "    }";
        if (i + 1 < m_entries.size()) {
            out << ',';
        }
        out << '\n';
    }

    out << "  ]\n}\n";

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << out.str();
    return static_cast<bool>(file);
}

bool CookCache::load(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string contents = buffer.str();

    if (contents.find("\"schemaVersion\"") == std::string::npos) {
        return false;
    }

    std::vector<CookCacheEntry> parsed;
    std::size_t cursor = 0;
    while (cursor < contents.size()) {
        const std::size_t objectStart = contents.find('{', cursor);
        if (objectStart == std::string::npos) {
            break;
        }

        std::size_t depth = 0;
        std::size_t objectEnd = objectStart;
        for (std::size_t i = objectStart; i < contents.size(); ++i) {
            if (contents[i] == '{') {
                ++depth;
            } else if (contents[i] == '}') {
                --depth;
                if (depth == 0) {
                    objectEnd = i;
                    break;
                }
            }
        }

        const std::string_view objectBody(contents.data() + objectStart, objectEnd - objectStart + 1);
        if (objectBody.find("\"contentHash\"") == std::string_view::npos) {
            cursor = objectEnd + 1;
            continue;
        }

        CookCacheEntry entry;
        const auto readStringField = [&](std::string_view key, std::string& value) {
            const std::string needle = std::string("\"") + std::string(key) + "\":";
            const std::size_t keyPos = objectBody.find(needle);
            if (keyPos == std::string_view::npos) {
                return;
            }
            std::size_t fieldCursor = keyPos + needle.size();
            while (fieldCursor < objectBody.size() && objectBody[fieldCursor] != '"') {
                ++fieldCursor;
            }
            if (fieldCursor >= objectBody.size()) {
                return;
            }
            ++fieldCursor;
            std::size_t end = fieldCursor;
            while (end < objectBody.size() && objectBody[end] != '"') {
                ++end;
            }
            value = std::string(objectBody.substr(fieldCursor, end - fieldCursor));
        };

        const std::string hashNeedle = "\"contentHash\":";
        const std::size_t hashPos = objectBody.find(hashNeedle);
        if (hashPos != std::string_view::npos) {
            std::size_t hashCursor = hashPos + hashNeedle.size();
            while (hashCursor < objectBody.size() && !std::isdigit(static_cast<unsigned char>(objectBody[hashCursor]))) {
                ++hashCursor;
            }
            entry.content_hash = 0;
            while (hashCursor < objectBody.size() && std::isdigit(static_cast<unsigned char>(objectBody[hashCursor]))) {
                entry.content_hash = entry.content_hash * 10 +
                                     static_cast<u64>(objectBody[hashCursor] - '0');
                ++hashCursor;
            }
        }

        const std::string upstreamNeedle = "\"upstreamHash\":";
        const std::size_t upstreamPos = objectBody.find(upstreamNeedle);
        if (upstreamPos != std::string_view::npos) {
            std::size_t upstreamCursor = upstreamPos + upstreamNeedle.size();
            while (upstreamCursor < objectBody.size() &&
                   !std::isdigit(static_cast<unsigned char>(objectBody[upstreamCursor]))) {
                ++upstreamCursor;
            }
            entry.upstream_hash = 0;
            while (upstreamCursor < objectBody.size() &&
                   std::isdigit(static_cast<unsigned char>(objectBody[upstreamCursor]))) {
                entry.upstream_hash = entry.upstream_hash * 10 +
                                      static_cast<u64>(objectBody[upstreamCursor] - '0');
                ++upstreamCursor;
            }
        }

        readStringField("outputPath", entry.output_path);
        readStringField("sourcePath", entry.source_path);

        std::string kindText;
        readStringField("kind", kindText);
        if (kindText == "mesh") {
            entry.kind = CookAssetKind::Mesh;
        } else if (kindText == "texture") {
            entry.kind = CookAssetKind::Texture;
        } else if (kindText == "audio") {
            entry.kind = CookAssetKind::Audio;
        } else if (kindText == "shader") {
            entry.kind = CookAssetKind::Shader;
        }

        if (is_valid_cook_cache_entry(entry)) {
            parsed.push_back(std::move(entry));
        }

        cursor = objectEnd + 1;
    }

    prune_stale_entries();
    m_entries = std::move(parsed);
    return true;
}

} // namespace fuse::project
