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

    /// Read-only upstream invalidation probe — guarded on empty `changed_source` (B7.9 deepen).
    [[nodiscard]] u32 count_upstream_invalidation(const CookManifest& manifest,
                                                  const std::string& changed_source) const;
    /// True when `invalidate_upstream_dependency` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_upstream_dependency(const CookManifest& manifest,
                                                            const std::string& changed_source) const;
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
        const CookManifest& manifest, const std::string& changed_source) const;

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

// --- deepen additive from b79-cooker-hash-guards-111f ---
        [[nodiscard]] bool would_reconcile() const { return total_entries() > 0; }

// --- deepen additive from deepen-b79-cooker-hash-0896 ---
    [[nodiscard]] bool would_reconcile_stale_dependencies(const CookManifest& manifest) const;

// --- deepen additive from deepen-b79-cooker-hash-guards-2061 ---
    [[nodiscard]] bool would_need_stale_dependency_reconcile(const CookManifest& manifest) const;

// --- deepen additive from deepen-fuse-b79-cooker-hash-fdd2 ---
    [[nodiscard]] bool would_upstream_invalidation(const CookManifest& manifest,
    [[nodiscard]] bool would_stale_dependency_invalidation(const CookManifest& manifest) const;

// --- deepen additive from deepen-b79-cooker-hash-3e1a ---
    [[nodiscard]] bool would_invalidate_upstream(const CookManifest& manifest,

// --- deepen additive from deepen-b79-cooker-hash-c0c4 ---
    [[nodiscard]] bool would_invalidate() const { return total() > 0; }

// --- deepen additive from deepen-b79-cooker-hash-7a34 ---
    [[nodiscard]] bool would_upstream_invalidate(const CookManifest& manifest,

// --- deepen additive from deepen-b79-cooker-hash-0e64 ---
    [[nodiscard]] bool would_reconcile_invalidate(const CookManifest& manifest) const;

// --- deepen additive from deepen-b79-cooker-hash-guards-82a4 ---
    [[nodiscard]] bool would_invalidate_stale_dependencies(const CookManifest& manifest) const;

// --- deepen additive from deepen-b79-cooker-hash-1159 ---
    [[nodiscard]] bool would_stale_dependency_invalidate(const CookManifest& manifest) const;

// --- deepen additive from deepen-b79-cooker-hash-guards-93a9 ---
    [[nodiscard]] bool would_prune_reconcile() const;

// --- deepen additive from deepen-b79-cooker-hash-b8de ---
    [[nodiscard]] bool would_reconcile() const { return total() != 0; }

// --- deepen additive from deepen-b79-cooker-hash-guards-6ecd ---
    [[nodiscard]] bool should_skip_upstream_invalidation(const CookManifest& manifest,
    [[nodiscard]] bool should_skip_stale_dependency_invalidation(const CookManifest& manifest) const;
    [[nodiscard]] bool should_skip_reconcile_invalidation(const CookManifest& manifest) const;

// --- deepen additive from b79-cooker-hash-skip-guards-93f1 ---
    [[nodiscard]] bool should_skip() const { return !would_reconcile(); }

// --- deepen additive from deepen-b79-cooker-hash-should-skip-fa40 ---
    [[nodiscard]] bool should_skip_prune_reconcile() const;

// --- deepen additive from deepen-b79-cooker-hash-e9a4 ---
    [[nodiscard]] inline bool should_skip_reconcile_invalidation(const CookManifest& manifest) const {
    [[nodiscard]] inline bool should_skip_upstream_invalidation(const CookManifest& manifest,

// --- deepen additive from deepen-b79-cooker-hash-0d57 ---
    [[nodiscard]] bool should_skip_reconcile_invalidation(const CookManifest& manifest,

// --- deepen additive from b79-cooker-hash-guards-2be9 ---
[[nodiscard]] inline bool should_skip_reconcile_invalidation(const CookCacheReconcileEstimate& estimate) {
    return estimate.should_skip();
