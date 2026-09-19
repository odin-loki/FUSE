#pragma once

#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/types.hpp>

#include <string>
#include <utility>
#include <vector>

namespace fuse::project {

enum class CookCacheLookup : u8 {
    Hit,
    Miss,
};

struct CookCacheEntry {
    u64 content_hash = 0;
    u64 upstream_hash = 0;
    std::string output_path;
    std::string source_path;
    CookAssetKind kind = CookAssetKind::Mesh;
};

struct CookCacheStats {
    u64 hits = 0;
    u64 misses = 0;
    u64 invalidations = 0;
};

/// Non-mutating probe for incremental invalidation — counts entries a call would remove (B7.9 deepen follow-up).
struct CookCacheInvalidationProbe {
    u32 affected_entries = 0;
    bool empty_cache = true;
    bool empty_path = false;
    bool zero_hash = false;

    [[nodiscard]] bool would_invalidate() const { return affected_entries > 0; }
    [[nodiscard]] bool should_skip() const { return !would_invalidate(); }
};

/// Non-mutating reconcile estimate — counts prunable or stale-upstream entries (B7.9 deepen follow-up).
struct CookCacheReconcileEstimate {
    u32 stale_entries = 0;
    u32 invalid_entries = 0;
    u32 upstream_stale_entries = 0;
    u32 total_removable = 0;
    bool empty_cache = true;

    [[nodiscard]] bool would_reconcile() const { return total_removable > 0; }
    [[nodiscard]] bool should_skip() const { return !would_reconcile(); }
};

/// Zero is reserved — empty or unreadable source keys must not enter the cache.
[[nodiscard]] inline bool is_valid_cook_cache_key(u64 content_hash) {
    return content_hash != 0;
}

/// Non-empty filesystem paths are required for cache entries (B7.9 deepen).
[[nodiscard]] inline bool is_valid_cook_cache_path(const std::string& path) {
    return !path.empty();
}

/// Structural validity for cache records — key plus both paths (B7.9 deepen).
[[nodiscard]] inline bool is_valid_cook_cache_entry(const CookCacheEntry& entry) {
    return is_valid_cook_cache_key(entry.content_hash) && is_valid_cook_cache_path(entry.source_path) &&
           is_valid_cook_cache_path(entry.output_path);
}

/// Combined source/upstream fold is cacheable when non-zero (B7.9 deepen).
[[nodiscard]] inline bool is_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return is_valid_cook_cache_key(combine_cook_cache_key(source_hash, upstream_hash));
}

/// Content-hashed cook output cache — identical source+desc hashes return cached records (B7.9 deepen stub).
class CookCache {
public:
    CookCacheLookup lookup(u64 content_hash, CookCacheEntry* out_entry = nullptr);
    void store(const CookCacheEntry& entry);

    bool invalidate(u64 content_hash);
    u32 invalidate_source(const std::string& source_path);
    /// Drop entries for `source_path` whose stored hash differs from `current_content_hash` (B7.9 deepen).
    u32 invalidate_stale_content_for_source(const std::string& source_path, u64 current_content_hash);
    /// Drop entries whose cooked output path matches `output_path` (B7.9 deepen).
    u32 invalidate_output(const std::string& output_path);
    /// Drop entries whose stored upstream hash differs from the freshly computed value (B7.9 deepen).
    /// Returns source paths that were invalidated.
    std::vector<std::string> invalidate_stale_upstream_hashes(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path);
    /// Remove entries sourced from `output_path` and transitively invalidate dependents (B7.9 deepen).
    u32 invalidate_downstream_of(const std::string& output_path,
                                 const std::vector<CookJobDependencyEdge>& edges,
                                 const std::vector<CookJob>& jobs);
    void invalidate_all();

    /// Recompute content keys from stored paths/kinds and drop entries whose source changed (B7.9 deepen).
    u32 prune_stale_entries();
    /// Drop entries with zero keys or empty paths without touching hit/miss stats (B7.9 deepen).
    u32 prune_invalid_entries();
    /// Run invalid-entry then stale-entry pruning — no-op on empty cache (B7.9 deepen).
    u32 prune_all();
    /// True when invalid or stale records are present — `prune_*` would remove at least one (B7.9 deepen).
    [[nodiscard]] bool has_prunable_entries() const;

    /// Incremental invalidation probes — non-mutating entry counts (B7.9 deepen follow-up).
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate(u64 content_hash) const;
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_stale_content_for_source(
        const std::string& source_path, u64 current_content_hash) const;

    /// Reconcile estimators — non-mutating prune and upstream-hash counts (B7.9 deepen follow-up).
    [[nodiscard]] CookCacheReconcileEstimate estimate_prune_stale_entries() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_prune_invalid_entries() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_prune_all() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_stale_upstream_invalidations(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;

    [[nodiscard]] bool contains(u64 content_hash) const;

    void clear();
    [[nodiscard]] bool empty() const { return m_entries.empty(); }
    [[nodiscard]] const CookCacheStats& stats() const { return m_stats; }
    [[nodiscard]] usize entry_count() const { return m_entries.size(); }

    bool save(const std::string& path) const;
    bool load(const std::string& path);

private:
    [[nodiscard]] CookCacheEntry* find_entry_(u64 content_hash);
    [[nodiscard]] const CookCacheEntry* find_entry_(u64 content_hash) const;

    std::vector<CookCacheEntry> m_entries;
    CookCacheStats m_stats;
};

} // namespace fuse::project
