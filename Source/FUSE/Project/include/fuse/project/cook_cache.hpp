#pragma once

#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

enum class CookCacheLookup : u8 {
    Hit,
    Miss,
};

struct CookCacheEntry {
    u64 content_hash = 0;
    std::string output_path;
    std::string source_path;
    CookAssetKind kind = CookAssetKind::Mesh;
};

struct CookCacheStats {
    u64 hits = 0;
    u64 misses = 0;
    u64 invalidations = 0;
};

/// Content-hashed cook output cache — identical source+desc hashes return cached records (B7.9 deepen stub).
class CookCache {
public:
    CookCacheLookup lookup(u64 content_hash, CookCacheEntry* out_entry = nullptr);
    void store(const CookCacheEntry& entry);

    bool invalidate(u64 content_hash);
    u32 invalidate_source(const std::string& source_path);
    /// Remove entries sourced from `output_path` and transitively invalidate dependents (B7.9 deepen).
    u32 invalidate_downstream_of(const std::string& output_path,
                                 const std::vector<CookJobDependencyEdge>& edges,
                                 const std::vector<CookJob>& jobs);
    void invalidate_all();

    void clear();
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
