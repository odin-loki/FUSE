#pragma once

#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

namespace fuse::project {

/// Read-only cache reconcile planner — mirrors prune/invalidate estimators (B7.9 deepen).
struct CookCacheReconcileEstimate {
    u32 stale_dependency_invalidations = 0;
    u32 upstream_invalidation = 0;
    u32 prunable_entries = 0;
    u32 invalid_entries = 0;
    u32 stale_entries = 0;
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
    /// Combined prune + stale-dependency reconcile estimate for a manifest (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_cache_reconcile(const CookManifest& manifest) const;
    /// Upstream-change reconcile estimate — upstream cascade plus prunable entry counts (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_upstream_change_reconcile(
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
