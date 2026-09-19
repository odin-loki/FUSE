#pragma once

#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

namespace fuse::project {

/// Read-only upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
struct CookCacheUpstreamInvalidationEstimate {
    u32 direct_source_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return direct_source_entries + downstream_entries; }
struct CookUpstreamInvalidationEstimate {
    u32 direct_entries = 0;

    [[nodiscard]] u32 total() const { return direct_entries + downstream_entries; }
};

/// Read-only reconcile planning breakdown for cache + dependency invalidation (B7.9 deepen).
struct CookCacheReconcileEstimate {
    u32 stale_dependency_entries = 0;
    u32 prune_invalid_entries = 0;
    u32 prune_stale_entries = 0;
    u32 upstream_invalidation_entries = 0;

    [[nodiscard]] u32 total() const {
        return stale_dependency_entries + prune_invalid_entries + prune_stale_entries +
               upstream_invalidation_entries;
    }
    /// True when no reconcile invalidation is estimated — mirrors `total() == 0` (B7.9 deepen).
    [[nodiscard]] bool should_skip() const { return total() == 0; }
};

/// Read-only upstream invalidation breakdown for a changed source (B7.9 deepen).
struct CookCacheUpstreamReconcileEstimate {
    u32 direct_source_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return direct_source_entries + downstream_entries; }
/// Read-only cache reconcile planner — mirrors prune/invalidate estimators (B7.9 deepen).
    u32 stale_dependency_invalidations = 0;
    u32 upstream_invalidation = 0;
    u32 prunable_entries = 0;
    u32 invalid_entries = 0;
    u32 stale_entries = 0;
/// Breakdown of upstream invalidation removals — read-only planning estimate (B7.9 deepen).
struct CookUpstreamInvalidationEstimate {
    u32 direct = 0;
    u32 downstream = 0;

    [[nodiscard]] u32 total() const { return direct + downstream; }

/// Breakdown of stale dependency-hash reconcile removals — read-only planning estimate (B7.9 deepen).
struct CookStaleDependencyReconcileEstimate {
    u32 direct_stale = 0;
    u32 downstream_cascade = 0;

    [[nodiscard]] u32 total() const { return direct_stale + downstream_cascade; }
/// Read-only invalidation breakdown for upstream / reconcile planning (B7.9 deepen).
struct CookReconcileEstimate {
    u32 direct_entries = 0;

    [[nodiscard]] u32 total() const { return direct_entries + downstream_entries; }
    [[nodiscard]] bool would_invalidate() const { return total() > 0; }
/// Read-only stale dependency-hash reconcile breakdown (B7.9 deepen).
    u32 stale_upstream_entries = 0;
    u32 downstream_cascade_entries = 0;

    [[nodiscard]] u32 total() const { return stale_upstream_entries + downstream_cascade_entries; }

/// Read-only upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
struct CookCacheUpstreamInvalidationEstimate {


/// Read-only upstream invalidation planning breakdown (B7.9 deepen).


struct CookCacheUpstreamInvalidateEstimate {










    u32 source_entries = 0;

    [[nodiscard]] u32 total() const { return source_entries + downstream_entries; }

/// Upstream invalidation breakdown — direct source entries plus downstream cascade (B7.9 deepen).






struct CookUpstreamReconcileEstimate {


/// Read-only upstream invalidation breakdown — mirrors `count_upstream_invalidation` (B7.9 deepen).


/// Upstream invalidation breakdown — direct source hits plus downstream dependents (B7.9 deepen).














    [[nodiscard]] bool can_reconcile() const { return total() > 0; }
    [[nodiscard]] bool should_skip() const { return !can_reconcile(); }

/// Upstream invalidation breakdown — direct source entries plus downstream dependents (B7.9 deepen).




/// Stale dependency-hash reconcile breakdown — upstream mismatches plus downstream cascade (B7.9 deepen).
struct CookStaleDependencyEstimate {


    [[nodiscard]] bool would_reconcile() const { return total() != 0; }
};

/// Read-only upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
struct CookCacheUpstreamInvalidationEstimate {
    u32 direct_source_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return direct_source_entries + downstream_entries; }
};

/// Read-only upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
struct CookUpstreamInvalidationEstimate {
    u32 source_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return source_entries + downstream_entries; }
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
    /// Upstream invalidation breakdown without mutating cache (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamInvalidationEstimate estimate_upstream_invalidation(
    /// Structured upstream invalidation breakdown — mirrors `count_upstream_invalidation` (B7.9 deepen).
    [[nodiscard]] CookUpstreamInvalidationEstimate estimate_upstream_invalidation(
    /// Upstream invalidation breakdown — direct source entries plus downstream dependents (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamInvalidateEstimate estimate_upstream_invalidation(
        const CookManifest& manifest, const std::string& changed_source) const;
    /// True when `estimate_upstream_invalidation(...).total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_upstream_invalidation(const CookManifest& manifest,
                                                   const std::string& changed_source) const;
    /// Deduplicated source paths `invalidate_upstream_dependency` would touch (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_upstream_invalidation_sources(
    /// Read-only output-path invalidation probe — guarded on empty `changed_output` (B7.9 deepen).
    [[nodiscard]] u32 count_output_invalidation(const CookManifest& manifest,
                                                const std::string& changed_output) const;
    /// Structured upstream invalidation breakdown for incremental planning (B7.9 deepen).
    /// True when `count_upstream_invalidation` is non-zero — guarded on empty `changed_source` (B7.9 deepen).
    [[nodiscard]] bool would_upstream_invalidate(const CookManifest& manifest,
    /// Upstream invalidation breakdown — mirrors `count_upstream_invalidation` (B7.9 deepen).
    /// Source paths `invalidate_upstream_dependency` would touch — deduplicated (B7.9 deepen).
    /// Upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
    /// True when `estimate_reconcile_invalidation(manifest).total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_reconcile_invalidation(const CookManifest& manifest) const;
    /// Upstream invalidation breakdown without mutating cache stats (B7.9 deepen).
    /// Read-only stale dependency-hash reconcile probe (B7.9 deepen).
    [[nodiscard]] u32 count_stale_dependency_invalidation(const CookManifest& manifest) const;
    /// True when `invalidate_stale_dependency_hashes` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_stale_dependency_hashes(const CookManifest& manifest) const;
    /// True when `count_stale_dependency_invalidation` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_stale_dependency_invalidation(const CookManifest& manifest) const;
    /// Upstream invalidation breakdown — mirrors `count_upstream_invalidation` components (B7.9 deepen).
    /// True when `count_upstream_invalidation` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_stale_dependency_invalidate(const CookManifest& manifest) const;
    /// True when `estimate_reconcile_invalidation().total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_reconcile_invalidate(const CookManifest& manifest) const;
    /// Deduplicated stale dependency sources from upstream-hash reconcile (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_dependency_sources(
        const CookManifest& manifest) const;
    /// True when `count_stale_dependency_invalidation` would remove at least one entry (B7.9 deepen).
    /// Deduplicated source paths `invalidate_stale_dependency_hashes` would touch (B7.9 deepen).
    /// True when stale dependency hashes would invalidate cache entries (B7.9 deepen).
    /// Read-only prune reconcile probe — mirrors `CookCache::estimate_prune_removals` (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_reconcile() const;
    /// Combined dependency + prune reconcile estimator for incremental invalidation planning (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_reconcile_invalidation(
        const CookManifest& manifest) const;
    /// Upstream invalidation breakdown for `changed_source` — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamReconcileEstimate estimate_upstream_reconcile(
    /// Read-only upstream invalidation reconcile breakdown (B7.9 deepen).
    [[nodiscard]] CookUpstreamInvalidationEstimate estimate_upstream_invalidation(
    /// Reconcile estimate including upstream invalidation for a changed source path (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_reconcile_invalidation(
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
    /// Read-only prune-all impact estimate — mirrors `CookCache::count_prunable_entries` (B7.9 deepen).
    [[nodiscard]] u32 estimate_prune_all() const;
    /// Combined reconcile estimate — stale dependency invalidation plus prunable entries (B7.9 deepen).
    [[nodiscard]] u32 estimate_reconcile_invalidation(const CookManifest& manifest) const;
    /// Combined prune + stale-dependency reconcile estimate for a manifest (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_cache_reconcile(const CookManifest& manifest) const;
    /// Upstream-change reconcile estimate — upstream cascade plus prunable entry counts (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_upstream_change_reconcile(
    /// Read-only upstream invalidation breakdown — guarded on empty `changed_source` (B7.9 deepen).
    /// Read-only stale dependency-hash reconcile breakdown (B7.9 deepen).
    [[nodiscard]] CookStaleDependencyReconcileEstimate estimate_stale_dependency_reconcile(
    /// True when `estimate_stale_dependency_reconcile` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_need_stale_dependency_reconcile(const CookManifest& manifest) const;

                                                                       const std::string& changed_source) const;

    /// Source paths that `invalidate_upstream_dependency` would touch — guarded on empty `changed_source` (B7.9 deepen).
    /// Read-only stale dependency reconcile breakdown via cook job graph (B7.9 deepen).
    [[nodiscard]] CookCacheStaleUpstreamEstimate estimate_stale_dependency_reconciliation(
    /// True when `invalidate_upstream_dependency` would remove at least one entry (B7.9 deepen).
    /// True when `invalidate_stale_dependency_hashes` would reconcile at least one entry (B7.9 deepen).
    /// Alias for `count_stale_dependency_invalidation` — reconcile removal estimate (B7.9 deepen).
    [[nodiscard]] u32 estimate_stale_dependency_reconcile(const CookManifest& manifest) const {
        return count_stale_dependency_invalidation(manifest);
    /// True when stale dependency reconcile would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_upstream_invalidation(const CookManifest& manifest,
    [[nodiscard]] bool would_stale_dependency_invalidation(const CookManifest& manifest) const;
    /// Read-only prune reconcile probe — entries `prune_all` would remove (B7.9 deepen).
    [[nodiscard]] u32 count_prunable_cache_entries() const;

    /// Read-only upstream invalidation breakdown — mirrors `count_upstream_invalidation` (B7.9 deepen).
    struct CookUpstreamInvalidationEstimate {
        u32 source_direct = 0;
        u32 downstream_cascade = 0;

        [[nodiscard]] u32 total() const { return source_direct + downstream_cascade; }

    /// Read-only stale dependency reconcile breakdown — mirrors `count_stale_dependency_invalidation` (B7.9 deepen).
    struct CookStaleDependencyEstimate {
        u32 direct_upstream_stale = 0;

        [[nodiscard]] u32 total() const { return direct_upstream_stale + downstream_cascade; }
    [[nodiscard]] CookStaleDependencyEstimate estimate_stale_dependency_reconcile(
    /// Entries `cache().prune_all` would remove — mirrors prune guards without mutating stats (B7.9 deepen).
    [[nodiscard]] u32 estimate_cache_prune() const;
    /// True when `count_upstream_invalidation` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_upstream(const CookManifest& manifest,
    /// True when `count_stale_dependency_invalidation` is non-zero (B7.9 deepen).
    /// Read-only prune reconcile estimator — mirrors `CookCache::count_prunable_entries` (B7.9 deepen).
    [[nodiscard]] u32 count_prune_reconcile() const;

    /// Direct vs downstream breakdown for upstream invalidation planning (B7.9 deepen).
        u32 direct = 0;
        u32 downstream = 0;
        u32 total = 0;
    /// Direct vs downstream breakdown for dependency-hash reconcile planning (B7.9 deepen).
        u32 direct_stale = 0;
        u32 downstream_stale = 0;
    [[nodiscard]] CookDependencyReconcileEstimate estimate_stale_dependency_reconcile(
    /// Upstream invalidation breakdown — direct source entries plus downstream dependents (B7.9 deepen).
    [[nodiscard]] CookReconcileEstimate estimate_upstream_invalidation(
    /// Stale dependency-hash reconcile breakdown — stale upstream entries plus downstream (B7.9 deepen).
    [[nodiscard]] CookReconcileEstimate estimate_stale_dependency_reconcile(
    [[nodiscard]] bool would_invalidate_upstream_dependency(const CookManifest& manifest,
    /// Aggregate reconcile estimator — cache prune + stale upstream/direct cascade (B7.9 deepen).
    /// Read-only stale content-hash reconcile probe — guarded on empty manifest (B7.9 deepen).
    /// Combined reconcile estimator — stale content plus stale dependency removals (B7.9 deepen).
    [[nodiscard]] u32 estimate_reconcile_removals(const CookManifest& manifest) const;
    /// Combined stale-dependency and prune reconcile estimate for manifest cache planning (B7.9 deepen).
    [[nodiscard]] u32 estimate_manifest_cache_reconcile(const CookManifest& manifest) const;
    /// Read-only stale dependency-hash reconcile estimator with upstream/downstream breakdown (B7.9 deepen).
    /// Read-only prune reconcile estimate — mirrors `prune_all` guards (B7.9 deepen).
    [[nodiscard]] u32 estimate_prune_reconcile() const;
    /// Combined stale-upstream and prunable-entry reconcile estimate (B7.9 deepen).
    [[nodiscard]] u32 estimate_full_cache_reconcile(const CookManifest& manifest) const;
    /// Bool reconcile probes — mirror count/estimate helpers without mutating cache (B7.9 deepen).
    /// True when `estimate_reconcile_invalidation(...).total()` is non-zero (B7.9 deepen).
    /// True when `estimate_reconcile_invalidation` reports a non-zero total (B7.9 deepen).
    [[nodiscard]] bool would_upstream_invalidate(const CookManifest& manifest,
    /// Deduplicated sources with stale upstream dependency hashes (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_dependency_sources(
        const CookManifest& manifest) const;
    /// True when `count_upstream_invalidation(manifest, changed_source)` is non-zero (B7.9 deepen).
    /// True when `estimate_reconcile_invalidation` would remove at least one entry (B7.9 deepen).
    /// Read-only upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamInvalidationEstimate estimate_upstream_invalidation(
    /// Deduplicated source paths upstream invalidation would touch — read-only probe (B7.9 deepen).
    /// Upstream invalidation reconcile breakdown — mirrors `count_upstream_invalidation` (B7.9 deepen).
    /// Source paths `invalidate_upstream_dependency` would touch — deduplicated (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamInvalidateEstimate estimate_upstream_invalidation(
    /// True when `estimate_upstream_invalidation` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_reconcile_invalidate(const CookManifest& manifest) const;
    /// Deduplicated source paths contributing to a non-zero reconcile estimate (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_reconcile_stale_sources(
    [[nodiscard]] CookCacheInvalidationEstimate estimate_upstream_invalidation(
    [[nodiscard]] std::vector<std::string> probe_upstream_invalidation_closure(
    /// True when `invalidate_stale_dependency_hashes` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_stale_dependencies(const CookManifest& manifest) const;
    /// Structured upstream invalidation probe — guarded on empty `changed_source` (B7.9 deepen).
    /// True when `estimate_prune_reconcile().total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_prune_reconcile() const;
    [[nodiscard]] CookUpstreamReconcileEstimate estimate_upstream_invalidation(
    /// True when `estimate_upstream_invalidation(manifest, changed_source).total()` is non-zero (B7.9 deepen).
    /// Shorthand for `estimate_prune_reconcile().total()` (B7.9 deepen).
    /// Upstream invalidation breakdown — mirrors `count_upstream_invalidation` (B7.9 deepen).
    /// True when `count_stale_dependency_invalidation` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_stale_dependency_invalidate(const CookManifest& manifest) const;
    /// True when `estimate_reconcile_invalidation().total()` is non-zero (B7.9 deepen).
    /// Upstream invalidation breakdown — guarded on empty `changed_source` (B7.9 deepen).
    /// True when `estimate_upstream_invalidation(...).total()` is non-zero (B7.9 deepen).
    /// Read-only reconcile preflight — alias of `estimate_reconcile_invalidation` (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate preflight_reconcile_invalidation(
        const CookManifest& manifest) const {
        return estimate_reconcile_invalidation(manifest);
    /// True when reconcile invalidation would remove or prune at least one cache entry (B7.9 deepen).
    /// True when reconcile invalidation can be skipped — mirrors `CookCacheReconcileEstimate::should_skip` (B7.9 deepen).
    [[nodiscard]] bool should_skip_reconcile_invalidation(const CookManifest& manifest) const;
    /// Upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` guards (B7.9 deepen).
    /// Stale dependency-hash reconcile breakdown — mirrors `invalidate_stale_dependency_hashes` (B7.9 deepen).
    /// Read-only upstream invalidation probe — guarded on empty `changed_source` (B7.9 deepen).

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
