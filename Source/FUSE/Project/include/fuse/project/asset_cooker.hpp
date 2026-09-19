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
    [[nodiscard]] bool should_skip() const { return total() == 0; }
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
    /// Read-only stale dependency-hash reconcile probe (B7.9 deepen).
    [[nodiscard]] u32 count_stale_dependency_invalidation(const CookManifest& manifest) const;
    /// Read-only prune reconcile probe — mirrors `CookCache::estimate_prune_removals` (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_reconcile() const;
    /// Combined dependency + prune reconcile estimator for incremental invalidation planning (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_reconcile_invalidation(
        const CookManifest& manifest) const;
    /// True when `count_upstream_invalidation` is zero — guarded on empty `changed_source` (B7.9 deepen).
    [[nodiscard]] bool should_skip_upstream_invalidation(const CookManifest& manifest,
                                                         const std::string& changed_source) const;
    /// True when `count_stale_dependency_invalidation` is zero (B7.9 deepen).
    [[nodiscard]] bool should_skip_stale_dependency_invalidation(const CookManifest& manifest) const;
    /// True when `estimate_reconcile_invalidation` would remove nothing (B7.9 deepen).
    [[nodiscard]] bool should_skip_reconcile_invalidation(const CookManifest& manifest) const;

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
