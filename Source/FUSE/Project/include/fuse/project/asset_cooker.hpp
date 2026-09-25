#pragma once

#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_desc.hpp>

#include <functional>
#include <string>

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

/// Result of writing one cooked output (importer/encoder outcome).
struct CookWriteOutcome {
    bool ok = false;
    u32 byte_count = 0;
    std::string note;
    /// Record status when `ok` is false (e.g. `MalformedSource`, `CorruptImage`).
    CookStatus status = CookStatus::InvalidInput;
};

/// How mesh / texture sources are validated during a cook.
enum class ImportValidation : u8 {
    /// Default. Sources go through the real importers (assimp, stb_image + BC7) and anything they
    /// cannot turn into a valid cooked asset fails with a specific `CookStatus` and writes nothing:
    /// `MalformedSource` / `InvalidGeometry` for meshes, `CorruptImage` / `InvalidImageDimensions` for
    /// textures, `ImporterUnavailable` when the library is not linked. Cache hits are only served when
    /// the cached output still loads as a real cooked asset.
    Strict,
    /// Explicit opt-out for tools that must keep going on unusable sources: undecodable inputs are
    /// cooked to labelled placeholder stubs (`FUSEMESH_STUB`, synthesized BC7 texture) and report `Ok`.
    /// Callers choosing this must say why at the call site.
    Lenient,
};

[[nodiscard]] const char* importValidationName(ImportValidation mode);

/// Offline asset cooker — mesh/texture/audio transforms (B7.9 stub; no runtime link).
class AssetCooker {
public:
    CookRecord cook_mesh(const MeshImportDesc& desc);
    CookRecord cook_texture(const TextureImportDesc& desc);
    CookRecord cook_audio(const AudioImportDesc& desc);
    CookRecord cook_shader(const ShaderImportDesc& desc);

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

    /// Import validation mode — `ImportValidation::Strict` unless explicitly relaxed (see the enum).
    void set_import_validation(ImportValidation mode) { m_validation = mode; }
    [[nodiscard]] ImportValidation import_validation() const { return m_validation; }
    /// Compatibility shorthand: `set_strict_import(false)` == `set_import_validation(Lenient)`.
    void set_strict_import(bool strict) {
        m_validation = strict ? ImportValidation::Strict : ImportValidation::Lenient;
    }
    [[nodiscard]] bool strict_import() const { return m_validation == ImportValidation::Strict; }

    CookCache& cache() { return m_cache; }
    const CookCache& cache() const { return m_cache; }

    /// Read-only cache probe for job-graph short-circuit — no stub cook on hit (U7 wave 13).
    [[nodiscard]] bool probe_cook_cache_hit(const CookManifestEntry& entry, const CookManifest& manifest,
                                            CookRecord* out_record = nullptr);

private:
    using CookWriteFn = std::function<CookWriteOutcome()>;

    /// Cache lookup → (on miss) write → cache store. Entries are stored only after the output was
    /// written successfully; a hit whose output file is gone is dropped and re-cooked.
    CookRecord cook_with_cache_(CookAssetKind kind,
                                const std::string& source_path,
                                const std::string& output_path,
                                u64 content_hash,
                                u64 upstream_hash,
                                const char* stub_note,
                                const CookWriteFn& write);
    [[nodiscard]] u64 effective_cache_key_(u64 content_hash, u64 upstream_hash) const;
    bool lookup_live_entry_(u64 cache_key, CookCacheEntry* out_entry);

    CookCache m_cache;
    ImportValidation m_validation = ImportValidation::Strict;
};

} // namespace fuse::project
