#pragma once

#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

namespace fuse::project {

/// Read-only invalidation breakdown for upstream / reconcile planning (B7.9 deepen).
struct CookReconcileEstimate {
    u32 direct_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return direct_entries + downstream_entries; }
    [[nodiscard]] bool would_invalidate() const { return total() > 0; }
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
    /// Upstream invalidation breakdown — direct source entries plus downstream dependents (B7.9 deepen).
    [[nodiscard]] CookReconcileEstimate estimate_upstream_invalidation(
        const CookManifest& manifest, const std::string& changed_source) const;
    /// Stale dependency-hash reconcile breakdown — stale upstream entries plus downstream (B7.9 deepen).
    [[nodiscard]] CookReconcileEstimate estimate_stale_dependency_reconcile(
        const CookManifest& manifest) const;
    /// True when `invalidate_upstream_dependency` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_upstream_dependency(const CookManifest& manifest,
                                                            const std::string& changed_source) const;

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
