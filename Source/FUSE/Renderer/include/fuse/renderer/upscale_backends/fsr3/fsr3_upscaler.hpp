#pragma once

// WP-4.2: the FSR 3.1 backend behind the IUpscaler interface (upscale/upscaler.hpp) — registry id "fsr3".
//
// The render path uses Fsr3Gpu (fsr3_gpu.hpp) directly on the frame's render graph. Fsr3TemporalUpscaler adapts
// it to ITemporalUpscaler's host-array contract (UpscaleInputs in, linear RGB out): each evaluate() uploads the
// frame into device images / buffers, runs the FSR passes on a render graph through the bound rg::Executor, reads
// the RGBA16F output back and waits. That makes FSR 3.1 selectable and switchable at runtime next to native_taau /
// fsr1 / nis / cas through UpscalerRegistry (the Phase 4 exit criterion) and lets the CPU-image quality harnesses
// evaluate it; it is not the zero-copy render path.
//
// Registration needs a Vulkan device, so "fsr3" is not a built-in backend: call register_fsr3_backend() with the
// device / allocator / executor to bind (like register_nvidia_backends). The binding must outlive every instance.

#include <fuse/renderer/upscale/upscaler.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_gpu.hpp>

#include <memory>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
namespace rg {
class Executor;
}
} // namespace fuse::renderer

namespace fuse::renderer::fsr3 {

struct Fsr3BackendBinding {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    rg::Executor* executor = nullptr;
};

/// Caps of "fsr3": temporal, depth + motion + jitter, optional reactive / T&C masks, exposure, HDR input, RCAS
/// sharpening, dynamic resolution, ratio 1..3, every quality mode, Vulkan, MIT.
upscale::UpscalerCaps fsr3_caps();

/// Binds `binding` and registers "fsr3" (false when the device cannot run the passes, "fsr3" is already
/// registered, or in the stub backend). unregister_fsr3_backend() removes it and drops the binding.
bool register_fsr3_backend(upscale::UpscalerRegistry& registry, const Fsr3BackendBinding& binding);
void unregister_fsr3_backend(upscale::UpscalerRegistry& registry);

/// Host-array adapter (see the header comment). Sharpening follows `sharpness()` (0 = RCAS off).
class Fsr3TemporalUpscaler final : public upscale::ITemporalUpscaler {
public:
    explicit Fsr3TemporalUpscaler(const Fsr3BackendBinding& binding);
    ~Fsr3TemporalUpscaler() override;

    const upscale::UpscalerCaps& caps() const override { return m_caps; }
    upscale::UpscaleStatus evaluate(const upscale::UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                    const upscale::TemporalUpscaleOutputs& outputs) override;
    const upscale::IJitterProvider& jitter_provider() const override { return m_jitter; }
    void invalidate_history(upscale::HistoryResetReason reason) override;
    u32 history_generation() const override { return m_generation; }
    upscale::HistoryResetReason last_reset_reason() const override { return m_lastReason; }
    u32 accumulated_frames() const override { return m_accumulated; }

    f32& sharpness() { return m_sharpness; }
    Fsr3Gpu& gpu() { return m_gpu; }
    /// Test hook: feed FSR the wrong jitter sign (jitter-sign gate control).
    bool& debugFlipJitterSign() { return m_flipJitter; }
    /// Language of the convert kernel; takes effect when the device side is created (first evaluate()).
    Fsr3KernelLanguage& kernelLanguage() { return m_language; }

private:
    struct Staging;
    bool ensureStaging(const UpscaleResolution& res);
    void releaseStaging();

    Fsr3BackendBinding m_binding{};
    upscale::UpscalerCaps m_caps{};
    upscale::HaltonJitterProvider m_jitter{};
    Fsr3Gpu m_gpu;
    std::unique_ptr<Staging> m_staging;
    u64 m_serial = 0;
    u32 m_generation = 0;
    u32 m_accumulated = 0;
    upscale::HistoryResetReason m_lastReason = upscale::HistoryResetReason::None;
    bool m_pendingReset = false;
    UpscaleResolution m_lastResolution{};
    f32 m_sharpness = 0.f;
    bool m_flipJitter = false;
    Fsr3KernelLanguage m_language = Fsr3KernelLanguage::Auto;
};

} // namespace fuse::renderer::fsr3
