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

/// Read-only prune reconcile breakdown — mirrors `prune_invalid_entries` / `prune_stale_entries` (B7.9 deepen).
/// Read-only prune breakdown — mirrors `prune_invalid_entries` + `prune_stale_entries` guards (B7.9 deepen).
struct CookCachePruneEstimate {
    u32 invalid_entries = 0;
    u32 stale_entries = 0;

    [[nodiscard]] u32 total() const { return invalid_entries + stale_entries; }
    /// True when no prune removals are estimated — mirrors `total() == 0` (B7.9 deepen).
    [[nodiscard]] bool should_skip() const { return total() == 0; }
/// Non-mutating prune estimate — mirrors `prune_*` without touching stats (B7.9 deepen).
/// Read-only prune reconcile breakdown — mirrors `prune_invalid_entries` + `prune_stale_entries` (B7.9 deepen).
    u32 invalid_count = 0;
    u32 stale_count = 0;

    [[nodiscard]] u32 total() const { return invalid_count + stale_count; }
    [[nodiscard]] bool would_prune() const { return total() > 0; }
};

/// Non-mutating invalidation probe — mirrors `invalidate_*` without mutation (B7.9 deepen).
struct CookCacheInvalidationProbe {
    u32 affected_count = 0;

    [[nodiscard]] bool would_invalidate() const { return affected_count > 0; }

/// Non-mutating reconcile estimate for manifest-driven cache invalidation (B7.9 deepen).
struct CookCacheReconcileEstimate {
    u32 direct_count = 0;
    u32 downstream_count = 0;

    [[nodiscard]] u32 total() const { return direct_count + downstream_count; }
    [[nodiscard]] bool would_reconcile() const { return total() > 0; }
/// Non-mutating prune/reconcile budget — mirrors `prune_*` without touching stats (B7.9 deepen).
    u32 prunable_entries = 0;
/// Non-mutating probe for incremental invalidation — counts entries a call would remove (B7.9 deepen follow-up).
    u32 affected_entries = 0;
    bool empty_cache = true;
    bool empty_path = false;
    bool zero_hash = false;

    [[nodiscard]] bool would_invalidate() const { return affected_entries > 0; }
    [[nodiscard]] bool should_skip() const { return !would_invalidate(); }

/// Non-mutating reconcile estimate — counts prunable or stale-upstream entries (B7.9 deepen follow-up).
    u32 upstream_stale_entries = 0;
    u32 total_removable = 0;

    [[nodiscard]] bool would_reconcile() const { return total_removable > 0; }
    [[nodiscard]] bool should_skip() const { return !would_reconcile(); }

/// Read-only stale-upstream reconcile breakdown — direct stale rows plus downstream cascade (B7.9 deepen).
struct CookCacheStaleUpstreamEstimate {
    u32 stale_upstream = 0;
    u32 downstream_cascade = 0;

    [[nodiscard]] u32 total() const { return stale_upstream + downstream_cascade; }
/// Read-only reconcile breakdown — mirrors prune/invalidate paths without mutating stats (B7.9 deepen).
    u32 stale_content_entries = 0;
    u32 stale_upstream_entries = 0;

    [[nodiscard]] u32 prunable_entries() const { return invalid_entries + stale_content_entries; }
    [[nodiscard]] u32 total_entries() const {
        return invalid_entries + stale_content_entries + stale_upstream_entries;
    }

/// Read-only invalidation planning breakdown — mirrors `invalidate_*` probes (B7.9 deepen).
struct CookCacheInvalidationEstimate {
    u32 all_entries = 0;

    [[nodiscard]] bool would_invalidate_all() const { return all_entries != 0; }

/// Read-only upstream invalidation breakdown — mirrors `invalidate_upstream_dependency` (B7.9 deepen).
struct CookCacheUpstreamInvalidationEstimate {
    u32 direct_source_entries = 0;
    u32 downstream_entries = 0;

    [[nodiscard]] u32 total() const { return direct_source_entries + downstream_entries; }

/// Structural store preflight — mirrors `store` / `is_valid_cook_cache_entry` guards (B7.9 deepen).
struct CookCacheEntryPreflight {
    bool can_store = false;
    CookHashRejectReason reason = CookHashRejectReason::None;

    [[nodiscard]] bool ok() const { return can_store; }

/// Read-only invalidation breakdown — mirrors `invalidate_*` guards (B7.9 deepen).
    u32 source_entries = 0;
    u32 output_entries = 0;

    [[nodiscard]] u32 total() const {
        return source_entries + output_entries + stale_content_entries + stale_upstream_entries +
               downstream_entries;



/// Cache-wide invalidation surface — entry totals plus prune-class breakdown (B7.9 deepen).
struct CookCacheInvalidationSurface {
    u32 entry_count = 0;
    u32 invalid_entries = 0;
    u32 stale_entries = 0;

    [[nodiscard]] u32 reconcile_total() const { return prunable_entries; }

/// Read-only cache-entry store preflight — mirrors `store` structural guards (B7.9 deepen).


/// Read-only store preflight — mirrors `store` guards without mutating the cache (B7.9 deepen).


/// Read-only upstream invalidation breakdown — mirrors `invalidate_downstream_of` (B7.9 deepen).
    u32 direct_entries = 0;

    [[nodiscard]] u32 total() const { return direct_entries + downstream_entries; }



/// Read-only invalidation reconcile breakdown — stale upstream plus prune buckets (B7.9 deepen).
    u32 prune_invalid_entries = 0;
    u32 prune_stale_entries = 0;

        return stale_upstream_entries + prune_invalid_entries + prune_stale_entries;




    [[nodiscard]] bool would_prune() const { return total() != 0; }
    /// True when `prune_*` would remove at least one entry (B7.9 deepen).
    /// True when prune reconcile can be skipped — mirrors `!would_prune()` (B7.9 deepen).
    /// True when `prune_all` would be a no-op — mirrors `would_prune_all` negation (B7.9 deepen).
    /// True when no prune reconcile work is pending (B7.9 deepen).
    /// True when `prune_all` would be a no-op — mirrors `total() == 0` (B7.9 deepen).
    /// True when no prune work is needed — `total()` is zero (B7.9 deepen).
    /// True when prune reconcile can be skipped — no invalid or stale records (B7.9 deepen).





    /// True when `prune_*` would be a no-op — mirrors `total() == 0` (B7.9 deepen).

    /// True when prune reconcile would be a no-op — `total()` is zero (B7.9 deepen).
    /// True when prune reconcile would be a no-op — mirrors `total() == 0` (B7.9 deepen).
    /// True when no prune work is needed — mirrors `prune_all` early-out (B7.9 deepen).
    /// True when no prune reconcile work is needed — mirrors `total() == 0` (B7.9 deepen).
    /// True when no invalid or stale records would be pruned (B7.9 deepen).
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

/// Inverse of `is_valid_cook_cache_entry` — zero keys or empty paths (B7.9 deepen).
[[nodiscard]] inline bool is_invalid_cook_cache_entry(const CookCacheEntry& entry) {
    return !is_valid_cook_cache_entry(entry);
}

/// True when a structurally valid entry's stored key no longer matches its source (B7.9 deepen).
[[nodiscard]] bool is_stale_cook_cache_entry(const CookCacheEntry& entry);

/// Preflight for cache store — mirrors `CookCache::store` rejection paths (B7.9 deepen).
struct CookCacheStorePreflight {
    bool zero_content_hash = false;
    bool empty_source_path = false;
    bool empty_output_path = false;

    [[nodiscard]] bool can_store() const {
        return !zero_content_hash && !empty_source_path && !empty_output_path;
};

/// Preflight for cache lookup — mirrors `CookCache::lookup` miss paths (B7.9 deepen).
struct CookCacheLookupPreflight {
    bool cache_empty = true;
    bool entry_present = false;

    [[nodiscard]] bool would_hit() const { return !zero_content_hash && !cache_empty && entry_present; }

/// Non-destructive invalidation probe — estimates removals without mutating stats (B7.9 deepen).
struct CookCacheInvalidationProbe {
    bool invalid_args = false;
    u32 would_invalidate_count = 0;

    [[nodiscard]] bool would_invalidate() const {
        return !cache_empty && !invalid_args && would_invalidate_count > 0;

/// Reconcile estimator — counts invalid/stale records `prune_all` would remove (B7.9 deepen).
struct CookCacheReconcileEstimate {
    u32 invalid_entry_count = 0;
    u32 stale_entry_count = 0;

    [[nodiscard]] u32 total_prunable() const { return invalid_entry_count + stale_entry_count; }
    [[nodiscard]] bool needs_reconcile() const { return total_prunable() > 0; }

/// Preflight guard before `store` — true when the entry would be accepted (B7.9 deepen).
[[nodiscard]] inline CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    if (entry.source_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
    if (entry.output_path.empty()) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;
    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
/// Read-only store preflight — mirrors `store` guards plus source readability (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);
/// Read-only cache-entry preflight — structural paths plus source readability (B7.9 deepen).
/// Read-only cache-entry hash preflight — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
/// Read-only cache-entry preflight — mirrors `is_valid_cook_cache_entry` guards (B7.9 deepen).
/// Read-only cache-entry hash preflight — mirrors `is_valid_cook_cache_entry` guards (B7.9 deepen).
/// Read-only cache-entry hash preflight — mirrors `store` guards without mutating (B7.9 deepen).
/// Read-only store preflight — structural guards plus kind-specific source hash checks (B7.9 deepen).
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
    case CookAssetKind::Shader: {
        preflight.reason = CookHashRejectReason::SourceUnreadable;
    return {};
/// Read-only structural preflight for cache records — mirrors `store` guards (B7.9 deepen).
        preflight.reason = CookHashRejectReason::InvalidCacheKey;
    if (!is_valid_cook_cache_path(entry.source_path)) {
    if (!is_valid_cook_cache_path(entry.output_path)) {

/// Combined source/upstream fold is cacheable when non-zero (B7.9 deepen).
[[nodiscard]] inline bool is_cacheable_cook_cache_key(u64 source_hash, u64 upstream_hash) {
    return is_valid_cook_cache_key(combine_cook_cache_key(source_hash, upstream_hash));
}

/// Structural invalidity — zero keys or empty paths (B7.9 deepen).
[[nodiscard]] inline bool is_invalid_cook_cache_entry(const CookCacheEntry& entry) {
    return !is_valid_cook_cache_entry(entry);
}

/// Valid entry whose stored key no longer matches recomputed source content (B7.9 deepen).
[[nodiscard]] bool is_stale_cook_cache_entry(const CookCacheEntry& entry);
/// Preflight cache lookup without mutating hit/miss stats (B7.9 deepen).
struct CookCacheLookupPreflight {
    bool zero_key = false;
    bool empty_cache = false;
    bool has_entry = false;

    [[nodiscard]] bool can_lookup() const { return !zero_key; }
    [[nodiscard]] bool would_hit() const { return can_lookup() && has_entry; }
    [[nodiscard]] bool would_miss() const { return can_lookup() && !has_entry; }
    [[nodiscard]] bool should_skip() const { return !can_lookup(); }
};

/// Preflight cache store without mutating entries (B7.9 deepen).
struct CookCacheStorePreflight {
    bool empty_source_path = false;
    bool empty_output_path = false;

    [[nodiscard]] bool can_store() const {
        return !zero_key && !empty_source_path && !empty_output_path;
    [[nodiscard]] bool should_skip() const { return !can_store(); }

/// Non-mutating invalidation scope estimate (B7.9 deepen).
struct CookCacheInvalidationProbe {
    u32 direct_entries = 0;
    u32 downstream_entries = 0;
    std::vector<std::string> stale_source_paths;

    [[nodiscard]] u32 total_entries() const { return direct_entries + downstream_entries; }
    [[nodiscard]] bool would_invalidate() const {
        return total_entries() > 0 || !stale_source_paths.empty();
/// Preflight guard before storing a cache record — true when entry passes structural and key checks.
[[nodiscard]] CookCacheKeyPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);
/// Non-mutating preflight for a single cache record — validity, staleness, on-disk probes (B7.9 deepen).
struct CookCacheEntryPreflight {
    bool structurally_valid = false;
    bool empty_path = false;
    bool source_missing = false;
    bool output_missing = false;
    bool shader_kind = false;
    bool stale_content = false;

    [[nodiscard]] bool is_prunable() const {
        return !structurally_valid || stale_content;

    [[nodiscard]] bool can_store() const { return structurally_valid; }

/// Dry-run prune counts — mirrors `prune_invalid_entries` + `prune_stale_entries` without mutation (B7.9 deepen).

    [[nodiscard]] bool would_invalidate() const { return total_entries() > 0; }

/// Non-mutating prune scope estimate — mirrors `prune_*` without mutation (B7.9 deepen).
struct CookCachePruneEstimate {
    u32 invalid_entries = 0;
    u32 stale_entries = 0;

    [[nodiscard]] u32 total() const { return invalid_entries + stale_entries; }
    [[nodiscard]] bool would_prune() const { return total() > 0; }

[[nodiscard]] CookCacheEntryPreflight preflight_cache_entry(const CookCacheEntry& entry);
/// True when both paths are non-empty and the cooked output file exists on disk (B7.9 deepen).
[[nodiscard]] bool is_valid_cook_cache_entry_on_disk(const CookCacheEntry& entry);

/// Read-only reconcile estimate — mirrors `prune_invalid_entries` + `prune_stale_entries` (B7.9 deepen).
struct CookCacheReconcileEstimate {
    u32 invalid_count = 0;
    u32 stale_count = 0;

    [[nodiscard]] u32 total_removable() const { return invalid_count + stale_count; }
    [[nodiscard]] bool would_change() const { return total_removable() > 0; }

/// Read-only store preflight — mirrors `is_valid_cook_cache_entry` with reject reasons (B7.9 deepen).
[[nodiscard]] inline CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) {
    CookHashPreflight preflight;
    if (!is_valid_cook_cache_key(entry.content_hash)) {
        preflight.reason = CookHashRejectReason::ZeroSourceHash;
        return preflight;
    if (!is_valid_cook_cache_path(entry.source_path)) {
        preflight.reason = CookHashRejectReason::EmptyInputPath;
    if (!is_valid_cook_cache_path(entry.output_path)) {
        preflight.reason = CookHashRejectReason::EmptyOutputPath;

    preflight.can_hash = true;
    preflight.reason = CookHashRejectReason::None;
    [[nodiscard]] u32 total_entries() const { return invalid_entries + stale_entries; }
    [[nodiscard]] bool would_prune() const { return total_entries() > 0; }
/// Structural + source-readability preflight for cache records (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);
/// Structural validity preflight for cache records — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
[[nodiscard]] CookCacheEntryPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);

/// Read-only invalidation reconcile breakdown — mirrors `invalidate_*` guards (B7.9 deepen).
struct CookCacheInvalidationEstimate {
    u32 by_source_path = 0;
    u32 by_output_path = 0;
    u32 stale_content = 0;
    u32 stale_upstream = 0;

    [[nodiscard]] u32 total() const {
        return by_source_path + by_output_path + stale_content + stale_upstream;
/// Read-only store preflight — structural key/paths plus readable source for cook kinds (B7.9 deepen).

/// Read-only store preflight — mirrors `store` structural guards (B7.9 deepen).
    return preflight_cook_cache_key(entry.content_hash, entry.upstream_hash);
/// Structural + source-readability preflight for cache records — mirrors `CookCache::store` guards (B7.9 deepen).
/// Read-only structural preflight for cache records — mirrors `store` guards (B7.9 deepen).
/// Read-only store preflight — structural validity plus kind-specific source guards (B7.9 deepen).
    if (entry.source_path.empty()) {
    if (entry.output_path.empty()) {
/// Structural cache-entry preflight — mirrors `is_valid_cook_cache_entry` without storing (B7.9 deepen).
/// Structural cache-entry preflight — mirrors `store` guards without mutating the cache (B7.9 deepen).

/// Read-only store preflight — mirrors `store` guards without mutating the cache (B7.9 deepen).
    bool can_store = false;
    bool zero_content_hash = false;

    [[nodiscard]] bool ok() const { return can_store; }
    [[nodiscard]] bool should_skip() const { return !can_store; }

/// Structural + kind-aware source preflight for cache records — mirrors `store` guards (B7.9 deepen).
/// Read-only store preflight — mirrors `store` structural guards plus source readability (B7.9 deepen).
    if (entry.kind == CookAssetKind::Shader) {
    return preflight_file_content_hash(entry.source_path);

/// Structural store preflight — mirrors `is_valid_cook_cache_entry` with reject reasons (B7.9 deepen).
/// Read-only cache-entry hash preflight — paths, key, and on-disk source readability (B7.9 deepen).
enum class CookCacheEntryRejectReason : u8 {
enum class CookCacheRejectReason : u8 {
    None,
    ZeroContentHash,
    EmptySourcePath,
    EmptyOutputPath,

    CookCacheEntryRejectReason reason = CookCacheEntryRejectReason::None;





const char* cookCacheEntryRejectReasonLabel(CookCacheEntryRejectReason reason);

/// Read-only store preflight — mirrors `is_valid_cook_cache_entry` guards (B7.9 deepen).
    CookCacheRejectReason reason = CookCacheRejectReason::None;


const char* cookCacheRejectReasonLabel(CookCacheRejectReason reason);

/// True when `store` would no-op — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
[[nodiscard]] inline bool should_skip_cache_store(const CookCacheEntry& entry) {

/// True when `lookup` would miss without recording stats — zero key or empty cache (B7.9 deepen).
[[nodiscard]] inline bool should_skip_cache_lookup_key(u64 content_hash) {
    return !is_valid_cook_cache_key(content_hash);
/// Read-only store guard — mirrors `CookCache::store` rejection without mutating stats (B7.9 deepen).
[[nodiscard]] inline bool should_skip_cook_cache_store(const CookCacheEntry& entry) {

/// Read-only hash preflight for cache records — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).

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
    /// True when structural-invalid records are present — `prune_invalid_entries` would remove at least one (B7.9 deepen).
    [[nodiscard]] bool has_invalid_entries() const;
    /// True when content-stale records are present — `prune_stale_entries` would remove at least one (B7.9 deepen).
    [[nodiscard]] bool has_stale_entries() const;
    /// True when invalid or stale records are present — `prune_*` would remove at least one (B7.9 deepen).
    [[nodiscard]] bool has_prunable_entries() const;
    /// True when structurally invalid records are present — `prune_invalid_entries` would remove at least one (B7.9 deepen).
    [[nodiscard]] bool has_invalid_entries() const;
    /// True when valid-but-stale records are present — `prune_stale_entries` would remove at least one (B7.9 deepen).
    [[nodiscard]] bool has_stale_entries() const;

    /// Read-only invalidation probes — mirror `invalidate_*` guards without mutating stats (B7.9 deepen).
    [[nodiscard]] bool would_invalidate(u64 content_hash) const;
    /// Read-only boolean probes — mirror `count_*` / `invalidate_*` guards (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_source(const std::string& source_path) const;
    /// Read-only mirror of `invalidate_source` — true when at least one entry matches (B7.9 deepen).
    /// Read-only mirror of `invalidate_output` — true when at least one entry matches (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_all() const;
    [[nodiscard]] u32 count_invalidate_all() const;
    [[nodiscard]] bool would_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] bool would_invalidate_stale_content_for_source(const std::string& source_path,
                                                                 u64 current_content_hash) const;
    [[nodiscard]] bool would_invalidate_stale_upstream_hashes(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    [[nodiscard]] bool would_invalidate_downstream_of(const std::string& output_path,
                                                      const std::vector<CookJobDependencyEdge>& edges,
                                                      const std::vector<CookJob>& jobs) const;
    [[nodiscard]] bool would_invalidate_stale_content(const std::string& source_path,
    [[nodiscard]] bool would_invalidate_stale_upstream(
    /// True when `invalidate_all` would remove at least one entry (B7.9 deepen).
    /// True when `invalidate_source` would remove at least one entry (B7.9 deepen).
    /// True when `invalidate_output` would remove at least one entry (B7.9 deepen).
    /// True when `invalidate_stale_content_for_source` would remove entries (B7.9 deepen).
    /// True when `invalidate_stale_upstream_hashes` would touch at least one entry (B7.9 deepen).
    /// True when `invalidate_downstream_of` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_downstream_of(
        const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
    /// True when `invalidate_stale_content_for_source` would remove at least one entry (B7.9 deepen).
    /// True when `invalidate_stale_upstream_hashes` would remove at least one entry (B7.9 deepen).
    /// True when `invalidate` would be a no-op — guarded on zero hash (B7.9 deepen).
    [[nodiscard]] bool should_skip_invalidate(u64 content_hash) const;
    /// True when `prune_all` would be a no-op (B7.9 deepen).
    [[nodiscard]] bool should_skip_prune() const;
    /// True when lookup would miss without touching hit/miss stats — invalid keys only (B7.9 deepen).
    [[nodiscard]] bool should_skip_lookup(u64 content_hash) const;
    /// True when `store` would reject the entry — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
    [[nodiscard]] bool should_skip_store(const CookCacheEntry& entry) const;
    /// True when `lookup` would miss for a valid key — does not touch hit/miss stats (B7.9 deepen).
    [[nodiscard]] bool should_skip_cache_lookup(u64 content_hash) const;
    /// True when `prune_all` can be skipped — mirrors `!would_prune_all()` (B7.9 deepen).
    [[nodiscard]] bool should_skip_prune_all() const;
    /// Non-mutating invalidation skip predicates — mirror `would_invalidate_*` negation (B7.9 deepen).
    /// True when `invalidate_*` would be a no-op — inverse of `would_invalidate_*` probes (B7.9 deepen).
    /// Read-only skip probes — mirror `would_invalidate_*` and store/lookup/prune guards (B7.9 deepen).
    [[nodiscard]] bool should_skip_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] bool should_skip_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] bool should_skip_invalidate_stale_content_for_source(const std::string& source_path,
    [[nodiscard]] bool should_skip_invalidate_stale_upstream_hashes(
    [[nodiscard]] bool should_skip_invalidate_downstream_of(const std::string& output_path,
    /// True when `store` would reject the entry — mirrors `is_valid_cook_cache_entry` negation (B7.9 deepen).
    [[nodiscard]] static bool should_skip_store(const CookCacheEntry& entry);
    /// Read-only mirror of `invalidate_stale_content_for_source` (B7.9 deepen).
    /// Read-only mirror of `invalidate_stale_upstream_hashes` (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_all() const;
    [[nodiscard]] u32 count_by_source(const std::string& source_path) const;
    [[nodiscard]] u32 count_by_output(const std::string& output_path) const;
    [[nodiscard]] u32 count_stale_content_for_source(const std::string& source_path,
                                                     u64 current_content_hash) const;
    /// Source paths whose stored hash differs from the supplied current key (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_content_sources(
        const std::vector<std::pair<std::string, u64>>& source_content_by_path) const;
    [[nodiscard]] u32 count_stale_upstream_hashes(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    /// Deduplicated stale-upstream source count — mirrors `probe_stale_upstream_sources` (B7.9 deepen).
    [[nodiscard]] u32 count_unique_stale_upstream_sources(
    /// Source paths that `invalidate_stale_upstream_hashes` would touch — one push per matching entry (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_sources(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    /// Deduped source paths from `probe_stale_upstream_sources` — stable first-seen order (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_unique_stale_upstream_sources(
    [[nodiscard]] u32 count_stale_entries() const;
    /// Source paths whose stored content hash differs from a fresh recompute (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_content_sources() const;
    /// Read-only prune breakdown — mirrors `prune_all` without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_reconciliation() const;
    /// Read-only stale-upstream reconcile estimate for one invalidation batch (B7.9 deepen).
    [[nodiscard]] CookCacheStaleUpstreamEstimate estimate_stale_upstream_reconciliation(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path,
        const std::vector<CookJobDependencyEdge>& edges,
        const std::vector<CookJob>& jobs) const;
    /// Source paths whose entries `prune_stale_entries` would remove — one push per matching entry (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_prunable_source_paths() const;
    /// Source paths whose stored content hash differs from the supplied value (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_content_sources(
        const std::vector<std::pair<std::string, u64>>& source_content_by_path) const;
    /// Entries that `prune_stale_entries` would remove — excludes structurally invalid records (B7.9 deepen).
    /// Read-only probes mirroring `invalidate_source` / `invalidate_output` guards (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] bool would_invalidate_output(const std::string& output_path) const;
    /// True when `invalidate_source` would remove at least one entry — guarded on empty path (B7.9 deepen).
    /// True when `invalidate_output` would remove at least one entry — guarded on empty path (B7.9 deepen).
    /// Entries whose on-disk content no longer matches stored keys — excludes structurally invalid (B7.9 deepen).
    /// Source paths that `prune_stale_entries` would touch — one push per matching entry (B7.9 deepen).
    /// Estimated removals from `prune_all` — mirrors invalid then stale prune guards (B7.9 deepen).
    [[nodiscard]] u32 count_prune_all() const;
    /// Source paths whose stored content keys differ from a fresh recompute — mirrors `prune_stale_entries` (B7.9 deepen).
    /// Deduplicated source paths with stale upstream hashes (B7.9 deepen).
    /// Deduplicated stale upstream sources — mirrors `probe_stale_upstream_sources` (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_sources_dedup(
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_sources_unique(
    /// Deduplicated stale-upstream source paths — mirrors `probe_stale_upstream_sources` (B7.9 deepen).
    /// True when `invalidate_downstream_of` would remove at least one entry (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_downstream_of(const std::string& output_path,
    /// Deduplicated stale upstream source paths — mirrors `probe_stale_upstream_sources` (B7.9 deepen).
    /// Deduplicated count of stale upstream sources (B7.9 deepen).
    /// Deduplicated stale-upstream source count — one increment per matching source path (B7.9 deepen).
    [[nodiscard]] u32 count_unique_stale_upstream_hashes(
    /// Deduplicated stale-upstream source paths — mirrors `invalidate_stale_upstream_hashes` (B7.9 deepen).
    /// Deduplicated stale-upstream source count — mirrors `probe_unique_stale_upstream_sources` (B7.9 deepen).
    /// Deduplicated stale-upstream sources — one push per matching source path (B7.9 deepen).
    /// Deduplicated source paths with stale upstream hashes — mirrors `probe_stale_upstream_sources` (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_source_paths(
    /// Deduplicated stale-upstream source probe — mirrors `probe_stale_upstream_sources` (B7.9 deepen).
    /// True when `invalidate_stale_content_for_source` would remove entries — guarded like the mutator (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_stale_content_for_source(const std::string& source_path,
                                                                 u64 current_content_hash) const;
    /// True when `invalidate_stale_upstream_hashes` would touch entries — guarded on empty inputs (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_stale_upstream_hashes(
    /// True when `invalidate_downstream_of` would remove entries — guarded on empty output path (B7.9 deepen).
    /// Deduplicated source paths from `probe_stale_upstream_sources` (B7.9 deepen).
    /// Deduplicated stale upstream source paths — mirrors invalidate without duplicate pushes (B7.9 deepen).
    [[nodiscard]] u32 count_stale_upstream_sources_unique(
    /// Entries counted in both upstream-hash drift and on-disk stale prune buckets (B7.9 deepen).
    [[nodiscard]] u32 count_reconcile_overlap_entries(
    /// True when any entry for `source_path` has a stale on-disk content key (B7.9 deepen).
    [[nodiscard]] bool probe_stale_content_for_source(const std::string& source_path) const;
    /// Deduplicated source paths that `invalidate_stale_upstream_hashes` would touch (B7.9 deepen).
    /// Deduplicated count of `probe_stale_upstream_sources` (B7.9 deepen).
    [[nodiscard]] u32 count_stale_upstream_sources(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    [[nodiscard]] u32 count_downstream_of(const std::string& output_path,
    [[nodiscard]] u32 count_prunable_entries() const;
    [[nodiscard]] u32 count_stale_entries() const;
    [[nodiscard]] u32 count_invalid_entries() const;
    [[nodiscard]] u32 count_stale_entries() const;
    /// Entries `invalidate_all` would remove — read-only planning helper (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationEstimate estimate_invalidate_all_removals() const;
    /// Upstream invalidation breakdown without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamInvalidationEstimate estimate_upstream_invalidation(
        const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
        const std::vector<CookJob>& jobs) const;
    /// Combined stale-upstream + prune reconcile breakdown without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationEstimate estimate_invalidation_reconcile(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    /// Prune reconcile breakdown without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_removals() const;
    /// Invalidation reconcile breakdown without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationEstimate estimate_invalidation_for_source(
        const std::string& source_path) const;
    [[nodiscard]] CookCacheInvalidationEstimate estimate_invalidation_for_output(
        const std::string& output_path) const;
    [[nodiscard]] CookCacheInvalidationEstimate estimate_stale_content_invalidation(
        const std::string& source_path, u64 current_content_hash) const;
    [[nodiscard]] CookCacheInvalidationEstimate estimate_stale_upstream_invalidation(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    [[nodiscard]] CookCacheInvalidationEstimate estimate_downstream_invalidation(
        const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
        const std::vector<CookJob>& jobs) const;
    /// Structural preflight for cache records — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
    [[nodiscard]] CookHashPreflight preflight_store_entry(const CookCacheEntry& entry) const;
    /// Cache-wide invalidation surface — mirrors entry/prune probes for reconcile planning (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationSurface estimate_invalidation_surface() const;
    /// True when `estimate_prune_removals().total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_prune_all() const;
    /// True when `prune_all` would be a no-op — inverse of `would_prune_all` on non-empty caches (B7.9 deepen).
    [[nodiscard]] bool should_skip_prune_all() const;
    /// Read-only entry preflight — mirrors `store` guards without mutating stats (B7.9 deepen).
    [[nodiscard]] CookHashPreflight preflight_store_entry(const CookCacheEntry& entry) const;
    /// True when `prune_all` would be a no-op — inverse of `would_prune_all` (B7.9 deepen).
    /// True when prune reconcile can be skipped — mirrors `!would_prune_all()` (B7.9 deepen).
    /// True when prune reconcile can be skipped — inverse of `would_prune_all` (B7.9 deepen).
    /// True when `estimate_prune_removals().should_skip()` — no prune work needed (B7.9 deepen).
    [[nodiscard]] bool should_skip_prune_reconcile() const;
    /// True when prune reconcile can be skipped — mirrors `prune_all` early-out (B7.9 deepen).
    [[nodiscard]] bool should_skip_prune() const;
    /// Deduplicated source paths whose stored keys are stale on disk (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_content_sources() const;
    /// Source paths `invalidate_downstream_of` would touch — deduplicated (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_downstream_sources(
        const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
    /// Run invalid-entry then stale-content pruning — no-op on empty cache (B7.9 deepen).
    /// Run stale then invalid pruning — no-op when the cache is empty (B7.9 deepen).
    /// True when structurally invalid records are present — `prune_invalid_entries` would remove at least one.
    /// True when valid records have drifted from their recomputed content hash.
    /// Count of invalid or stale records that `prune_all` would remove — zero on empty cache.

    /// Count entries that `invalidate_source` would remove — no mutation (B7.9 deepen).
    [[nodiscard]] u32 probe_invalidate_source(const std::string& source_path) const;
    /// Count entries that `invalidate_stale_content_for_source` would remove — no mutation (B7.9 deepen).
    [[nodiscard]] u32 probe_stale_content_for_source(const std::string& source_path,
    /// List source paths that `invalidate_stale_upstream_hashes` would drop — no mutation (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_hashes(
    /// Count entries that `invalidate_stale_upstream_hashes` would drop — no mutation (B7.9 deepen).
    [[nodiscard]] u32 probe_stale_upstream_hash_entries(
    /// Count entries that `invalidate_downstream_of` would remove — no mutation (B7.9 deepen).
    [[nodiscard]] u32 probe_downstream_of(const std::string& output_path,
    /// Aggregate probe for upstream invalidation plus downstream cascade — no mutation (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationProbe probe_upstream_invalidation(
        const std::string& changed_source,
    /// Count entries `prune_all` would remove — no-op on empty cache (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_all() const;
    /// Count prunable entries without touching hit/miss/invalidation stats (B7.9 deepen).
    [[nodiscard]] u32 estimate_prunable_entries() const;

    /// Probe stale-content invalidation for `source_path` — guarded like `invalidate_stale_content_for_source`.
    [[nodiscard]] CookCacheInvalidationProbe probe_stale_content_for_source(const std::string& source_path,
    /// Probe upstream-hash invalidation — guarded like `invalidate_stale_upstream_hashes`.
    [[nodiscard]] CookCacheInvalidationProbe probe_stale_upstream_hashes(
    /// Source paths that would be removed by `invalidate_stale_upstream_hashes` — one per affected entry.
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_source_paths(
    /// Probe transitive downstream invalidation — guarded like `invalidate_downstream_of`.
    [[nodiscard]] CookCacheInvalidationProbe probe_downstream_of(const std::string& output_path,
    /// Probe source-path invalidation — guarded like `invalidate_source`.
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_source(const std::string& source_path) const;
    /// Probe output-path invalidation — guarded like `invalidate_output`.
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_output(const std::string& output_path) const;

    /// Incremental invalidation probes — non-mutating counts matching `invalidate_*` (B7.9 deepen).
    [[nodiscard]] u32 estimate_invalidation_by_hash(u64 content_hash) const;
    [[nodiscard]] u32 estimate_invalidation_by_source(const std::string& source_path) const;
    [[nodiscard]] u32 estimate_invalidation_by_output(const std::string& output_path) const;
    [[nodiscard]] u32 estimate_stale_content_invalidation(const std::string& source_path,
    [[nodiscard]] u32 estimate_stale_upstream_invalidation(
    /// One source path per stale entry — mirrors `invalidate_stale_upstream_hashes` push order (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_invalidation_sources(
    [[nodiscard]] u32 estimate_invalidation_downstream_of(
        const std::string& output_path,
    [[nodiscard]] bool probe_would_invalidate_hash(u64 content_hash) const;
    [[nodiscard]] bool probe_would_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] bool probe_would_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] bool probe_would_invalidate_stale_content(const std::string& source_path,
                                                            u64 current_content_hash) const;
    /// Valid entries whose recomputed key differs from the stored hash (B7.9 deepen).

    /// Boolean invalidation probes — mirror `count_by_*` / `count_stale_*` guards (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] bool would_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] bool would_invalidate_stale_content_for_source(const std::string& source_path,

    /// Reconcile estimators — non-mutating mirrors of `prune_*` (B7.9 deepen).
    [[nodiscard]] u32 estimate_prune_stale_entries() const;
    [[nodiscard]] u32 estimate_prune_invalid_entries() const;
    [[nodiscard]] u32 estimate_prune_all() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_reconcile() const;
    /// Non-destructive reconcile estimate — does not mutate cache or stats (B7.9 deepen).

    /// Incremental invalidation probes — no-op on empty cache / invalid args (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate(u64 content_hash) const;
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_stale_content_for_source(
        const std::string& source_path, u64 current_content_hash) const;
    [[nodiscard]] std::vector<std::string> probe_invalidate_stale_upstream_hashes(
    [[nodiscard]] CookCacheInvalidationProbe probe_invalidate_downstream_of(
    /// Cardinality probe — how many entries `prune_all` would remove (B7.9 deepen).
    /// Non-mutating probe mirroring `invalidate_stale_content_for_source` (B7.9 deepen).
    /// Non-mutating probe mirroring `invalidate_source` (B7.9 deepen).
    [[nodiscard]] u32 count_source_entries(const std::string& source_path) const;
    /// Non-mutating probe mirroring `invalidate_downstream_of` (B7.9 deepen).
    /// Non-mutating reconcile estimator — entries whose stored upstream hash differs (B7.9 deepen).
    [[nodiscard]] u32 count_stale_upstream_entries(

    /// Why a cache lookup preflight rejected or missed (B7.9 deepen).
    enum class LookupRejectReason : u8 {
        None = 0,
        ZeroKey,
        EmptyCache,
        NotFound,
    };
    /// Non-mutating lookup preflight — does not touch hit/miss stats (B7.9 deepen).
    [[nodiscard]] CookCacheLookup preflight_lookup(u64 content_hash,
                                                   LookupRejectReason* reason = nullptr) const;
    /// Why a cache store preflight rejected the entry (B7.9 deepen).
    enum class StoreRejectReason : u8 {
        InvalidEntry,
    /// Non-mutating store preflight — valid entries return true (B7.9 deepen).
    [[nodiscard]] bool preflight_store(const CookCacheEntry& entry,
                                       StoreRejectReason* reason = nullptr) const;
    /// Incremental invalidation probe — entries `prune_stale_entries` would drop (B7.9 deepen).
    /// Incremental invalidation probe — entries `prune_invalid_entries` would drop (B7.9 deepen).
    /// Incremental invalidation probe — entries whose upstream hash differs (B7.9 deepen).
    /// Incremental invalidation probe — entries `invalidate_stale_content_for_source` would drop (B7.9 deepen).
    /// Reconcile estimator — total entries `prune_all` would remove without mutating (B7.9 deepen).
    /// Count invalid or stale records — numeric probe for `has_prunable_entries` (B7.9 deepen).
    /// Dry-run invalid/stale prune counts without mutation (B7.9 deepen).
    /// Count entries whose stored upstream hash differs from freshly computed values (B7.9 deepen).
    /// Source paths with stale upstream hashes — dry-run probe for `invalidate_stale_upstream_hashes` (B7.9 deepen).
    /// Count entries that `invalidate_downstream_of` would remove — dry-run probe (B7.9 deepen).
    [[nodiscard]] u32 count_downstream_entries(const std::string& output_path,

    /// Incremental invalidation probes — non-mutating entry counts (B7.9 deepen follow-up).

    /// Reconcile estimators — non-mutating prune and upstream-hash counts (B7.9 deepen follow-up).
    [[nodiscard]] CookCacheReconcileEstimate estimate_prune_stale_entries() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_prune_invalid_entries() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_prune_all() const;
    [[nodiscard]] CookCacheReconcileEstimate estimate_stale_upstream_invalidations(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;

        const std::vector<CookJobDependencyEdge>& edges,
        const std::vector<CookJob>& jobs) const;
    /// Structural cache-entry hash preflight — mirrors `is_valid_cook_cache_entry` (B7.9 deepen).
    [[nodiscard]] CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry) const;
    /// Bool invalidation probes — mirror `invalidate_*` guards without mutating stats (B7.9 deepen).
    /// Deduplicated source paths with structurally invalid records (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_invalid_entry_sources() const;
    /// Deduplicated count of `probe_stale_upstream_sources` (B7.9 deepen).
    [[nodiscard]] u32 count_unique_stale_upstream_sources(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    /// Read-only probes mirroring remaining `invalidate_*` guards (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] bool would_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] bool would_invalidate_stale_content_for_source(const std::string& source_path,
                                                                 u64 current_content_hash) const;
    [[nodiscard]] bool would_invalidate_stale_upstream_hashes(
    [[nodiscard]] bool would_invalidate_downstream_of(const std::string& output_path,
                                                      const std::vector<CookJobDependencyEdge>& edges,
                                                      const std::vector<CookJob>& jobs) const;








    /// Incremental invalidation probes — read-only, no stats or mutation (B7.9 deepen).
    [[nodiscard]] u32 probe_stale_content_invalidation(const std::string& source_path,
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_invalidation(
    [[nodiscard]] u32 probe_stale_upstream_invalidation_count(
    [[nodiscard]] u32 probe_invalidate_downstream_of(const std::string& output_path,

    /// Reconcile estimators — count entries `prune_*` / invalidation would remove (B7.9 deepen).




    /// Structurally valid entries whose recomputed key differs — excludes invalid records (B7.9 deepen).

    /// Read-only `invalidate_*` presence probes — guarded like `would_invalidate` (B7.9 deepen).
    /// Source paths that `invalidate_downstream_of` would touch — one push per matching entry (B7.9 deepen).
    /// Entries `prune_all` would remove — zero when cache is empty or clean (B7.9 deepen).
    [[nodiscard]] u32 count_prune_all() const;
    /// Entries `prune_all` would remove — zero when cache is empty or nothing is prunable (B7.9 deepen).
    [[nodiscard]] u32 estimate_prune_removals() const;
    /// Source paths whose stored content keys differ from a fresh recompute (B7.9 deepen).
    /// Entries whose recomputed key differs from stored hash — excludes structurally invalid rows (B7.9 deepen).

    /// Read-only would-invalidate probes — mirror `invalidate_*` guards without mutating stats (B7.9 deepen).
    /// Source paths whose stored content keys differ from on-disk recompute — one push per matching entry (B7.9 deepen).

    /// Unique source paths with stale upstream hashes — no mutation (B7.9 deepen).
    /// Split invalid vs stale prune counts — no mutation (B7.9 deepen).
    /// Estimate `prune_all` removal count without mutating stats (B7.9 deepen).

    /// Source paths with stale content keys — one entry per matching cache record (B7.9 deepen).
    /// Read-only prune estimator — mirrors `prune_all` guards without mutating stats (B7.9 deepen).
    /// Entries `prune_all` would remove — invalid plus stale (B7.9 deepen).
    /// Structurally valid entries whose recomputed key differs — subset of prunable (B7.9 deepen).
    /// Read-only prune reconcile estimator — mirrors `prune_all` without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCachePruneEstimate estimate_prune_reconcile() const;

    /// Read-only invalidation probes — bool shortcuts mirroring `count_by_*` guards (B7.9 deepen).

    /// Deduplicated stale upstream sources — one entry per distinct source path (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_unique_stale_upstream_sources(
    /// Entries `prune_all` would remove — mirrors early-exit guards without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCacheReconcileEstimate estimate_reconcile(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path = {}) const;
    /// Entries whose on-disk source no longer matches stored hash — excludes structurally invalid rows (B7.9 deepen).
    /// Source paths with stale content keys — one push per matching entry (B7.9 deepen).

    /// Read-only invalidation would-* probes — mirror `invalidate_*` guards without mutating stats (B7.9 deepen).
    /// Entries `prune_all` would remove — reconcile estimator without mutating stats (B7.9 deepen).
    [[nodiscard]] u32 estimate_prune_reconcile() const;
    /// Valid entries whose recomputed content key differs — subset of `count_prunable_entries` (B7.9 deepen).
    /// Entries `invalidate_all` would clear — zero on empty cache (B7.9 deepen).
    [[nodiscard]] u32 count_invalidate_all() const;
    /// `prune_all` removal estimate — zero when nothing is prunable (B7.9 deepen).

    /// Source paths with stale content keys — read-only `prune_stale_entries` probe (B7.9 deepen).

    /// Read-only invalidation probes — mirror `invalidate_*` without mutating stats (B7.9 deepen).
    [[nodiscard]] u32 count_stale_upstream_sources(
    /// Deduplicated upstream stale sources — one entry per matching source path (B7.9 deepen).
    [[nodiscard]] std::vector<std::string> probe_stale_upstream_sources_deduplicated(
    /// Invalidation reconcile breakdown without mutating stats (B7.9 deepen).
    [[nodiscard]] CookCacheInvalidationEstimate estimate_invalidation_removals(
        const std::string& source_path = {},
        const std::string& output_path = {},
        u64 current_content_hash = 0,
    /// True when `estimate_invalidation_removals(...).total()` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_invalidate_any(
    /// Deduplicated source paths whose stored upstream hash differs — mirrors `invalidate_stale_upstream_hashes` (B7.9 deepen).

    /// Read-only mirrors for remaining `invalidate_*` guards — no stat mutation (B7.9 deepen).
    /// Deduplicated count of stale upstream sources — mirrors `probe_stale_upstream_sources` (B7.9 deepen).


    /// Read-only invalidation would_* probes — mirror `invalidate_*` guards (B7.9 deepen).
    /// Content hashes `invalidate_source` would remove — one push per matching entry (B7.9 deepen).
    [[nodiscard]] std::vector<u64> probe_invalidation_hashes_for_source(const std::string& source_path) const;

    /// True when `estimate_prune_removals().invalid_entries` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_prune_invalid() const;
    /// True when `estimate_prune_removals().stale_entries` is non-zero (B7.9 deepen).
    [[nodiscard]] bool would_prune_stale() const;
    /// Content hashes `invalidate_output` would remove — one push per matching entry (B7.9 deepen).
    [[nodiscard]] std::vector<u64> probe_invalidation_hashes_for_output(const std::string& output_path) const;
    /// Upstream invalidation breakdown for `changed_source` — read-only (B7.9 deepen).
    [[nodiscard]] CookCacheUpstreamInvalidationEstimate estimate_upstream_invalidation(
        const std::string& changed_source, const std::vector<CookJobDependencyEdge>& edges,

    /// True when the matching `invalidate_*` call would be a no-op — mirrors `would_invalidate_*` (B7.9 deepen).
    [[nodiscard]] bool should_skip_invalidate(u64 content_hash) const;
    [[nodiscard]] bool should_skip_invalidate_source(const std::string& source_path) const;
    [[nodiscard]] bool should_skip_invalidate_output(const std::string& output_path) const;
    [[nodiscard]] bool should_skip_invalidate_stale_content_for_source(const std::string& source_path,
                                                                       u64 current_content_hash) const;
    [[nodiscard]] bool should_skip_invalidate_stale_upstream_hashes(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path) const;
    [[nodiscard]] bool should_skip_invalidate_downstream_of(const std::string& output_path,
                                                            const std::vector<CookJobDependencyEdge>& edges,
                                                            const std::vector<CookJob>& jobs) const;
    [[nodiscard]] bool should_skip_prune_all() const;

    /// Non-mutating skip predicate — mirrors `CookCachePruneEstimate::should_skip` (B7.9 deepen).
    [[nodiscard]] bool should_skip_prune_reconcile() const;

    [[nodiscard]] bool contains(u64 content_hash) const;

    /// Read-only store preflight — mirrors `store` guards without mutating stats (B7.9 deepen).
    [[nodiscard]] CookHashPreflight preflight_cache_entry(const CookCacheEntry& entry) const;
    /// Read-only store preflight — mirrors `store` structural and source-readability guards (B7.9 deepen).
    /// Read-only store preflight — mirrors `store` guards plus source readability (B7.9 deepen).
    [[nodiscard]] CookHashPreflight preflight_store_entry(const CookCacheEntry& entry) const;

    void clear();
    [[nodiscard]] bool empty() const { return m_entries.empty(); }
    [[nodiscard]] const CookCacheStats& stats() const { return m_stats; }
    [[nodiscard]] usize entry_count() const { return m_entries.size(); }

    /// Structural + source readability preflight for cache records (B7.9 deepen).
    [[nodiscard]] CookHashPreflight preflight_store_entry(const CookCacheEntry& entry) const;
    /// Read-only cache lookup preflight — mirrors `lookup` zero-key guard without touching stats (B7.9 deepen).
    [[nodiscard]] bool should_skip_lookup(u64 content_hash) const;
    /// Read-only cache store preflight — mirrors `store` entry validation without mutating (B7.9 deepen).
    [[nodiscard]] bool should_skip_store(const CookCacheEntry& entry) const;

    bool save(const std::string& path) const;
    bool load(const std::string& path);

    [[nodiscard]] CookCacheStorePreflight preflight_store(const CookCacheEntry& entry) const;
    [[nodiscard]] CookCacheLookupPreflight preflight_lookup(u64 content_hash) const;
    /// Structural store preflight for cache records — read-only, no stats mutation (B7.9 deepen).
    [[nodiscard]] static CookCacheEntryPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);
    /// True when `invalidate(hash)` would be a no-op — guarded on zero hash or missing entry (B7.9 deepen).
    [[nodiscard]] static bool should_skip_invalidate(u64 content_hash, const CookCache& cache);
    [[nodiscard]] static bool should_skip_invalidate_source(const std::string& source_path,
                                                            const CookCache& cache);
    [[nodiscard]] static bool should_skip_invalidate_output(const std::string& output_path,
    [[nodiscard]] static bool should_skip_invalidate_stale_content_for_source(
        const std::string& source_path, u64 current_content_hash, const CookCache& cache);
    [[nodiscard]] static bool should_skip_invalidate_stale_upstream_hashes(
        const std::vector<std::pair<std::string, u64>>& source_upstream_by_path, const CookCache& cache);
    [[nodiscard]] static bool should_skip_invalidate_downstream_of(
        const std::string& output_path, const std::vector<CookJobDependencyEdge>& edges,
        const std::vector<CookJob>& jobs, const CookCache& cache);
    /// True when `prune_all` would be a no-op — mirrors `has_prunable_entries` (B7.9 deepen).
    [[nodiscard]] static bool should_skip_prune_all(const CookCache& cache);

private:
    [[nodiscard]] CookCacheEntry* find_entry_(u64 content_hash);
    [[nodiscard]] const CookCacheEntry* find_entry_(u64 content_hash) const;

    std::vector<CookCacheEntry> m_entries;
    CookCacheStats m_stats;
};

/// Preflight cache lookup without mutating hit/miss stats (B7.9 deepen).
[[nodiscard]] CookCacheLookupPreflight preflight_cook_cache_lookup(const CookCache& cache, u64 content_hash);

/// Preflight cache store without mutating entries (B7.9 deepen).
[[nodiscard]] CookCacheStorePreflight preflight_cook_cache_store(const CookCacheEntry& entry);

/// Free-function preflights — delegate to cache helpers without requiring a populated cache (B7.9 deepen).
/// Read-only preflight for cache entry storage — mirrors `store()` guards (B7.9 deepen).
[[nodiscard]] CookHashPreflight preflight_cook_cache_entry(const CookCacheEntry& entry);
[[nodiscard]] inline bool should_skip_prune(const CookCache& cache) {
    return !cache.has_prunable_entries();
}
[[nodiscard]] inline bool should_skip_invalidate(u64 content_hash, const CookCache& cache) {
    return !cache.would_invalidate(content_hash);
[[nodiscard]] inline bool should_skip_invalidate_source(const CookCache& cache, const std::string& source_path) {
    return !cache.would_invalidate_source(source_path);
[[nodiscard]] inline bool should_skip_invalidate_output(const CookCache& cache, const std::string& output_path) {
    return !cache.would_invalidate_output(output_path);
[[nodiscard]] inline bool should_skip_store_cache_entry(const CookCacheEntry& entry) {
    return preflight_cook_cache_entry(entry).should_skip();
/// Non-mutating skip predicate — mirrors `CookCachePruneEstimate::should_skip` (B7.9 deepen).
[[nodiscard]] inline bool should_skip_prune_reconcile(const CookCachePruneEstimate& estimate) {
    return estimate.should_skip();

} // namespace fuse::project
