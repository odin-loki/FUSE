// Built-in upscaler backends: native_taau (TaauUpscaler adapter), fsr1, nis, cas (see upscaler.hpp).

#include <fuse/renderer/taa/taau.hpp>
#include <fuse/renderer/upscale/upscale_passes.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>

#include <vector>

namespace fuse::renderer::upscale {

namespace {

constexpr u8 kCpuApis = api_bit(UpscalerApi::Cpu);

PassOptions pass_options(const UpscaleDispatch& dispatch) {
    PassOptions options{};
    options.backend = dispatch.backend;
    return options;
}

// ---- fsr1 ---------------------------------------------------------------------------------------------

UpscalerCaps fsr1_caps() {
    UpscalerCaps c{};
    c.name = kFsr1Name;
    c.display_name = "AMD FidelityFX Super Resolution 1 (EASU + RCAS)";
    c.license = "MIT";
    c.kind = UpscalerKind::Spatial;
    c.built_in_sharpening = true;
    c.supports_dynamic_resolution = true;
    c.min_ratio = 1.f;
    c.max_ratio = 4.f;
    // NativeAA is excluded: FSR1 is not an anti-aliaser (use cas / rcas for 1x sharpening).
    c.quality_modes = static_cast<u8>(quality_mode_bit(QualityMode::UltraQuality) | quality_mode_bit(QualityMode::Quality) |
                                      quality_mode_bit(QualityMode::Balanced) | quality_mode_bit(QualityMode::Performance) |
                                      quality_mode_bit(QualityMode::UltraPerformance));
    c.apis = kCpuApis;
    return c;
}

/// EASU into an owned display-resolution intermediate, then RCAS into the output
/// (the SDK's FFX_FSR1_OPTION_APPLY_RCAS=1 pipeline).
class Fsr1Upscaler final : public ISpatialUpscaler {
public:
    const UpscalerCaps& caps() const override { return m_caps; }

    UpscaleStatus evaluate(const UpscaleDispatch& dispatch, const SpatialUpscaleInputs& inputs,
                           const UpscaleOutputs& outputs) override {
        const UpscaleStatus status = validate(inputs, outputs);
        if (status != UpscaleStatus::Ok) {
            return status;
        }
        const RgbaImage& out = outputs.color;
        m_intermediate.resize(static_cast<usize>(out.width) * out.height);
        const RgbaImage easu{m_intermediate.data(), out.width, out.height};
        const PassOptions options = pass_options(dispatch);
        if (!run_easu(inputs.color, easu, options)) {
            return UpscaleStatus::LaunchFailed;
        }
        const ConstRgbaImage easuIn{m_intermediate.data(), out.width, out.height};
        if (!run_rcas(easuIn, out, rcas_stops_from_sharpness(inputs.sharpness), options)) {
            return UpscaleStatus::LaunchFailed;
        }
        return UpscaleStatus::Ok;
    }

private:
    UpscalerCaps m_caps = fsr1_caps();
    std::vector<math::Vec4> m_intermediate;
};

// ---- nis ----------------------------------------------------------------------------------------------

UpscalerCaps nis_caps() {
    UpscalerCaps c{};
    c.name = kNisName;
    c.display_name = "NVIDIA Image Scaling (NVScaler)";
    c.license = "MIT";
    c.kind = UpscalerKind::Spatial;
    c.built_in_sharpening = true;
    c.supports_dynamic_resolution = true;
    c.min_ratio = 1.f;
    c.max_ratio = 2.f; // NVScalerUpdateConfig rejects scale factors below 0.5.
    c.quality_modes = static_cast<u8>(quality_mode_bit(QualityMode::UltraQuality) | quality_mode_bit(QualityMode::Quality) |
                                      quality_mode_bit(QualityMode::Balanced) | quality_mode_bit(QualityMode::Performance));
    c.apis = kCpuApis;
    return c;
}

class NisUpscaler final : public ISpatialUpscaler {
public:
    const UpscalerCaps& caps() const override { return m_caps; }

    UpscaleStatus evaluate(const UpscaleDispatch& dispatch, const SpatialUpscaleInputs& inputs,
                           const UpscaleOutputs& outputs) override {
        const UpscaleStatus status = validate(inputs, outputs);
        if (status != UpscaleStatus::Ok) {
            return status;
        }
        return run_nis(inputs.color, outputs.color, inputs.sharpness, pass_options(dispatch))
                   ? UpscaleStatus::Ok
                   : UpscaleStatus::LaunchFailed;
    }

private:
    UpscalerCaps m_caps = nis_caps();
};

// ---- cas ----------------------------------------------------------------------------------------------

UpscalerCaps cas_caps() {
    UpscalerCaps c{};
    c.name = kCasName;
    c.display_name = "AMD FidelityFX Contrast Adaptive Sharpening";
    c.license = "MIT";
    c.kind = UpscalerKind::Sharpen;
    c.built_in_sharpening = true;
    c.min_ratio = 1.f;
    c.max_ratio = 1.f;
    c.quality_modes = quality_mode_bit(QualityMode::NativeAA);
    c.apis = kCpuApis;
    return c;
}

/// Same-resolution CAS (the post-process sharpener as a registry backend).
class CasSharpener final : public ISpatialUpscaler {
public:
    const UpscalerCaps& caps() const override { return m_caps; }

    UpscaleStatus evaluate(const UpscaleDispatch& dispatch, const SpatialUpscaleInputs& inputs,
                           const UpscaleOutputs& outputs) override {
        const UpscaleStatus status = validate(inputs, outputs);
        if (status != UpscaleStatus::Ok) {
            return status;
        }
        return run_cas(inputs.color, outputs.color, inputs.sharpness, pass_options(dispatch))
                   ? UpscaleStatus::Ok
                   : UpscaleStatus::LaunchFailed;
    }

private:
    UpscalerCaps m_caps = cas_caps();
};

// ---- native_taau --------------------------------------------------------------------------------------
//
// Adapter over renderer::TaauUpscaler (taa/taau.hpp): the single-source "taau" kernel written by the TAAU
// work stream (display-resolution history, Lanczos accumulation, variance clipping, depth / velocity /
// reactive rejection). The adapter adds the ITemporalUpscaler bookkeeping: history generation and reset
// reasons (resolution change, reset_history, explicit invalidation) and the FSR-style jitter provider.

UpscalerCaps native_taau_caps() {
    UpscalerCaps c{};
    c.name = kNativeTaauName;
    c.display_name = "FUSE native TAAU";
    c.license = "MIT";
    c.kind = UpscalerKind::Temporal;
    c.temporal = true;
    c.needs_depth = true;
    c.needs_motion_vectors = true;
    c.needs_jitter = true;
    c.needs_exposure = true; // validateUpscaleInputs requires exposure > 0 (1 when colour is already exposed).
    c.accepts_exposure = true;
    c.accepts_reactive_mask = true;
    c.accepts_transparency_mask = true;
    c.hdr_input = true;
    c.supports_dynamic_resolution = true;
    c.min_ratio = 1.f;
    c.max_ratio = 3.f;
    c.quality_modes = kAllQualityModes;
    c.apis = kCpuApis;
    return c;
}

class NativeTaauAdapter final : public ITemporalUpscaler {
public:
    const UpscalerCaps& caps() const override { return m_caps; }
    const IJitterProvider& jitter_provider() const override { return m_jitter; }

    void invalidate_history(HistoryResetReason reason) override {
        m_taau.invalidate();
        m_accumulated = 0;
        ++m_generation;
        m_lastReset = reason;
    }
    u32 history_generation() const override { return m_generation; }
    HistoryResetReason last_reset_reason() const override { return m_lastReset; }
    u32 accumulated_frames() const override { return m_accumulated; }

    fuse::renderer::TaauUpscaler& taau() { return m_taau; }

    UpscaleStatus evaluate(const UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                           const TemporalUpscaleOutputs& outputs) override {
        const UpscaleStatus status = validate(inputs, outputs);
        if (status != UpscaleStatus::Ok) {
            return status;
        }
        const fuse::renderer::UpscaleResolution& r = inputs.resolution;
        const bool resized = r.render_width != m_resolution.render_width ||
                             r.render_height != m_resolution.render_height ||
                             r.display_width != m_resolution.display_width ||
                             r.display_height != m_resolution.display_height;
        if (resized) {
            invalidate_history(HistoryResetReason::ResolutionChange);
            m_resolution = r;
        } else if (inputs.reset_history) {
            invalidate_history(HistoryResetReason::CameraCut);
        }
        if (!m_taau.upscale(inputs, outputs.color.data, dispatch.backend)) {
            return UpscaleStatus::LaunchFailed;
        }
        ++m_accumulated;
        return UpscaleStatus::Ok;
    }

private:
    UpscalerCaps m_caps = native_taau_caps();
    HaltonJitterProvider m_jitter{};
    fuse::renderer::TaauUpscaler m_taau{};
    fuse::renderer::UpscaleResolution m_resolution{};
    u32 m_generation = 0;
    u32 m_accumulated = 0;
    HistoryResetReason m_lastReset = HistoryResetReason::None;
};

template <typename T>
std::unique_ptr<IUpscaler> make_backend() {
    return std::make_unique<T>();
}

} // namespace

void register_builtin_upscalers(UpscalerRegistry& registry) {
    // Registration order is the selection preference order (UpscalerRegistry::select).
    registry.register_backend(native_taau_caps(), &make_backend<NativeTaauAdapter>);
    registry.register_backend(fsr1_caps(), &make_backend<Fsr1Upscaler>);
    registry.register_backend(nis_caps(), &make_backend<NisUpscaler>);
    registry.register_backend(cas_caps(), &make_backend<CasSharpener>);
}

} // namespace fuse::renderer::upscale
