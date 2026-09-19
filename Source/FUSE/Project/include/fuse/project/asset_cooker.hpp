#pragma once

#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

namespace fuse::project {

/// Read-only reconcile planning breakdown for cache + dependency invalidation (B7.9 deepen).
struct CookCacheReconcileEstimate {
    u32 stale_dependency_entries = 0;
    u32 prune_invalid_entries = 0;
    u32 prune_stale_entries = 0;

    [[nodiscard]] u32 total() const {
        return stale_dependency_entries + prune_invalid_entries + prune_stale_entries;
    }
    /// True when no reconcile invalidation is estimated — mirrors `total() == 0` (B7.9 deepen).
    [[nodiscard]] bool should_skip() const { return total() == 0; }
};

/// Read-only upstream invalidation breakdown for a changed source (B7.9 deepen).
struct CookCacheUpstreamReconcileEstimate {
    u32 direct_source_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return direct_source_entries + downstream_entries; }
};

/// Offline asset cooker — mesh/texture/audio transforms (B7.9 stub; no runtime link).
class AssetCooker {
public:
    CookRecord cook_mesh(const MeshImportDesc& desc);
    CookRecord cook_texture(const TextureImportDesc& desc);
    CookRecord cook_audio(const AudioImportDesc& desc);

    CookRecord cook_entry(const CookManifestEntry& entry);
    CookRecord cook_entry(const CookManifestEntry& entry, const CookManifest& manifest);
    CookBatchResult cook_manifest(const CookManifest& manifest);
    CookJobGraphExecuteResult cook_manifest_graph(const CookManifest& manifest);
    CookBatchResult cook_dirty(AssetGraph& graph, const std::string& project_dir);

    /// Invalidate cache entries for `changed_source` and all manifest dependents (B7.9 deepen).
    u32 invalidate_upstream_dependency(const CookManifest& manifest, const std::string& changed_source);
    /// Reconcile cache with current upstream dependency hashes via the cook job graph (B7.9 deepen).
    u32 invalidate_stale_dependency_hashes(const CookManifest& manifest);
    /// Drop invalid and stale cache records — no-op when cache is empty or clean (B7.9 deepen).
    u32 prune_stale_cache();

    /// Read-only upstream invalidation probe — guarded on empty `changed_source` (B7.9 deepen).
    [[nodiscard]] u32 count_upstream_invalidation(const CookManifest& manifest,
                                                  const std::string& changed_source) const;
    /// True when `invalidate_upstream_dependency` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_upstream_dependency(const CookManifest& manifest,
    /// Read-only stale dependency-hash reconcile probe (B7.9 deepen).
    [[nodiscard]] u32 count_stale_dependency_invalidation(const CookManifest& manifest) const;
    /// True when `invalidate_stale_dependency_hashes` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_stale_dependency_hashes(const CookManifest& manifest) const;
    /// Read-only prune reconcile probe — mirrors `CookCache::estimate_prune_removals` (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_reconcile() const;
    /// Combined dependency + prune reconcile estimator for incremental invalidation planning (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_reconcile_invalidation(
        const CookManifest& manifest) const;
    /// Upstream invalidation breakdown for `changed_source` — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamReconcileEstimate estimate_upstream_reconcile(
        const CookManifest& manifest, const std::string& changed_source) const;
    /// True when `estimate_reconcile_invalidation(manifest).total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_reconcile_invalidation(const CookManifest& manifest) const;
    /// Deduplicated source paths `invalidate_upstream_dependency` would touch (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_upstream_invalidation_sources(

    /// Non-mutating upstream invalidation scope probe via the cook job graph (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationProbe probe_upstream_dependency(const CookManifest& manifest,
    /// Non-mutating reconcile estimate for stale upstream dependency hashes (B7.9 deepen).
    struct CookDependencyReconcileEstimate {
        u32 stale_upstream_entries = 0;
        u32 downstream_cascade_entries = 0;
        std::vector<std::string> stale_source_paths;

        [[nodiscard]] u32 total_entries() const {
            return stale_upstream_entries + downstream_cascade_entries;
        }
        [[nodiscard]] bool would_reconcile() const { return total_entries() > 0; }
    };
    [[nodiscard]] CookDependencyReconcileEstimate estimate_stale_dependency_hashes(

    /// Non-mutating reconcile estimate for upstream dependency invalidation (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_invalidate_upstream_dependency(
    /// Non-mutating reconcile estimate for stale dependency-hash invalidation (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_invalidate_stale_dependency_hashes(
    /// Non-mutating estimate of entries `invalidate_stale_dependency_hashes` would drop (B7.9 deepen).
    [[nodiscard]] u32 estimate_stale_dependency_hashes(const CookManifest& manifest) const;
    /// Non-destructive reconcile estimate for invalid/stale cache records (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_cache_reconcile() const;
    /// Non-destructive estimate of stale upstream dependency invalidations (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationProbe estimate_stale_dependency_invalidation(
    /// Non-mutating reconcile estimator — stale upstream entries that reconcile would remove (B7.9 deepen).
    [[nodiscard]] u32 estimate_stale_dependency_entries(const CookManifest& manifest) const;
    /// True when `estimate_stale_dependency_entries` is non-zero (B7.9 deepen).
    [[nodiscard]] bool cache_needs_dependency_reconcile(const CookManifest& manifest) const;
    /// Non-mutating probe mirroring `invalidate_upstream_dependency` (B7.9 deepen).
    [[nodiscard]] u32 estimate_upstream_invalidation(const CookManifest& manifest,
    /// Reconcile estimator — upstream stale entries `invalidate_stale_dependency_hashes` would drop (B7.9 deepen).
    [[nodiscard]] u32 estimate_stale_dependency_reconcile(const CookManifest& manifest) const;
    /// Dry-run reconcile — counts entries `invalidate_stale_dependency_hashes` would remove (B7.9 deepen).
    [[nodiscard]] u32 estimate_stale_dependency_invalidations(const CookManifest& manifest) const;
    /// Non-mutating estimate of entries `invalidate_upstream_dependency` would drop (B7.9 deepen).
    [[nodiscard]] u32 estimate_upstream_dependency_invalidation(const CookManifest& manifest,
    /// Non-mutating estimate for `invalidate_stale_dependency_hashes` (B7.9 deepen follow-up).
    [[nodiscard]] CookCacheReconcileEstimate estimate_stale_dependency_hashes(const CookManifest& manifest) const;
    /// Read-only estimate of `invalidate_stale_dependency_hashes` removals (B7.9 deepen).
    [[nodiscard]] u32 estimate_stale_dependency_hash_invalidations(const CookManifest& manifest) const;
    /// Read-only prune reconcile estimator — entries `prune_all` would remove (B7.9 deepen).
    [[nodiscard]] u32 count_prune_removals() const;
    /// True when stale dependency-hash reconcile would invalidate at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_reconcile_stale_dependencies(const CookManifest& manifest) const;
    /// Read-only stale-content reconcile probe across manifest job sources (B7.9 deepen).
    [[nodiscard]] u32 count_stale_content_invalidation(const CookManifest& manifest) const;
    /// Read-only prune reconcile probe — entries `CookCache::prune_all` would drop (B7.9 deepen).
    [[nodiscard]] u32 count_prune_invalidation() const;
    [[nodiscard]] u32 estimate_stale_dependency_invalidation(const CookManifest& manifest) const;

    CookCache& cache() { return m_cache; }
    const CookCache& cache() const { return m_cache; }

private:
    CookRecord cook_with_cache_(CookAssetKind kind,
                                const std::string& source_path,
                                const std::string& output_path,
                                u64 content_hash,
                                u64 upstream_hash,
                                const char* stub_note);

    CookCache m_cache;
};

} // namespace fuse::project
