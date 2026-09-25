// FUSE Relight RL-5.2: ReSTIR DI for the path tracer (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.3; the algorithm, the
// integrand and the packed records are documented in render/pathtrace/shaders/restir_di_core.h).
//
// The path tracer's direct lighting at its G-buffer vertex (the primary surface after PSR) of each frame's first
// sample comes from spatiotemporally resampled reservoirs when kPtFlagRestirDi is set in PtSettings::flags:
//
//   presample -> surface -> initial -> temporal -> spatial x iterations -> shade        (RestirDiCpu / RestirDiGpu)
//   path tracer (kPtFlagRestirDi): NEE at the matching vertex replaced by the shaded estimate
//
// CPU (the oracle; kernel::Backend CpuReference == CpuParallel bit for bit, one pixel per item):
//   RestirDiCpu di;
//   di.frame(scene, settings, w, h, frameSeed, sampleBase, diSettings);      // the stages, history kept
//   renderReference(scene, withRestir(settings), w, h, frameSeed, sampleBase, 1, img, backend, nullptr, di.hook());
// GPU: RestirDiGpu (restir_di_gpu.hpp) records the same stages as render-graph passes before "relight.pt.trace".
//
// Options (RL-0.6 registry; upstream names from Remix's rtx.di.* family where one exists, FUSE additions marked):
//   rtx.useRTXDI                         ReSTIR DI for the path tracer's direct lighting (default off in FUSE until
//                                        the frame renderer opts in; RestirDiSettings::fromOptions reads it)
//   rtx.di.initialSampleCount            light-tile candidates per pixel (RIS)
//   rtx.di.lightTreeSampleCount          (FUSE) WP-7.1 light-tree candidates per pixel, MIS-combined with the tiles
//   rtx.di.bsdfSampleCount               (FUSE) BSDF-sampled candidates per pixel (one ray each), MIS-combined
//   rtx.di.spatialSamples                spatial neighbours per iteration
//   rtx.di.spatialIterations             (FUSE) spatial passes
//   rtx.di.spatialRadius                 (FUSE) neighbour disk radius in pixels
//   rtx.di.maxHistoryLength              temporal confidence cap (x the canonical M)
//   rtx.di.enableTemporalReuse           (FUSE) temporal reuse
//   rtx.di.enableInitialVisibility       visibility reuse of the initial candidate
//   rtx.di.enableRayTracedBiasCorrection unbiased mode (visibility-tested pairwise MIS); off: fast biased reuse
//   rtx.di.lightTileCount / lightTileSize (FUSE) presampled tiles
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::ptk {
struct PtDiHook;
}

namespace fuse::relight::render::pathtrace {

// Mirrors of pt_reference_types.h / restir_di_core.h.
inline constexpr u32 kPtFlagRestirDi = 64u;
inline constexpr u32 kPtFlagDiRecord = 128u;
inline constexpr u32 kRdiParamWords = 4u;
inline constexpr u32 kRdiSurfaceWords = 4u;
inline constexpr u32 kRdiReservoirWords = 2u;
inline constexpr u32 kRdiOutWords = 2u;
inline constexpr u32 kRdiMaxNeighbors = 8u;
enum RdiFlag : u32 {
    kRdiFlagUnbiased = 1u,
    kRdiFlagHistory = 2u,
    kRdiFlagInitialVisibility = 4u,
    kRdiFlagTemporal = 8u,
};

struct RestirDiOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, useRTXDI, false,
                        "ReSTIR DI (spatiotemporal reservoir resampling) for the path tracer's direct lighting.");
    FUSE_RELIGHT_OPTION("rtx.di", int, initialSampleCount, 8, "Light-tile candidates per pixel (initial RIS).");
    FUSE_RELIGHT_OPTION("rtx.di", int, lightTreeSampleCount, 1,
                        "FUSE: light-tree candidates per pixel, MIS-combined with the light-tile candidates.");
    FUSE_RELIGHT_OPTION("rtx.di", int, bsdfSampleCount, 1,
                        "FUSE: BSDF-sampled candidates per pixel (the emitters the ray reaches first), MIS-combined.");
    FUSE_RELIGHT_OPTION("rtx.di", int, spatialSamples, 4, "Spatial neighbours per spatial reuse pass.");
    FUSE_RELIGHT_OPTION("rtx.di", int, spatialIterations, 1, "FUSE: spatial reuse passes (0: none).");
    FUSE_RELIGHT_OPTION("rtx.di", float, spatialRadius, 16.f, "FUSE: spatial neighbour radius in pixels.");
    FUSE_RELIGHT_OPTION("rtx.di", int, maxHistoryLength, 20,
                        "Temporal confidence cap: the history's M is clamped to this multiple of the new sample's.");
    FUSE_RELIGHT_OPTION("rtx.di", bool, enableTemporalReuse, true, "FUSE: temporal reservoir reuse.");
    FUSE_RELIGHT_OPTION("rtx.di", bool, enableInitialVisibility, true,
                        "Trace the chosen initial candidate's shadow ray (visibility reuse).");
    FUSE_RELIGHT_OPTION("rtx.di", bool, enableRayTracedBiasCorrection, true,
                        "Unbiased reuse: visibility-tested pairwise MIS weights. Off: faster biased reuse.");
    FUSE_RELIGHT_OPTION("rtx.di", int, lightTileCount, 64, "FUSE: presampled light tiles per frame.");
    FUSE_RELIGHT_OPTION("rtx.di", int, lightTileSize, 256, "FUSE: lights per presampled tile.");
};

struct RestirDiSettings {
    bool enabled = false;
    bool unbiased = true;
    bool temporal = true;
    bool initialVisibility = true;
    u32 tileCandidates = 8;
    u32 treeCandidates = 1;
    u32 bsdfCandidates = 1;
    u32 spatialSamples = 4;
    u32 spatialIterations = 1;
    float spatialRadius = 16.f;
    float maxHistory = 20.f;
    u32 tileCount = 64;
    u32 tileSize = 256;
    float normalThreshold = 0.9f;
    float depthThreshold = 0.1f;
    float distantArea = 16.f; ///< tile weight of a distant light = luminance(irradiance) x this

    static RestirDiSettings fromOptions();
};

/// PtSettings with kPtFlagRestirDi set (the path tracer takes the DI estimate).
inline PtSettings withRestirDi(PtSettings s) {
    s.flags |= kPtFlagRestirDi;
    return s;
}

/// The tiles' power distribution over the compiled scene's light set (host; allocation-free once sized).
class RestirDiLightTable {
public:
    /// Rebuilds the CDF (float, last entry 1) and the exact per-entry pmf. False: no light with power (no tiles).
    bool build(const PtCompiledScene& scene, float distantArea);
    u32 lightCount() const { return static_cast<u32>(m_pmf.size()); }
    bool usable() const { return m_usable; }
    const std::vector<float>& pmf() const { return m_pmf; }
    const std::vector<float>& cdf() const { return m_cdf; }

private:
    std::vector<double> m_weights;
    std::vector<float> m_pmf;
    std::vector<float> m_cdf;
    bool m_usable = false;
};

/// The packed stage parameters (restir_di_core.h rdiParamsUnpack) for `iteration`.
void packRestirDiParams(const RestirDiSettings& settings, u32 flags, u32 tileCount, u32 iteration, Word* out);

struct RestirDiStats {
    u32 frames = 0;
    u32 surfaces = 0;  ///< pixels with a ReSTIR vertex this frame
    u32 samples = 0;   ///< final reservoirs holding a sample
    bool history = false;
};

/// The CPU runner (see the header comment). Keeps the previous frame's surfaces and final reservoirs.
class RestirDiCpu {
public:
    RestirDiCpu();
    ~RestirDiCpu();
    RestirDiCpu(const RestirDiCpu&) = delete;
    RestirDiCpu& operator=(const RestirDiCpu&) = delete;

    /// Runs the stages for the frame (the path tracer's params: settings, size, frameSeed, sampleBase). False on
    /// invalid input. A size change drops the history.
    bool frame(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
               u32 sampleBase, const RestirDiSettings& di, kernel::Backend backend = kernel::Backend::CpuParallel);
    void resetHistory() { m_historyValid = false; }
    /// The apply-mode hook for renderReference (reads this frame's output); valid until the next frame().
    const ptk::PtDiHook* hook() const;
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    /// This frame's buffers (Word = float4): surface records, final reservoirs (the history), output, tiles.
    const std::vector<Word>& surfaces() const { return m_surfaces[m_cur]; }
    const std::vector<Word>& previousSurfaces() const { return m_surfaces[m_cur ^ 1u]; }
    const std::vector<Word>& reservoirs() const { return m_history; }
    const std::vector<Word>& initialReservoirs() const { return m_resInitial; }
    const std::vector<Word>& output() const { return m_output; }
    const std::vector<u32>& tiles() const { return m_tiles; }
    const RestirDiLightTable& lightTable() const { return m_table; }
    const RestirDiStats& stats() const { return m_stats; }

    /// One stage on explicit inputs (the GPU parity gate replays a GPU pass on its read-back inputs):
    /// stage kRdiStageInitial / Temporal / Spatial reads `surfaces` (this frame), `previous` (temporal), `source`,
    /// `history` and writes `out` (kRdiReservoirWords per pixel).
    enum Stage : u32 { kStageInitial = 2u, kStageTemporal = 3u, kStageSpatial = 8u };
    bool replayStage(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
                     u32 sampleBase, const RestirDiSettings& di, u32 flags, u32 stage, u32 iteration,
                     const std::vector<Word>& surfaces, const std::vector<Word>& previous,
                     const std::vector<Word>& source, const std::vector<Word>& history, const std::vector<u32>& tiles,
                     std::vector<Word>& out, kernel::Backend backend = kernel::Backend::CpuReference);

    struct Impl;

private:
    Impl* m_impl = nullptr;
    RestirDiLightTable m_table;
    std::vector<Word> m_surfaces[2];
    std::vector<Word> m_resInitial;
    std::vector<Word> m_resA;
    std::vector<Word> m_resB;
    std::vector<Word> m_history;
    std::vector<Word> m_output;
    std::vector<u32> m_tiles;
    u32 m_cur = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_historyValid = false;
    RestirDiStats m_stats{};
};

} // namespace fuse::relight::render::pathtrace
