// FUSE Relight RL-5.4: the world-space hash-grid radiance cache for the path tracer (docs/plans/FUSE_REMIX_PORT_PLAN.md
// §5.4 "Radiance cache (NRC replacement, default)", §5.8 row `radiance_cache_*`). The algorithm, the table layout and
// the cited papers are in kernels/radiance_cache_core.h; the path-tracer side (training records, path spread,
// termination) in kernels/radiance_cache_path.h.
//
// Per frame:
//   train     one training path per trainStride x trainStride pixel tile (the path tracer's core with kPtFlagRcTrain):
//             every scattering vertex becomes a record (position, facing normal, the radiance it scatters back);
//   update    the records are hashed into their cells (adaptive cell size by distance to the camera, 6 normal bins) and
//             accumulated with integer atomics (order-independent: CPU and GPU agree per cell);
//   resolve   each cell blends the frame's mean into its resolved radiance (exponential window of maxSamples samples),
//             ages, and is evicted after maxAge frames without samples;
//   query     the path tracer (kPtFlagRadianceCache) ends a path at a vertex after the G-buffer vertex once the path's
//             spread exceeds the vertex's cell size, with the cell's resolved radiance.
//
// CPU (the reference; kernel::Backend CpuReference == CpuParallel per cell):
//   RadianceCacheCpu rc;  rc.configure(settings);
//   rc.frame(scene, ptSettings, w, h, frameSeed, sampleBase);           // train + update + resolve
//   renderReference(scene, withRadianceCache(ptSettings), w, h, frameSeed, sampleBase, spp, img, backend, nullptr,
//                   nullptr, nullptr, rc.hook(w, h));
// GPU: RadianceCacheGpu (radiance_cache_gpu.hpp) records the same stages before "relight.pt.trace".
//
// Options (RL-0.6 registry; FUSE names - upstream Remix's NRC options configure NVIDIA's SDK and are not mirrored):
//   rtx.radianceCache.enable            the path tracer's paths end in the hash-grid cache (default off until the frame
//                                       renderer opts in; RadianceCacheSettings::fromOptions reads it)
//   rtx.radianceCache.capacityLog2      table slots = 2^capacityLog2 (48 bytes each)
//   rtx.radianceCache.cellSize          finest cell edge (world units)
//   rtx.radianceCache.cellPixels        target cell edge in pixels (adaptive level by distance)
//   rtx.radianceCache.trainStride       one training path per trainStride^2 pixels
//   rtx.radianceCache.minSamples        samples before a cell answers queries
//   rtx.radianceCache.maxSamples        temporal window (resolved sample count cap)
//   rtx.radianceCache.maxAge            frames without samples before eviction
//   rtx.radianceCache.minRoughness      only vertices at least this rough end in the cache
//   rtx.radianceCache.spreadThreshold   path spread / cell size at which a path may end
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::ptk {
struct PtRcHook;
}

namespace fuse::relight::render::pathtrace {

// Mirrors of pt_reference_types.h / radiance_cache_types.h.
inline constexpr u32 kPtFlagRadianceCache = 1024u;
inline constexpr u32 kPtFlagRcTrain = 2048u;
inline constexpr u32 kRcParamWords = 5u;
inline constexpr u32 kRcSlotWords = 12u;
inline constexpr u32 kRcHeaderWords = 64u;
inline constexpr u32 kRcRecordWords = 3u;
inline constexpr u32 kRcMaxVertices = 8u;
inline constexpr float kRcFixedScale = 16384.f;
enum RcCounter : u32 {
    kRcCounterQueries = 32u,
    kRcCounterHits = 33u,
    kRcCounterInserts = 34u,
    kRcCounterDropped = 35u,
    kRcCounterLive = 36u,
    kRcCounterEvicted = 37u,
    kRcCounterFresh = 38u,
};
inline constexpr u32 kRcFlagStats = 1u;

struct RadianceCacheOptions {
    FUSE_RELIGHT_OPTION("rtx.radianceCache", bool, enable, false,
                        "FUSE: end path-traced paths in the world-space hash-grid radiance cache.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", int, capacityLog2, 18, "FUSE: radiance cache slots = 2^capacityLog2.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", float, cellSize, 0.05f, "FUSE: finest radiance cache cell edge (world).");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", float, cellPixels, 8.f,
                        "FUSE: target radiance cache cell edge in pixels at the cell's distance.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", int, trainStride, 4,
                        "FUSE: one radiance cache training path per trainStride x trainStride pixels.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", float, minSamples, 4.f,
                        "FUSE: samples a radiance cache cell needs before it answers queries.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", float, maxSamples, 256.f,
                        "FUSE: radiance cache temporal window (resolved sample count cap).");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", int, maxAge, 32, "FUSE: frames without samples before a cell is evicted.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", float, minRoughness, 0.3f,
                        "FUSE: only vertices at least this rough end in the radiance cache.");
    FUSE_RELIGHT_OPTION("rtx.radianceCache", float, spreadThreshold, 1.f,
                        "FUSE: path spread (x the cell size) at which a path may end in the radiance cache.");
};

struct RadianceCacheSettings {
    bool enabled = false;
    u32 capacity = 1u << 18;      ///< slots (power of two; rounded up)
    float cellSize = 0.05f;       ///< finest cell edge (world units)
    float cellPixels = 8.f;       ///< target cell edge in pixels at the cell's distance
    u32 maxProbe = 8;             ///< linear probing length
    u32 trainStride = 4;          ///< training tile edge (pixels)
    float minSamples = 4.f;
    float spreadThreshold = 1.f;
    float maxSamples = 256.f;
    float maxRadiance = 32.f;     ///< training radiance clamp (per channel)
    u32 maxAge = 32;
    u32 minBounce = 1;            ///< first path vertex index that may end in the cache
    float minRoughness = 0.3f;
    u32 maxVertices = kRcMaxVertices;
    bool stats = true;            ///< count queries / hits in the table header

    static RadianceCacheSettings fromOptions();
};

/// PtSettings with kPtFlagRadianceCache (the path tracer's paths may end in the cache).
inline PtSettings withRadianceCache(PtSettings s) {
    s.flags |= kPtFlagRadianceCache;
    return s;
}

/// Packs RcParams (radiance_cache_core.h rcParamsUnpack) for the frame whose path-tracer params are `ptParams`
/// (kPtParamWords: the camera position and field of view come from there). `frame` picks the training pixels.
void packRadianceCacheParams(const RadianceCacheSettings& settings, const Word* ptParams, u32 frame, Word* out);
/// The path-tracer params of the training stage: `ptParams` with kPtFlagRcTrain on and the query / ReSTIR flags off.
void radianceCacheTrainParams(const Word* ptParams, Word* out);
/// Training tiles (= training paths) of a width x height frame.
u32 radianceCacheTiles(const RadianceCacheSettings& settings, u32 width, u32 height);
/// Table size in u32 words (header included).
u64 radianceCacheTableWords(const RadianceCacheSettings& settings);

struct RadianceCacheStats {
    u32 frames = 0;
    u32 records = 0;  ///< valid training records this frame
    u32 queries = 0;  ///< header counters (kRcCounter*): this frame's
    u32 hits = 0;
    u32 inserts = 0;
    u32 dropped = 0;
    u32 live = 0;
    u32 evicted = 0;
    u32 fresh = 0;
    double hitRate() const { return queries != 0u ? double(hits) / double(queries) : 0.0; }
};
/// The header counters of a table (CPU vector or GPU read-back).
RadianceCacheStats radianceCacheCounters(const u32* table);

/// Hash key of a point (the cell id the gates compare): checksum, probe start slot and level.
struct RadianceCacheCell {
    u32 check = 0;
    u32 slot = 0;
    u32 level = 0;
    u32 bin = 0;
};
RadianceCacheCell radianceCacheCell(const Word* rcParams, const float p[3], const float n[3]);
/// Resolved radiance of the cell of (p, n) in `table` (false: absent / too few samples).
bool radianceCacheLookup(const Word* rcParams, const u32* table, const float p[3], const float n[3], float out[3],
                         float* samples = nullptr);
/// Per-key content of a table: (checksum -> slot words 1..11), for order-independent comparisons.
struct RadianceCacheEntry {
    u32 check = 0;
    u32 words[kRcSlotWords - 1u] = {};
};
void radianceCacheEntries(const u32* table, u32 capacity, std::vector<RadianceCacheEntry>& out);

/// An alternative answer at query time (the neural research track, research/relight_nrc): the path tracer's
/// termination decision is the hash grid's (spread, eligibility), the radiance comes from `fn` (false: no answer,
/// the path continues). Called concurrently (CpuParallel).
struct RadianceCacheLookup {
    bool (*fn)(void* user, const float p[3], const float n[3], float out[3]) = nullptr;
    void* user = nullptr;
};

class RadianceCacheCpu {
public:
    RadianceCacheCpu();
    ~RadianceCacheCpu();
    RadianceCacheCpu(const RadianceCacheCpu&) = delete;
    RadianceCacheCpu& operator=(const RadianceCacheCpu&) = delete;

    /// Applies the settings; a capacity change reallocates and clears the table.
    bool configure(const RadianceCacheSettings& settings);
    void clear();
    const RadianceCacheSettings& settings() const { return m_settings; }

    /// train + update + resolve for the frame (`settings`: the render settings of the path tracer).
    bool frame(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
               u32 sampleBase, kernel::Backend backend = kernel::Backend::CpuParallel);
    /// The stages one by one. train: this frame's params, header (params + zeroed counters) and records.
    bool train(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
               u32 sampleBase, kernel::Backend backend = kernel::Backend::CpuParallel);
    bool update(kernel::Backend backend = kernel::Backend::CpuParallel);
    bool resolve(kernel::Backend backend = kernel::Backend::CpuParallel);
    /// Replays: update / resolve on explicit tables and records (the GPU parity gate: the GPU's read-back inputs).
    bool replayUpdate(const Word* rcParams, const Word* records, u32 recordCount, std::vector<u32>& table,
                      kernel::Backend backend = kernel::Backend::CpuReference) const;
    bool replayResolve(const Word* rcParams, std::vector<u32>& table,
                       kernel::Backend backend = kernel::Backend::CpuReference) const;

    /// renderReference's hook for a width x height render (queries this cache; the render settings need
    /// kPtFlagRadianceCache). Valid until the next configure / hook call.
    const ptk::PtRcHook* hook(u32 width, u32 height);
    /// Replaces the hash-grid answer (null fn: the hash grid).
    void setLookup(const RadianceCacheLookup& lookup) { m_lookup = lookup; }

    /// Gates: adopts another cache's state (the GPU's read-back params and table) for the hook and lookups.
    bool adopt(const Word* rcParams, const u32* table, u64 words);

    const std::vector<u32>& table() const { return m_table; }
    std::vector<u32>& table() { return m_table; }
    const std::vector<Word>& records() const { return m_records; }
    const Word* params() const { return m_params; }
    u32 frameIndex() const { return m_frame; }
    RadianceCacheStats stats() const;

    struct Impl;

private:
    Impl* m_impl = nullptr;
    RadianceCacheSettings m_settings{};
    RadianceCacheLookup m_lookup{};
    std::vector<u32> m_table;
    std::vector<Word> m_records;
    std::vector<Word> m_paths;
    Word m_params[kRcParamWords] = {};
    u32 m_frame = 0;
    u32 m_frames = 0;
    u32 m_width = 0;
    u32 m_height = 0;
};

} // namespace fuse::relight::render::pathtrace
