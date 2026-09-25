// FUSE Relight RL-5.3: ReSTIR GI for the path tracer (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.4; the algorithm, the
// split with the path tracer and the packed records are documented in render/pathtrace/shaders/restir_gi_core.h).
//
// The indirect light at the path tracer's G-buffer vertex of each frame's first sample comes from spatiotemporally
// resampled secondary-vertex reservoirs when kPtFlagRestirGi is set in PtSettings::flags:
//
//   surface -> initial -> temporal -> spatial x iterations -> shade                    (RestirGiCpu / RestirGiGpu)
//   path tracer (kPtFlagRestirGi): at the matching vertex NEE as usual, the continuation collects its first
//   segment's light-set emission and stops; the shaded GI estimate is added.
// It composes with RL-5.2's ReSTIR DI (both flags: DI replaces NEE, GI the rest of the continuation).
//
// CPU (the oracle; kernel::Backend CpuReference == CpuParallel bit for bit, one pixel per item):
//   RestirGiCpu gi;
//   gi.frame(scene, settings, w, h, frameSeed, sampleBase, giSettings);
//   renderReference(scene, withRestirGi(settings), w, h, frameSeed, sampleBase, 1, img, backend, nullptr, nullptr,
//                   gi.hook());
// GPU: RestirGiGpu (restir_gi_gpu.hpp) records the same stages as render-graph passes before "relight.pt.trace".
//
// Options (RL-0.6 registry; upstream Remix rtx.restirGI.* names where the meaning matches, FUSE additions marked):
//   rtx.useReSTIRGI                       ReSTIR GI for the path tracer's indirect light (default off until the frame
//                                         renderer opts in; RestirGiSettings::fromOptions reads it)
//   rtx.restirGI.useTemporalReuse         temporal reuse
//   rtx.restirGI.useSpatialReuse          spatial reuse
//   rtx.restirGI.biasCorrectionMode       4 (pairwise, ray traced): unbiased (visibility-tested pairwise MIS);
//                                         0..3: FUSE's fast mode (biased M-weighted reuse without visibility)
//   rtx.restirGI.temporalHistoryLength    (FUSE semantics) history confidence cap (x the canonical M)
//   rtx.restirGI.spatialSamples           (FUSE) spatial neighbours per pass
//   rtx.restirGI.spatialIterations        (FUSE) spatial passes
//   rtx.restirGI.spatialRadius            (FUSE) neighbour disk radius in pixels
// ReSTIR PT (Lin et al. 2022, hybrid shift; the plan's "Ultra" preset) is not implemented: see the RL-5.3 row.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::relight::ptk {
struct PtGiHook;
}

namespace fuse::relight::render::pathtrace {

// Mirrors of pt_reference_types.h / restir_gi_core.h.
inline constexpr u32 kPtFlagRestirGi = 256u;
inline constexpr u32 kPtFlagGiRecord = 512u;
inline constexpr u32 kRgiParamWords = 2u;
inline constexpr u32 kRgiSurfaceWords = 4u;
inline constexpr u32 kRgiReservoirWords = 9u;
inline constexpr u32 kRgiOutWords = 2u;
inline constexpr u32 kRgiMaxNeighbors = 8u;
enum RgiFlag : u32 {
    kRgiFlagUnbiased = 1u,
    kRgiFlagHistory = 2u,
    kRgiFlagTemporal = 4u,
};

struct RestirGiOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, useReSTIRGI, false,
                        "ReSTIR GI (secondary-vertex reservoir resampling) for the path tracer's indirect light.");
    FUSE_RELIGHT_OPTION("rtx.restirGI", bool, useTemporalReuse, true, "Temporal reuse of the GI reservoirs.");
    FUSE_RELIGHT_OPTION("rtx.restirGI", bool, useSpatialReuse, true, "Spatial reuse of the GI reservoirs.");
    FUSE_RELIGHT_OPTION("rtx.restirGI", int, biasCorrectionMode, 4,
                        "4: unbiased reuse (visibility-tested pairwise MIS). 0-3: FUSE fast mode (biased reuse).");
    FUSE_RELIGHT_OPTION("rtx.restirGI", int, temporalHistoryLength, 20,
                        "Temporal confidence cap: the history's M is clamped to this multiple of the new sample's.");
    FUSE_RELIGHT_OPTION("rtx.restirGI", int, spatialSamples, 4, "FUSE: spatial neighbours per spatial reuse pass.");
    FUSE_RELIGHT_OPTION("rtx.restirGI", int, spatialIterations, 1, "FUSE: spatial reuse passes.");
    FUSE_RELIGHT_OPTION("rtx.restirGI", float, spatialRadius, 16.f, "FUSE: spatial neighbour radius in pixels.");
};

struct RestirGiSettings {
    bool enabled = false;
    bool unbiased = true;
    bool temporal = true;
    u32 spatialSamples = 4;
    u32 spatialIterations = 1; ///< 0: no spatial reuse
    float spatialRadius = 16.f;
    float maxHistory = 20.f;
    float normalThreshold = 0.9f;
    float depthThreshold = 0.1f;

    static RestirGiSettings fromOptions();
};

/// PtSettings with kPtFlagRestirGi set (the path tracer takes the GI estimate).
inline PtSettings withRestirGi(PtSettings s) {
    s.flags |= kPtFlagRestirGi;
    return s;
}

/// The packed stage parameters (restir_gi_core.h rgiParamsUnpack) for `iteration`.
void packRestirGiParams(const RestirGiSettings& settings, u32 flags, u32 iteration, Word* out);

struct RestirGiStats {
    u32 frames = 0;
    u32 surfaces = 0; ///< pixels with a GI vertex this frame
    u32 samples = 0;  ///< final reservoirs holding a sample
    bool history = false;
};

/// The CPU runner (see the header comment). Keeps the previous frame's surfaces and final reservoirs.
class RestirGiCpu {
public:
    RestirGiCpu();
    ~RestirGiCpu();
    RestirGiCpu(const RestirGiCpu&) = delete;
    RestirGiCpu& operator=(const RestirGiCpu&) = delete;

    /// Runs the stages for the frame (the path tracer's params: settings - with kPtFlagRestirDi when DI replaces NEE
    /// in the same frame -, size, frameSeed, sampleBase). False on invalid input. A size change drops the history.
    bool frame(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
               u32 sampleBase, const RestirGiSettings& gi, kernel::Backend backend = kernel::Backend::CpuParallel);
    void resetHistory() { m_historyValid = false; }
    /// The apply-mode hook for renderReference (reads this frame's output); valid until the next frame().
    const ptk::PtGiHook* hook() const;
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    /// This frame's buffers (Word = float4): surface records, initial / final reservoirs (the history), output.
    const std::vector<Word>& surfaces() const { return m_surfaces[m_cur]; }
    const std::vector<Word>& previousSurfaces() const { return m_surfaces[m_cur ^ 1u]; }
    const std::vector<Word>& reservoirs() const { return m_history; }
    const std::vector<Word>& initialReservoirs() const { return m_resInitial; }
    const std::vector<Word>& output() const { return m_output; }
    const RestirGiStats& stats() const { return m_stats; }
    /// The frame flags (kRgiFlag*) of the last frame().
    u32 frameFlags() const { return m_flags; }

    /// One stage on explicit inputs (the GPU parity gate replays a GPU pass on its read-back inputs): kStageInitial
    /// reads `surfaces`; kStageTemporal / kStageSpatial read `surfaces`, `previous`, `source` and `history`;
    /// kStageShade reads `surfaces` and `source` and writes the output (kRgiOutWords per pixel). `out` otherwise
    /// gets kRgiReservoirWords per pixel.
    enum Stage : u32 { kStageSurface = 1u, kStageInitial = 2u, kStageTemporal = 3u, kStageShade = 4u, kStageSpatial = 8u };
    bool replayStage(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
                     u32 sampleBase, const RestirGiSettings& gi, u32 flags, u32 stage, u32 iteration,
                     const std::vector<Word>& surfaces, const std::vector<Word>& previous,
                     const std::vector<Word>& source, const std::vector<Word>& history, std::vector<Word>& out,
                     kernel::Backend backend = kernel::Backend::CpuReference);

    struct Impl;

private:
    Impl* m_impl = nullptr;
    std::vector<Word> m_surfaces[2];
    std::vector<Word> m_resInitial;
    std::vector<Word> m_resA;
    std::vector<Word> m_resB;
    std::vector<Word> m_history;
    std::vector<Word> m_output;
    u32 m_cur = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_flags = 0;
    bool m_historyValid = false;
    RestirGiStats m_stats{};
};

} // namespace fuse::relight::render::pathtrace
