#pragma once

#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

namespace fuse::project {

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

    /// Non-mutating upstream invalidation scope probe via the cook job graph (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationProbe probe_upstream_dependency(const CookManifest& manifest,
                                                                       const std::string& changed_source) const;
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
        const CookManifest& manifest) const;

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
