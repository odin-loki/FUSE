#include <fuse/renderer/nvidia/nv_backends.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace fuse::renderer::nvidia {

namespace {

using upscale::HistoryResetReason;
using upscale::UpscaleStatus;

std::mutex g_pluginMutex;
std::shared_ptr<NvPlugin> g_plugin; // bound by register_nvidia_backends, read by the registry factories

// General 4x4 inverse (column-major, column-vector); false when singular.
bool invert(const math::Mat4& m, math::Mat4& out) {
    const float* a = m.data.data();
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::fabs(det) > 1e-30f) || !std::isfinite(det)) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        out.data[static_cast<usize>(i)] = inv[i] / det;
    }
    return true;
}

// A column-major, column-vector FUSE matrix M has the same 16 floats, in memory order, as the row-major
// row-vector matrix M^T Streamline expects (element (r, c) of M^T = M(c, r) = data[r * 4 + c]).
void to_row_vector(const math::Mat4& m, float (&out)[16]) {
    std::memcpy(out, m.data.data(), sizeof(out));
}

void fill_camera(const UpscaleInputs& in, FuseNvConstants& c) {
    const UpscaleCamera& cam = in.camera;
    const UpscaleCamera& prev = in.previous_camera;
    to_row_vector(cam.projection, c.camera_view_to_clip);
    math::Mat4 inv{};
    if (invert(cam.projection, inv)) {
        to_row_vector(inv, c.clip_to_camera_view);
    }
    to_row_vector(cam.view, c.world_to_camera_view);
    math::Mat4 view_to_world{};
    if (invert(cam.view, view_to_world)) {
        to_row_vector(view_to_world, c.camera_view_to_world);
        // Camera basis in world space: view looks down -Z.
        c.camera_right[0] = view_to_world.at(0, 0);
        c.camera_right[1] = view_to_world.at(1, 0);
        c.camera_right[2] = view_to_world.at(2, 0);
        c.camera_up[0] = view_to_world.at(0, 1);
        c.camera_up[1] = view_to_world.at(1, 1);
        c.camera_up[2] = view_to_world.at(2, 1);
        c.camera_fwd[0] = -view_to_world.at(0, 2);
        c.camera_fwd[1] = -view_to_world.at(1, 2);
        c.camera_fwd[2] = -view_to_world.at(2, 2);
    }
    const math::Mat4 vp = cam.projection * cam.view;
    const math::Mat4 prev_vp = prev.projection * prev.view;
    math::Mat4 inv_vp{}, inv_prev_vp{};
    if (invert(vp, inv_vp)) {
        to_row_vector(prev_vp * inv_vp, c.clip_to_prev_clip);
    }
    if (invert(prev_vp, inv_prev_vp)) {
        to_row_vector(vp * inv_prev_vp, c.prev_clip_to_clip);
    }
    c.camera_pos[0] = cam.position.x;
    c.camera_pos[1] = cam.position.y;
    c.camera_pos[2] = cam.position.z;
    c.camera_near = cam.near_plane;
    c.camera_far = cam.far_plane;
    c.camera_vfov_rad = cam.vertical_fov_rad;
    c.camera_aspect = cam.aspect;
}

bool is_display_kind(FuseNvBufferKind k) {
    return k == FUSE_NV_BUFFER_COLOR_OUT || k == FUSE_NV_BUFFER_HUDLESS_COLOR || k == FUSE_NV_BUFFER_UI_COLOR_ALPHA;
}

upscale::UpscalerCaps make_caps(FuseNvFeature feature) {
    upscale::UpscalerCaps c{};
    const bool rr = feature == FUSE_NV_FEATURE_DLSS_RR;
    c.name = rr ? kDlssRrName : kDlssSrName;
    c.display_name = rr ? "NVIDIA DLSS Ray Reconstruction" : "NVIDIA DLSS Super Resolution";
    // Adapter is MIT; the runtime it drives is the developer's NVIDIA RTX SDKs-licensed copy.
    c.license = "LicenseRef-NVIDIA-RTX-SDKs";
    c.kind = upscale::UpscalerKind::Temporal;
    c.temporal = true;
    c.needs_depth = true;
    c.needs_motion_vectors = true;
    c.needs_jitter = true;
    c.accepts_reactive_mask = !rr;
    c.accepts_transparency_mask = !rr;
    c.accepts_exposure = true;
    c.hdr_input = true;
    c.supports_dynamic_resolution = !rr; // RR does not support dynamic resolution scaling
    c.min_ratio = 1.f;
    c.max_ratio = 3.f;
    c.quality_modes = upscale::kAllQualityModes;
    c.apis = upscale::api_bit(upscale::UpscalerApi::Vulkan); // never selected for CPU dispatch
    return c;
}

const upscale::UpscalerCaps& caps_for(FuseNvFeature feature) {
    static const upscale::UpscalerCaps kSr = make_caps(FUSE_NV_FEATURE_DLSS_SR);
    static const upscale::UpscalerCaps kRr = make_caps(FUSE_NV_FEATURE_DLSS_RR);
    return feature == FUSE_NV_FEATURE_DLSS_RR ? kRr : kSr;
}

std::shared_ptr<NvPlugin> current_plugin() {
    std::lock_guard<std::mutex> lock(g_pluginMutex);
    return g_plugin;
}

std::unique_ptr<upscale::IUpscaler> make_dlss_sr() {
    return std::make_unique<NvTemporalUpscaler>(FUSE_NV_FEATURE_DLSS_SR, current_plugin());
}

std::unique_ptr<upscale::IUpscaler> make_dlss_rr() {
    return std::make_unique<NvTemporalUpscaler>(FUSE_NV_FEATURE_DLSS_RR, current_plugin());
}

FuseNvStatus run_feature(const NvPlugin& p, const NvFrameInputs& in, u32 viewport, u64 cmd, const FuseNvFeatureOptions& o) {
    const std::vector<FuseNvResourceTag> tags = in.packed();
    FuseNvStatus s = p.api().set_tags(p.context(), in.constants.frame_index, viewport, tags.data(), u32(tags.size()), cmd);
    if (s == FUSE_NV_OK) {
        s = p.api().set_constants(p.context(), viewport, &in.constants);
    }
    if (s == FUSE_NV_OK) {
        s = p.api().evaluate(p.context(), in.constants.frame_index, viewport, &o, cmd);
    }
    return s;
}

FuseNvFeatureOptions make_options(FuseNvFeature f) {
    FuseNvFeatureOptions o{};
    o.struct_size = sizeof(o);
    o.feature = f;
    o.quality = FUSE_NV_QUALITY_QUALITY;
    o.frames_to_generate = 1;
    return o;
}

} // namespace

FuseNvQuality to_nv_quality(upscale::QualityMode mode) noexcept {
    switch (mode) {
        case upscale::QualityMode::NativeAA: return FUSE_NV_QUALITY_DLAA;
        case upscale::QualityMode::UltraQuality: return FUSE_NV_QUALITY_ULTRA_QUALITY;
        case upscale::QualityMode::Quality: return FUSE_NV_QUALITY_QUALITY;
        case upscale::QualityMode::Balanced: return FUSE_NV_QUALITY_BALANCED;
        case upscale::QualityMode::Performance: return FUSE_NV_QUALITY_PERFORMANCE;
        case upscale::QualityMode::UltraPerformance: return FUSE_NV_QUALITY_ULTRA_PERFORMANCE;
    }
    return FUSE_NV_QUALITY_QUALITY;
}

NvFrameInputs map_upscale_inputs(const UpscaleInputs& in, const NvGpuFrame& gpu, FuseNvFeature feature) {
    NvFrameInputs out;
    FuseNvConstants& c = out.constants;
    const UpscaleResolution& res = in.resolution;
    const bool nr = feature == FUSE_NV_FEATURE_DLSS_NR;
    c.frame_index = in.frame_index;
    c.render_width = nr ? res.display_width : res.render_width;
    c.render_height = nr ? res.display_height : res.render_height;
    c.output_width = res.display_width;
    c.output_height = res.display_height;
    c.jitter_offset_px[0] = nr ? 0.f : in.jitter_px.x;
    c.jitter_offset_px[1] = nr ? 0.f : in.jitter_px.y;
    // UpscaleInputs::motion is UV current - previous; Streamline wants previous - current in [-1, 1].
    c.mvec_scale[0] = -1.f;
    c.mvec_scale[1] = -1.f;
    c.exposure_scale = in.exposure;
    c.pre_exposure = 1.f;
    c.frame_time_ms = in.frame_time_s * 1000.f;
    c.flags = FUSE_NV_CONST_CAMERA_MOTION_INCLUDED;
    c.flags |= gpu.depth_reversed_z ? FUSE_NV_CONST_DEPTH_INVERTED : 0u;
    c.flags |= gpu.hdr ? FUSE_NV_CONST_HDR : 0u;
    c.flags |= in.reset_history ? FUSE_NV_CONST_RESET : 0u;
    fill_camera(in, c);

    for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        FuseNvResource r = gpu.resources[k];
        if (r.native == 0u) {
            continue;
        }
        if (r.width == 0u || r.height == 0u) { // host did not describe the extent: assume the contract's
            const bool display = is_display_kind(k) || nr;
            r.width = display ? res.display_width : res.render_width;
            r.height = display ? res.display_height : res.render_height;
            if (k == FUSE_NV_BUFFER_EXPOSURE) {
                r.width = r.height = 1u;
            }
        }
        out.set(k, r);
    }
    return out;
}

upscale::UpscaleStatus to_upscale_status(FuseNvStatus status) noexcept {
    switch (status) {
        case FUSE_NV_OK: return UpscaleStatus::Ok;
        case FUSE_NV_ERR_MISSING_INPUT:
        case FUSE_NV_ERR_MISSING_CONSTANTS:
        case FUSE_NV_ERR_INVALID_CONSTANTS:
        case FUSE_NV_ERR_INVALID_ARGUMENT: return UpscaleStatus::InvalidInputs;
        case FUSE_NV_ERR_UNSUPPORTED_FEATURE:
        case FUSE_NV_ERR_NOT_INITIALIZED:
        case FUSE_NV_ERR_RUNTIME_MISSING:
        case FUSE_NV_ERR_NO_NVIDIA_GPU:
        case FUSE_NV_ERR_DRIVER_TOO_OLD:
        case FUSE_NV_ERR_GRAPHICS_API: return UpscaleStatus::BackendUnavailable;
        default: return UpscaleStatus::LaunchFailed;
    }
}

// ---- NvTemporalUpscaler ---------------------------------------------------------------------------------

NvTemporalUpscaler::NvTemporalUpscaler(FuseNvFeature feature, std::shared_ptr<NvPlugin> plugin)
    : m_feature(feature == FUSE_NV_FEATURE_DLSS_RR ? FUSE_NV_FEATURE_DLSS_RR : FUSE_NV_FEATURE_DLSS_SR),
      m_plugin(std::move(plugin)) {}

const upscale::UpscalerCaps& NvTemporalUpscaler::caps() const { return caps_for(m_feature); }

upscale::Extent2D NvTemporalUpscaler::render_size(upscale::Extent2D display, upscale::QualityMode mode) const {
    if (m_plugin && m_plugin->supports(m_feature) && display.valid()) {
        FuseNvRenderSize rs{};
        rs.struct_size = sizeof(rs);
        if (m_plugin->api().get_render_size(m_plugin->context(), m_feature, to_nv_quality(mode), display.width, display.height,
                                            &rs) == FUSE_NV_OK &&
            rs.render_width && rs.render_height) {
            return {rs.render_width, rs.render_height};
        }
    }
    return upscale::render_extent(display, mode);
}

void NvTemporalUpscaler::invalidate_history(HistoryResetReason reason) {
    ++m_generation;
    m_lastReason = reason;
    m_accumulated = 0;
    m_pendingReset = true;
}

void NvTemporalUpscaler::bind_gpu_frame(const NvGpuFrame& frame) {
    m_gpu = frame;
    m_gpuBound = true;
}

upscale::UpscaleStatus NvTemporalUpscaler::evaluate(const upscale::UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                                    const upscale::TemporalUpscaleOutputs& outputs) {
    (void)dispatch; // GPU-only: the kernel backend selector does not apply
    (void)outputs;  // result is written to the bound COLOR_OUT resource
    m_lastValidation = ValidationReport{};
    if (!m_plugin || !m_plugin->supports(m_feature)) {
        return UpscaleStatus::BackendUnavailable;
    }
    if (!m_gpuBound) {
        return UpscaleStatus::BackendUnavailable; // no native resources for this frame
    }
    m_gpuBound = false; // one bind per frame
    const UpscaleResolution& r = inputs.resolution;
    if (!r.valid()) {
        return UpscaleStatus::InvalidInputs;
    }
    const f32 ratio = std::max(r.scaleX(), r.scaleY());
    if (!caps().supports_ratio(ratio)) {
        return UpscaleStatus::UnsupportedRatio;
    }
    if (m_lastRenderW != r.render_width || m_lastRenderH != r.render_height) {
        if (m_lastRenderW != 0u) {
            invalidate_history(HistoryResetReason::ResolutionChange);
        }
        m_lastRenderW = r.render_width;
        m_lastRenderH = r.render_height;
    } else if (inputs.reset_history) {
        invalidate_history(HistoryResetReason::CameraCut);
    }
    NvFrameInputs nv = map_upscale_inputs(inputs, m_gpu, m_feature);
    if (m_pendingReset) {
        nv.constants.flags |= FUSE_NV_CONST_RESET;
    }
    m_lastValidation = validate_inputs(m_feature, nv);
    if (!m_lastValidation.ok()) {
        return UpscaleStatus::InvalidInputs;
    }
    FuseNvFeatureOptions o = make_options(m_feature);
    o.quality = to_nv_quality(m_quality);
    m_lastProvider = run_feature(*m_plugin, nv, m_gpu.viewport, m_gpu.command_buffer, o);
    if (m_lastProvider != FUSE_NV_OK) {
        return to_upscale_status(m_lastProvider);
    }
    m_pendingReset = false;
    ++m_accumulated;
    return UpscaleStatus::Ok;
}

// ---- extensions -----------------------------------------------------------------------------------------

NvExtensionRegistry& NvExtensionRegistry::instance() {
    static NvExtensionRegistry registry;
    return registry;
}

bool NvExtensionRegistry::add(const NvExtensionCaps& caps) {
    if (!caps.name || !*caps.name || find(caps.name)) {
        return false;
    }
    m_entries.push_back(caps);
    return true;
}

bool NvExtensionRegistry::remove(std::string_view name) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const NvExtensionCaps& c) { return name == c.name; });
    if (it == m_entries.end()) {
        return false;
    }
    m_entries.erase(it);
    return true;
}

const NvExtensionCaps* NvExtensionRegistry::find(std::string_view name) const {
    for (const NvExtensionCaps& c : m_entries) {
        if (name == c.name) {
            return &c;
        }
    }
    return nullptr;
}

NvLookPlacement plan_dlss5_look_placement(const look::LookEffectGraph& graph) {
    NvLookPlacement p;
    const look::LookGraphValidation v = graph.validate();
    if (!v.ok()) {
        p.reason = "look graph does not validate";
        return p;
    }
    u32 i = 0;
    while (i < graph.size() && look::look_effect_info(graph.at(i)).stage < look::LookStage::PostUpscaleHdr) {
        ++i;
    }
    // The node consumes and produces scene-referred HDR: whatever follows must accept SceneHdr.
    if (i < graph.size()) {
        const look::LookDomain next = look::look_effect_info(graph.at(i)).input;
        if (next != look::LookDomain::SceneHdr && next != look::LookDomain::Any) {
            p.reason = "no scene-referred HDR slot after the upscaler";
            return p;
        }
    }
    p.ok = true;
    p.insert_index = i;
    p.reason = "after the temporal upscaler, before the first display-resolution HDR node";
    return p;
}

bool NvFrameGenerator::available() const { return m_plugin && m_plugin->supports(FUSE_NV_FEATURE_DLSS_FG); }

u32 NvFrameGenerator::max_generated_frames() const {
    if (!available()) {
        return 0u;
    }
    return m_plugin->adapter().rtx_generation >= 50u ? 5u : 1u;
}

FuseNvStatus NvFrameGenerator::present(const UpscaleInputs& inputs, const NvGpuFrame& gpu, u32 frames_to_generate) {
    if (!available()) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    if (frames_to_generate < 1u || frames_to_generate > max_generated_frames()) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const NvFrameInputs nv = map_upscale_inputs(inputs, gpu, FUSE_NV_FEATURE_DLSS_FG);
    m_lastValidation = validate_inputs(FUSE_NV_FEATURE_DLSS_FG, nv);
    if (!m_lastValidation.ok()) {
        return to_plugin_status(m_lastValidation.status());
    }
    FuseNvFeatureOptions o = make_options(FUSE_NV_FEATURE_DLSS_FG);
    o.frames_to_generate = frames_to_generate;
    return run_feature(*m_plugin, nv, gpu.viewport, gpu.command_buffer, o);
}

bool NvNeuralLookPass::available() const { return m_plugin && m_plugin->supports(FUSE_NV_FEATURE_DLSS_NR); }

FuseNvStatus NvNeuralLookPass::evaluate(const UpscaleInputs& inputs, const NvGpuFrame& gpu, f32 structure_intensity,
                                        f32 tone_intensity) {
    if (!available()) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    const NvFrameInputs nv = map_upscale_inputs(inputs, gpu, FUSE_NV_FEATURE_DLSS_NR);
    m_lastValidation = validate_inputs(FUSE_NV_FEATURE_DLSS_NR, nv);
    if (!m_lastValidation.ok()) {
        return to_plugin_status(m_lastValidation.status());
    }
    FuseNvFeatureOptions o = make_options(FUSE_NV_FEATURE_DLSS_NR);
    o.nr_structure_intensity = std::clamp(structure_intensity, 0.f, 1.f);
    o.nr_tone_intensity = std::clamp(tone_intensity, 0.f, 1.f);
    return run_feature(*m_plugin, nv, gpu.viewport, gpu.command_buffer, o);
}

// ---- registration -----------------------------------------------------------------------------------------

NvRegistration register_nvidia_backends(upscale::UpscalerRegistry& upscalers, NvExtensionRegistry& extensions,
                                        std::shared_ptr<NvPlugin> plugin) {
    NvRegistration reg;
    if (!plugin) {
        return reg;
    }
    {
        std::lock_guard<std::mutex> lock(g_pluginMutex);
        g_plugin = plugin;
    }
    if (plugin->supports(FUSE_NV_FEATURE_DLSS_SR) && upscalers.register_backend(caps_for(FUSE_NV_FEATURE_DLSS_SR), &make_dlss_sr)) {
        reg.feature_mask |= FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_SR);
        reg.names.push_back(kDlssSrName);
    }
    if (plugin->supports(FUSE_NV_FEATURE_DLSS_RR) && upscalers.register_backend(caps_for(FUSE_NV_FEATURE_DLSS_RR), &make_dlss_rr)) {
        reg.feature_mask |= FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_RR);
        reg.names.push_back(kDlssRrName);
    }
    if (plugin->supports(FUSE_NV_FEATURE_DLSS_FG) && plugin->supports(FUSE_NV_FEATURE_REFLEX)) {
        NvExtensionCaps fg{};
        fg.name = kDlssFgName;
        fg.display_name = "NVIDIA DLSS Frame Generation";
        fg.license = "LicenseRef-NVIDIA-RTX-SDKs";
        fg.kind = NvExtensionKind::FrameGeneration;
        fg.feature = FUSE_NV_FEATURE_DLSS_FG;
        fg.min_rtx_generation = plugin->feature(FUSE_NV_FEATURE_DLSS_FG).min_rtx_generation;
        fg.max_generated_frames = NvFrameGenerator(plugin).max_generated_frames();
        fg.requires_reflex = true;
        fg.requires_hudless_color = true;
        if (extensions.add(fg)) {
            reg.feature_mask |= FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_FG);
            reg.names.push_back(kDlssFgName);
        }
    }
    if (plugin->supports(FUSE_NV_FEATURE_DLSS_NR)) {
        NvExtensionCaps nr{};
        nr.name = kDlss5LookName;
        nr.display_name = "NVIDIA DLSS 5 (3D-Guided Neural Rendering)";
        nr.license = "LicenseRef-NVIDIA-RTX-SDKs";
        nr.kind = NvExtensionKind::LookNode;
        nr.feature = FUSE_NV_FEATURE_DLSS_NR;
        nr.min_rtx_generation = plugin->feature(FUSE_NV_FEATURE_DLSS_NR).min_rtx_generation;
        nr.stage = look::LookStage::PostUpscaleHdr;
        nr.input = look::LookDomain::SceneHdr;
        nr.output = look::LookDomain::SceneHdr;
        if (extensions.add(nr)) {
            reg.feature_mask |= FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_NR);
            reg.names.push_back(kDlss5LookName);
        }
    }
    return reg;
}

void unregister_nvidia_backends(upscale::UpscalerRegistry& upscalers, NvExtensionRegistry& extensions) {
    upscalers.unregister_backend(kDlssSrName);
    upscalers.unregister_backend(kDlssRrName);
    extensions.remove(kDlssFgName);
    extensions.remove(kDlss5LookName);
    std::lock_guard<std::mutex> lock(g_pluginMutex);
    g_plugin.reset();
}

std::shared_ptr<NvPlugin> registered_nvidia_plugin() { return current_plugin(); }

LoadResult probe_and_register_nvidia_backends(const PluginConfig& project) {
    LoadResult r = NvPluginLoader::probe(project);
    if (r.status != PluginStatus::Available || !r.plugin) {
        return r;
    }
    std::shared_ptr<NvPlugin> shared(std::move(r.plugin));
    const NvRegistration reg =
        register_nvidia_backends(upscale::UpscalerRegistry::instance(), NvExtensionRegistry::instance(), shared);
    if (reg.feature_mask == 0u) {
        r.status = PluginStatus::NoFeatures;
        r.detail += " (nothing registered)";
    }
    return r;
}

} // namespace fuse::renderer::nvidia
