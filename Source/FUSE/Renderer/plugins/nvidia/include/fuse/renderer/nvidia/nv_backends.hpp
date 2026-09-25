#pragma once

// Registration of the NVIDIA features into FUSE's upscaler registry and the NVIDIA extension registry
// (frame generation + the DLSS 5 look node), plus the UpscaleInputs -> provider input mapping
// (docs/nvidia-plugin.md "Backends").
//
//   "dlss_sr"     ITemporalUpscaler in upscale::UpscalerRegistry            (RTX 20+)
//   "dlss_rr"     ITemporalUpscaler in upscale::UpscalerRegistry            (RTX 20+, needs RR guides)
//   "dlss_fg"     NvFrameGenerator, NvExtensionRegistry kind FrameGeneration (RTX 40+, Reflex; MFG RTX 50)
//   "dlss5_look"  NvNeuralLookPass, NvExtensionRegistry kind LookNode       (RTX 50 only)
//
// Each name is registered only when the loaded provider reports the feature (NvPlugin::supports), so a
// machine without the runtime / GPU never sees them. The upscaler registry has no frame-generation or
// external-look-node slot yet, so those two live in NvExtensionRegistry, which describes where the Look
// graph should place "dlss5_look" (plan_dlss5_look_placement) until the Look system grows a hook.
//
// NVIDIA backends are GPU-only (UpscalerCaps::apis = Vulkan): the CPU views in UpscaleInputs carry no
// texture handles, so the renderer binds the frame's native resources with bind_gpu_frame() before each
// evaluate(); without them evaluate() reports BackendUnavailable. Output lands in the tagged COLOR_OUT
// resource; the CPU output view is not written.

#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/nvidia/nv_input_mapping.hpp>
#include <fuse/renderer/nvidia/nv_plugin_loader.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fuse::renderer::nvidia {

inline constexpr const char* kDlssSrName = "dlss_sr";
inline constexpr const char* kDlssRrName = "dlss_rr";
inline constexpr const char* kDlssFgName = "dlss_fg";
inline constexpr const char* kDlss5LookName = "dlss5_look";

/// Native GPU resources of one frame, indexed by FuseNvBufferKind (native == 0: not bound).
struct NvGpuFrame {
    std::array<FuseNvResource, FUSE_NV_BUFFER_KIND_COUNT> resources{};
    u64 command_buffer = 0; ///< VkCommandBuffer / ID3D12GraphicsCommandList*
    u32 viewport = 0;
    bool depth_reversed_z = true; ///< the bound DEPTH resource is reversed-Z device depth
    bool hdr = true;              ///< COLOR_IN is linear pre-tonemap HDR

    void set(FuseNvBufferKind kind, const FuseNvResource& r) {
        if (kind < FUSE_NV_BUFFER_KIND_COUNT) {
            resources[kind] = r;
        }
    }
};

/// FUSE quality mode -> provider quality (UltraQuality 1.3x -> DLSS Ultra Quality, NativeAA -> DLAA).
[[nodiscard]] FuseNvQuality to_nv_quality(upscale::QualityMode mode) noexcept;

/// Maps the canonical UpscaleInputs (UV motion current - previous, unjittered column-major cameras, linear
/// exposure) plus the bound GPU resources into provider inputs for `feature`:
///   mvec_scale = (-1, -1)                (UV current-previous -> Streamline previous-current, [-1, 1])
///   camera matrices -> row-major row-vector (FUSE column-major storage is bit-identical), clip_to_prev_clip =
///   prevViewProj * inverse(viewProj), jitter_px as is, exposure -> exposure_scale, reset_history -> RESET.
/// NR (dlss5_look) runs at display resolution: render == output == display.
[[nodiscard]] NvFrameInputs map_upscale_inputs(const UpscaleInputs& in, const NvGpuFrame& gpu, FuseNvFeature feature);

/// Maps a provider status to the upscaler contract's status.
[[nodiscard]] upscale::UpscaleStatus to_upscale_status(FuseNvStatus status) noexcept;

// ---- "dlss_sr" / "dlss_rr" ------------------------------------------------------------------------------

class NvTemporalUpscaler final : public upscale::ITemporalUpscaler {
public:
    NvTemporalUpscaler(FuseNvFeature feature, std::shared_ptr<NvPlugin> plugin);

    const upscale::UpscalerCaps& caps() const override;
    upscale::Extent2D render_size(upscale::Extent2D display, upscale::QualityMode mode) const override;
    upscale::UpscaleStatus evaluate(const upscale::UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                    const upscale::TemporalUpscaleOutputs& outputs) override;
    const upscale::IJitterProvider& jitter_provider() const override { return m_jitter; }
    void invalidate_history(upscale::HistoryResetReason reason) override;
    u32 history_generation() const override { return m_generation; }
    upscale::HistoryResetReason last_reset_reason() const override { return m_lastReason; }
    u32 accumulated_frames() const override { return m_accumulated; }

    /// Native resources for the next evaluate() (consumed by it).
    void bind_gpu_frame(const NvGpuFrame& frame);
    void set_quality(upscale::QualityMode mode) { m_quality = mode; }
    [[nodiscard]] const ValidationReport& last_validation() const { return m_lastValidation; }
    [[nodiscard]] FuseNvStatus last_provider_status() const { return m_lastProvider; }

private:
    FuseNvFeature m_feature;
    std::shared_ptr<NvPlugin> m_plugin;
    upscale::HaltonJitterProvider m_jitter{};
    NvGpuFrame m_gpu{};
    bool m_gpuBound = false;
    bool m_pendingReset = true;
    upscale::QualityMode m_quality = upscale::QualityMode::Quality;
    u32 m_generation = 0;
    u32 m_accumulated = 0;
    u32 m_lastRenderW = 0, m_lastRenderH = 0;
    upscale::HistoryResetReason m_lastReason = upscale::HistoryResetReason::None;
    ValidationReport m_lastValidation{};
    FuseNvStatus m_lastProvider = FUSE_NV_OK;
};

// ---- "dlss_fg" / "dlss5_look" -----------------------------------------------------------------------------

enum class NvExtensionKind : u8 { FrameGeneration, LookNode };

struct NvExtensionCaps {
    const char* name = "";
    const char* display_name = "";
    const char* license = "";
    NvExtensionKind kind = NvExtensionKind::FrameGeneration;
    FuseNvFeature feature = FUSE_NV_FEATURE_COUNT;
    u32 min_rtx_generation = 0;
    u32 max_generated_frames = 0;     ///< FG: 1 on RTX 40, 5 (6x MFG) on RTX 50
    bool requires_reflex = false;
    bool requires_hudless_color = false;
    // LookNode placement (look graph vocabulary; the node itself is not a look::LookEffect).
    look::LookStage stage = look::LookStage::PostUpscaleHdr;
    look::LookDomain input = look::LookDomain::SceneHdr;
    look::LookDomain output = look::LookDomain::SceneHdr;
};

/// Frame-generation and look-node backends contributed by optional plugins (process-wide instance()).
class NvExtensionRegistry {
public:
    static NvExtensionRegistry& instance();
    bool add(const NvExtensionCaps& caps);
    bool remove(std::string_view name);
    [[nodiscard]] const NvExtensionCaps* find(std::string_view name) const;
    [[nodiscard]] std::span<const NvExtensionCaps> entries() const { return m_entries; }

private:
    std::vector<NvExtensionCaps> m_entries;
};

/// Where "dlss5_look" goes in a look graph: immediately after the temporal upscaler, before the first
/// display-resolution HDR node (NVIDIA: "the final rendering stage", one frame in / one frame out on
/// colour + motion vectors), i.e. before the first node whose stage is >= PostUpscaleHdr.
struct NvLookPlacement {
    bool ok = false;
    u32 insert_index = 0; ///< position in LookEffectGraph order (== size() when appended)
    const char* reason = "";
};
[[nodiscard]] NvLookPlacement plan_dlss5_look_placement(const look::LookEffectGraph& graph);

/// "dlss_fg": sets DLSS-G options for the frame (generation happens in Streamline's present hooks).
class NvFrameGenerator {
public:
    explicit NvFrameGenerator(std::shared_ptr<NvPlugin> plugin) : m_plugin(std::move(plugin)) {}
    [[nodiscard]] bool available() const;
    [[nodiscard]] u32 max_generated_frames() const;
    /// `frames_to_generate` 1..max_generated_frames(). Returns the provider status (host validation first).
    FuseNvStatus present(const UpscaleInputs& inputs, const NvGpuFrame& gpu, u32 frames_to_generate);
    [[nodiscard]] const ValidationReport& last_validation() const { return m_lastValidation; }

private:
    std::shared_ptr<NvPlugin> m_plugin;
    ValidationReport m_lastValidation{};
};

/// "dlss5_look": DLSS 5 3D-guided neural rendering as a look-graph post node (RTX 50 only).
class NvNeuralLookPass {
public:
    explicit NvNeuralLookPass(std::shared_ptr<NvPlugin> plugin) : m_plugin(std::move(plugin)) {}
    [[nodiscard]] bool available() const;
    /// structure / tone intensity in [0, 1]. COLOR_IN (and optionally COLOR_OUT, NEURAL_CONTROL_MASK) in `gpu`.
    FuseNvStatus evaluate(const UpscaleInputs& inputs, const NvGpuFrame& gpu, f32 structure_intensity, f32 tone_intensity);
    [[nodiscard]] const ValidationReport& last_validation() const { return m_lastValidation; }

private:
    std::shared_ptr<NvPlugin> m_plugin;
    ValidationReport m_lastValidation{};
};

// ---- registration -----------------------------------------------------------------------------------------

struct NvRegistration {
    u32 feature_mask = 0;         ///< FUSE_NV_FEATURE_BIT of every feature that got registered
    std::vector<const char*> names;
};

/// Registers every backend the plugin supports; nothing when `plugin` is null. Factories bind to the
/// plugin passed here (process-wide) until unregister_nvidia_backends().
NvRegistration register_nvidia_backends(upscale::UpscalerRegistry& upscalers, NvExtensionRegistry& extensions,
                                        std::shared_ptr<NvPlugin> plugin);
void unregister_nvidia_backends(upscale::UpscalerRegistry& upscalers, NvExtensionRegistry& extensions);
/// The plugin the registered factories use (null when none registered).
[[nodiscard]] std::shared_ptr<NvPlugin> registered_nvidia_plugin();

/// Engine start-up convenience: NvPluginLoader::probe(project) then register_nvidia_backends() into the
/// process-wide registries. Returns the load result (status Available only when something registered).
LoadResult probe_and_register_nvidia_backends(const PluginConfig& project);

} // namespace fuse::renderer::nvidia
