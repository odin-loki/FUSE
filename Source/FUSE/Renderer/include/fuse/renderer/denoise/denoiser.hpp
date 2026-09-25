#pragma once

// Denoiser abstraction (WP-6.4b): a small interface every denoiser backend implements, plus a registry with
// capability-based selection and a documented fallback to the in-tree SVGF / A-SVGF (WP-6.4).
//
//   "svgf"  in-tree  SvgfDenoiserAdapter over SvgfDenoiser (Svgf, ASvgf; Shadow / Reflection / Gi), always registered
//                    by register_builtin_denoisers()
//   "nrd"   plugin   nrd::NrdDenoiser (plugins/nvidia_nrd, WP-6.4b): NVIDIA NRD SIGMA / REBLUR / RELAX through a
//                    runtime-loaded provider; registered only when a provider loads and reports the methods
//
// The frame protocol is the WP-6.4 one (svgf_denoiser.hpp): configure() -> beginFrame(serial, DenoiseFrameDesc) ->
// importInto(graph) -> addPasses(graph, refs, DenoiseGraphInputs) -> collectRetired(done). `DenoiseGraphInputs`
// stays the producer-facing contract; a backend that needs more (NRD: native NRD-packed textures, camera matrices)
// takes it through its own bind call before beginFrame and reports it in DenoiserCaps::needs_native_frame.
//
// This header is additive: SvgfDenoiser is unchanged, the adapter wraps it.

#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fuse::renderer::denoise {

/// Denoising algorithm. Svgf / ASvgf are in-tree; Sigma / Reblur / Relax are NVIDIA NRD (plugin).
enum class DenoiserMethod : u8 {
    Svgf = 0,   ///< SVGF [Schied et al. 2017]
    ASvgf = 1,  ///< SVGF + A-SVGF temporal gradients [Schied et al. 2018] (needs producer gradient samples)
    Sigma = 2,  ///< NRD SIGMA (shadows)
    Reblur = 3, ///< NRD REBLUR (diffuse / specular radiance + hit distance)
    Relax = 4,  ///< NRD RELAX (diffuse / specular radiance, ReSTIR-friendly)
};
inline constexpr u32 kDenoiserMethodCount = 5u;

constexpr u8 denoiser_method_bit(DenoiserMethod m) { return static_cast<u8>(1u << static_cast<u32>(m)); }
constexpr u8 denoise_signal_bit(DenoiseSignal s) { return static_cast<u8>(1u << static_cast<u32>(s)); }
inline constexpr u8 kAllDenoiseSignals = 0x7u;

const char* denoiser_method_name(DenoiserMethod m);
/// True when `method` can denoise `signal` at all (SIGMA: shadows only; REBLUR / RELAX: reflection and GI).
bool denoiser_method_accepts(DenoiserMethod method, DenoiseSignal signal);

struct DenoiserCaps {
    const char* name = "";         ///< registry id ("svgf", "nrd")
    const char* display_name = "";
    const char* license = "";      ///< SPDX id of what the backend runs (adapter code is MIT either way)
    u8 methods = 0;                ///< denoiser_method_bit set
    u8 signals = 0;                ///< denoise_signal_bit set
    bool needs_native_frame = false; ///< needs a backend-specific bind (native textures) before beginFrame
    u8 hit_distance_methods = 0;     ///< methods whose signal must carry the hit distance (NRD radiance + hitDist)
    bool gpu_only = true;
    bool in_tree = false;

    bool supports(DenoiserMethod m) const { return (methods & denoiser_method_bit(m)) != 0u; }
    bool supports(DenoiseSignal s) const { return (signals & denoise_signal_bit(s)) != 0u; }
    bool needs_hit_distance(DenoiserMethod m) const { return (hit_distance_methods & denoiser_method_bit(m)) != 0u; }
};

/// Backend-neutral tunables. Each backend maps what it understands and keeps its defaults for the rest.
struct DenoiserSettings {
    DenoiseSignal signal = DenoiseSignal::Gi;
    DenoiserMethod method = DenoiserMethod::Svgf;
    u32 max_history_frames = 32u;   ///< SVGF maxHistory / NRD maxAccumulatedFrameNum
    f32 disocclusion_depth = 0.05f; ///< relative depth tolerance of a history tap (SVGF reprojDepth, NRD disocclusionThreshold)
    bool anti_firefly = false;      ///< NRD enableAntiFirefly (ignored by SVGF)
};

class IDenoiser {
public:
    virtual ~IDenoiser() = default;
    virtual const DenoiserCaps& caps() const = 0;
    /// False (nothing changed) when the method / signal pair is not supported by this backend.
    virtual bool configure(const DenoiserSettings& settings) = 0;
    virtual const DenoiserSettings& settings() const = 0;
    /// The next frame starts without history.
    virtual void reset() = 0;
    virtual bool beginFrame(u64 frameSerial, const DenoiseFrameDesc& frame) = 0;
    virtual DenoiseGraphRefs importInto(rg::Graph& graph) = 0;
    virtual void addPasses(rg::Graph& graph, const DenoiseGraphRefs& refs, const DenoiseGraphInputs& inputs) = 0;
    virtual u32 collectRetired(u64 completedSerial) = 0;
};

/// Creation parameters every factory receives (the in-tree SVGF needs the device; plugins bind their own).
struct DenoiserCreateInfo {
    SvgfDenoiserDesc svgf{};
};

using DenoiserFactory = std::unique_ptr<IDenoiser> (*)(const DenoiserCreateInfo& info);

struct DenoiserBackendEntry {
    DenoiserCaps caps{};
    DenoiserFactory factory = nullptr;
};

struct DenoiserRequirements {
    DenoiseSignal signal = DenoiseSignal::Gi;
    DenoiserMethod method = DenoiserMethod::Svgf; ///< preferred method
    bool allow_fallback = true;                   ///< fall back to the in-tree SVGF when nothing serves `method`
    bool have_native_frame = true;                ///< the renderer can bind native textures (plugins)
    bool have_hit_distance = false;               ///< the producer writes the NRD radiance + hit-distance packing
    bool have_gradients = false;                  ///< the producer writes A-SVGF gradient samples
};

struct DenoiserSelection {
    const DenoiserCaps* caps = nullptr; ///< null: nothing can serve the request
    DenoiserMethod method = DenoiserMethod::Svgf;
    bool fallback = false;              ///< caps / method differ from the preferred ones
    const char* reason = "";
};

class DenoiserRegistry {
public:
    /// Process-wide registry with the built-in "svgf" registered.
    static DenoiserRegistry& instance();
    DenoiserRegistry() = default;
    void register_builtin_denoisers();

    /// False when the name is empty or taken, or the factory is null.
    bool register_backend(const DenoiserCaps& caps, DenoiserFactory factory);
    bool unregister_backend(std::string_view name);
    const DenoiserCaps* find(std::string_view name) const;
    std::span<const DenoiserBackendEntry> backends() const { return m_entries; }
    /// Null when unknown or when the factory fails (e.g. SVGF without a capable device).
    std::unique_ptr<IDenoiser> create(std::string_view name, const DenoiserCreateInfo& info) const;

    /// The first backend (registration order) serving `req.method` for `req.signal` with the inputs the caller
    /// has; otherwise, when allowed, the first in-tree backend with the in-tree fallback method (ASvgf when the
    /// preferred method is ASvgf and gradients exist, else Svgf).
    DenoiserSelection select(const DenoiserRequirements& req) const;

private:
    std::vector<DenoiserBackendEntry> m_entries;
};

inline constexpr const char* kSvgfDenoiserName = "svgf";

/// IDenoiser over the in-tree SvgfDenoiser (owned). init() is the SvgfDenoiser one.
class SvgfDenoiserAdapter final : public IDenoiser {
public:
    SvgfDenoiserAdapter();
    bool init(const SvgfDenoiserDesc& desc) { return m_svgf.init(desc); }
    SvgfDenoiser& svgf() { return m_svgf; }

    const DenoiserCaps& caps() const override;
    bool configure(const DenoiserSettings& settings) override;
    const DenoiserSettings& settings() const override { return m_settings; }
    void reset() override { m_svgf.reset(); }
    bool beginFrame(u64 frameSerial, const DenoiseFrameDesc& frame) override { return m_svgf.beginFrame(frameSerial, frame); }
    DenoiseGraphRefs importInto(rg::Graph& graph) override { return m_svgf.importInto(graph); }
    void addPasses(rg::Graph& graph, const DenoiseGraphRefs& refs, const DenoiseGraphInputs& inputs) override {
        m_svgf.addPasses(graph, refs, inputs);
    }
    u32 collectRetired(u64 completedSerial) override { return m_svgf.collectRetired(completedSerial); }

    /// The SvgfSettings configure() derives (svgf_preset(signal) + method / history / disocclusion).
    static SvgfSettings to_svgf_settings(const DenoiserSettings& settings);

private:
    SvgfDenoiser m_svgf;
    DenoiserSettings m_settings{};
};

const DenoiserCaps& svgf_denoiser_caps();

} // namespace fuse::renderer::denoise
