// FUSE Relight RL-5.4: the records and constants of the world-space hash-grid radiance cache (radiance_cache_core.h
// has the file comment: hashing, the table layout, update / resolve / query; radiance_cache_path.h the path-tracer
// side). Included by every dialect after its RC_* macros.

// ---- constants --------------------------------------------------------------------------------------------------

RC_CONST uint kRcParamWords = 5u;       ///< float4 words of RcParams (rcParamsUnpack)
RC_CONST uint kRcSlotWords = 12u;       ///< u32 words per table slot (see TABLE in radiance_cache_core.h)
RC_CONST uint kRcHeaderWords = 64u;     ///< u32 words before slot 0: params (kRcParamWords float4) + counters
RC_CONST uint kRcRecordWords = 3u;      ///< float4 words per training record
RC_CONST uint kRcMaxVertices = 8u;      ///< training records per training path
RC_CONST uint kRcMaxLevel = 24u;        ///< coarsest cell level (cell = baseCell x 2^level)
RC_CONST uint kRcMaxFrameSamples = 8192u; ///< samples a cell accumulates per frame (the rest only counted)
RC_CONST float kRcFixedScale = 16384.0f;  ///< fixed-point scale of the per-frame radiance sums
RC_CONST uint kRcInvalid = 4294967295u;

// Header counters (u32 words; zeroed by the train stage each frame, then atomically incremented).
RC_CONST uint kRcCounterQueries = 32u;  ///< path vertices that asked the cache (eligible and past the spread)
RC_CONST uint kRcCounterHits = 33u;     ///< ... and ended with the cached radiance
RC_CONST uint kRcCounterInserts = 34u;  ///< training records accumulated into a cell
RC_CONST uint kRcCounterDropped = 35u;  ///< training records dropped (probe sequence full)
RC_CONST uint kRcCounterLive = 36u;     ///< cells alive after the resolve
RC_CONST uint kRcCounterEvicted = 37u;  ///< cells evicted by the resolve (not updated for maxAge frames)
RC_CONST uint kRcCounterFresh = 38u;    ///< cells updated this frame

// RcParams.flags
RC_CONST uint kRcFlagStats = 1u;        ///< count queries / hits (atomics on the header)

// Hook vertex flags (ptRadianceCacheVertex `vflags`, set by the path-tracing core)
RC_CONST uint kRcVertexChain = 1u;      ///< on the primary chain (may become the G-buffer vertex: never terminated)
RC_CONST uint kRcVertexPrevDelta = 2u;  ///< the previous scatter was a dirac lobe (or the camera / a portal)
RC_CONST uint kRcVertexReplaced = 4u;   ///< ReSTIR DI / GI own this vertex's light (neither recorded nor terminated)

// ---- records ----------------------------------------------------------------------------------------------------

struct RcParams {
    float3 camera;          ///< the frame's camera position (cell size by distance)
    float pixelAngle;       ///< 2 tan(fovY / 2) / height: one pixel's footprint per unit distance
    float cellSize;         ///< finest cell edge (world units)
    float invCellSize;      ///< 1 / cellSize (computed on the host: identical on every dialect)
    float cellPixels;       ///< target cell edge in pixels at the cell's distance (adaptive level)
    uint capacity;          ///< slots (power of two)
    uint maxProbe;          ///< linear probing length
    uint trainStride;       ///< one training path per trainStride x trainStride pixel tile
    uint frame;             ///< frame index (training pixel choice), < 2^24
    uint flags;             ///< kRcFlag*
    float minSamples;       ///< a cell answers queries once its sample count reaches this
    float spreadThreshold;  ///< a path ends at a vertex once its spread reaches this x the vertex's cell size
    float maxSamples;       ///< temporal window of the resolve (sample count cap)
    float maxRadiance;      ///< training radiance clamp (per channel)
    uint maxAge;            ///< frames without samples before a cell is evicted
    uint minBounce;         ///< first path vertex index that may end in the cache
    float minRoughness;     ///< only opaque vertices at least this rough end in the cache
    uint maxVertices;       ///< training records per path (<= kRcMaxVertices)
};

/// Hash key of a cell: probe start slot, 32-bit checksum (never 0: 0 marks an empty slot), level, normal bin.
struct RcKey {
    uint slot;
    uint check;
    uint level;
    uint bin;
};

/// Per-path state of the path-tracer hook (radiance_cache_path.h); the recorded vertices live in the includer's
/// storage (rcPathLoad / rcPathStore).
struct RcPathState {
    uint count;   ///< recorded training vertices
    uint seen;    ///< vertices seen (0: the next one is reached from the camera)
    float spread; ///< path spread (square root of the footprint area, world units)
    float pad;
};
