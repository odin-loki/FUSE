// Gates: NVIDIA backends appear in the upscaler registry / extension registry only when the provider
// detects them, UpscaleInputs map onto provider inputs correctly, and the look-graph placement of
// "dlss5_look" is legal. Driven by the in-tree MIT mock provider (docs/nvidia-plugin.md).

#include "fuse_nv_mock_echo.h"

#include <fuse/math/mat.hpp>
#include <fuse/renderer/nvidia/nv_backends.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace nv = fuse::renderer::nvidia;
namespace up = fuse::renderer::upscale;
namespace lk = fuse::renderer::look;
namespace math = fuse::math;

namespace {

using u32 = fuse::u32;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::shared_ptr<nv::NvPlugin> load(const char* options) {
    nv::PluginConfig c;
    c.options = options;
    nv::LoadResult r = nv::NvPluginLoader::load_file(FUSE_NV_TEST_MOCK, c);
    check(r.status == nv::PluginStatus::Available, std::string("mock loads with '") + options + "': " + r.detail);
    return std::shared_ptr<nv::NvPlugin>(std::move(r.plugin));
}

FuseNvMockEcho echo(const nv::NvPlugin& p) {
    FuseNvMockEcho e{};
    if (auto fn = reinterpret_cast<FuseNvMockGetEchoFn>(p.symbol(FUSE_NV_MOCK_ECHO_NAME))) {
        fn(&e);
    }
    return e;
}

fuse::renderer::UpscaleCamera camera(const math::Vec3& eye) {
    fuse::renderer::UpscaleCamera c;
    c.view = math::lookAt(eye, eye + math::Vec3(0.f, 0.f, -1.f), math::Vec3(0.f, 1.f, 0.f));
    c.projection = math::perspective(60.f, 16.f / 9.f, 0.1f, 500.f);
    c.position = eye;
    c.vertical_fov_rad = 60.f * 3.14159265f / 180.f;
    c.aspect = 16.f / 9.f;
    c.near_plane = 0.1f;
    c.far_plane = 500.f;
    return c;
}

up::UpscaleInputs inputs(u32 frame, bool moved = true) {
    up::UpscaleInputs in;
    in.resolution = fuse::renderer::makeUpscaleResolution(1920, 1080, up::QualityMode::Performance);
    in.jitter_px = math::Vec2(0.3125f, -0.1875f);
    in.exposure = 0.5f;
    in.frame_index = frame;
    in.camera = camera(math::Vec3(moved ? 0.25f : 0.f, 1.f, 5.f));
    in.previous_camera = camera(math::Vec3(0.f, 1.f, 5.f));
    return in;
}

nv::NvGpuFrame gpu(FuseNvFeature feature) {
    nv::NvGpuFrame g;
    const u32 required = nv::required_buffer_mask(feature);
    for (u32 k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        if (required & (1u << k)) {
            FuseNvResource r{};
            r.native = 0x5000u + k; // extents left 0: the mapping fills the contract's sizes
            g.set(k, r);
        }
    }
    g.viewport = 2;
    g.command_buffer = 0xC0FFEEu;
    return g;
}

bool has(const up::UpscalerRegistry& r, const char* name) { return r.find(name) != nullptr; }

void test_registration_follows_detection() {
    up::UpscalerRegistry reg;
    reg.register_builtin_backends();
    nv::NvExtensionRegistry ext;
    const size_t builtin = reg.backends().size();

    const nv::NvRegistration none = nv::register_nvidia_backends(reg, ext, nullptr);
    check(none.feature_mask == 0u && reg.backends().size() == builtin && ext.entries().empty(), "no plugin -> nothing registered");

    struct Case {
        const char* options;
        bool sr, rr, fg, look;
        u32 fg_frames;
    } cases[] = {
        {"gpu=rtx20", true, true, false, false, 0},
        {"gpu=rtx40", true, true, true, false, 1},
        {"gpu=rtx50", true, true, true, true, 5},
        {"gpu=rtx50;disable=sr", false, true, true, true, 5},
        {"gpu=rtx50;disable=reflex", true, true, false, true, 0},
    };
    for (const Case& k : cases) {
        const std::string tag = std::string("[") + k.options + "] ";
        std::shared_ptr<nv::NvPlugin> p = load(k.options);
        const nv::NvRegistration r = nv::register_nvidia_backends(reg, ext, p);
        check(has(reg, nv::kDlssSrName) == k.sr, tag + "dlss_sr registered iff detected");
        check(has(reg, nv::kDlssRrName) == k.rr, tag + "dlss_rr registered iff detected");
        check((ext.find(nv::kDlssFgName) != nullptr) == k.fg, tag + "dlss_fg registered iff FG + Reflex detected");
        check((ext.find(nv::kDlss5LookName) != nullptr) == k.look, tag + "dlss5_look registered iff RTX 50 NR detected");
        if (const nv::NvExtensionCaps* fg = ext.find(nv::kDlssFgName)) {
            check(fg->max_generated_frames == k.fg_frames && fg->requires_reflex && fg->min_rtx_generation == 40u,
                  tag + "dlss_fg caps (MFG up to 5 generated frames on RTX 50 only)");
        }
        if (const nv::NvExtensionCaps* lookNode = ext.find(nv::kDlss5LookName)) {
            check(lookNode->kind == nv::NvExtensionKind::LookNode && lookNode->min_rtx_generation == 50u &&
                      lookNode->stage == lk::LookStage::PostUpscaleHdr && lookNode->input == lk::LookDomain::SceneHdr,
                  tag + "dlss5_look is an RTX 50 post-upscale HDR look node");
        }
        if (const up::UpscalerCaps* sr = reg.find(nv::kDlssSrName)) {
            check(sr->temporal && sr->hdr_input && sr->needs_jitter && sr->supports_api(up::UpscalerApi::Vulkan) &&
                      !sr->supports_api(up::UpscalerApi::Cpu),
                  tag + "dlss_sr caps: temporal HDR, jittered, GPU-only");
            up::UpscalerRequirements cpu;
            cpu.api = up::UpscalerApi::Cpu;
            const up::UpscalerCaps* pick = reg.select(cpu);
            check(!pick || std::string(pick->name).rfind("dlss", 0) != 0, tag + "CPU selection never picks a DLSS backend");
            up::UpscalerRequirements gpuReq;
            gpuReq.api = up::UpscalerApi::Vulkan;
            gpuReq.allow_spatial = false;
            const up::UpscalerCaps* vk = reg.select(gpuReq);
            check(vk != nullptr, tag + "a Vulkan temporal backend is selectable");
        }
        check(nv::registered_nvidia_plugin() == p, tag + "factories bound to this plugin");
        check(static_cast<bool>(r.feature_mask & FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_SR)) == k.sr, tag + "registration mask");
        nv::unregister_nvidia_backends(reg, ext);
        check(!has(reg, nv::kDlssSrName) && !has(reg, nv::kDlssRrName) && ext.entries().empty() && reg.backends().size() == builtin &&
                  !nv::registered_nvidia_plugin(),
              tag + "unregister removes every NVIDIA backend");
    }
}

void test_upscaler_through_registry() {
    up::UpscalerRegistry reg;
    nv::NvExtensionRegistry ext;
    std::shared_ptr<nv::NvPlugin> p = load("gpu=rtx40");
    nv::register_nvidia_backends(reg, ext, p);
    std::unique_ptr<up::IUpscaler> base = reg.create(nv::kDlssSrName);
    auto* sr = dynamic_cast<nv::NvTemporalUpscaler*>(base.get());
    check(sr != nullptr, "registry creates an NvTemporalUpscaler for dlss_sr");
    if (!sr) {
        return;
    }
    const up::Extent2D rs = sr->render_size({1920, 1080}, up::QualityMode::Performance);
    check(rs.width == 960u && rs.height == 540u, "render_size from the provider (Performance 2x)");
    check(sr->jitter_provider().phase_count({960, 540}, {1920, 1080}) >= 8u, "Halton jitter provider");

    up::TemporalUpscaleOutputs out{};
    up::UpscaleInputs in = inputs(100);
    check(sr->evaluate({}, in, out) == up::UpscaleStatus::BackendUnavailable, "no GPU frame bound -> BackendUnavailable");

    sr->set_quality(up::QualityMode::Performance);
    sr->bind_gpu_frame(gpu(FUSE_NV_FEATURE_DLSS_SR));
    check(sr->evaluate({}, in, out) == up::UpscaleStatus::Ok, "dlss_sr evaluate through the mock");
    FuseNvMockEcho e = echo(*p);
    check(e.last_feature == FUSE_NV_FEATURE_DLSS_SR && e.last_frame_index == 100u && e.last_viewport == 2u, "echo: feature/frame/viewport");
    check(e.last_render_width == 960u && e.last_output_width == 1920u && e.last_quality == FUSE_NV_QUALITY_PERFORMANCE,
          "echo: render/display split + quality");
    check(e.last_mvec_scale[0] == -1.f && e.last_mvec_scale[1] == -1.f, "UV current-previous motion -> mvec_scale (-1, -1)");
    check(e.last_jitter_px[0] == 0.3125f && e.last_jitter_px[1] == -0.1875f, "jitter forwarded in render pixels");
    check((e.last_flags & FUSE_NV_CONST_RESET) && (e.last_flags & FUSE_NV_CONST_HDR) && (e.last_flags & FUSE_NV_CONST_DEPTH_INVERTED),
          "first frame resets history; HDR + reversed-Z flags");
    check(sr->accumulated_frames() == 1u, "accumulated 1");

    sr->bind_gpu_frame(gpu(FUSE_NV_FEATURE_DLSS_SR));
    in = inputs(101);
    check(sr->evaluate({}, in, out) == up::UpscaleStatus::Ok && !(echo(*p).last_flags & FUSE_NV_CONST_RESET), "steady frame: no reset");

    const u32 gen = sr->history_generation();
    sr->bind_gpu_frame(gpu(FUSE_NV_FEATURE_DLSS_SR));
    in = inputs(102);
    in.reset_history = true;
    check(sr->evaluate({}, in, out) == up::UpscaleStatus::Ok && (echo(*p).last_flags & FUSE_NV_CONST_RESET) &&
              sr->history_generation() == gen + 1u && sr->last_reset_reason() == up::HistoryResetReason::CameraCut,
          "reset_history -> RESET flag + CameraCut invalidation");

    nv::NvGpuFrame missing = gpu(FUSE_NV_FEATURE_DLSS_SR);
    missing.resources[FUSE_NV_BUFFER_DEPTH] = FuseNvResource{};
    sr->bind_gpu_frame(missing);
    in = inputs(103);
    check(sr->evaluate({}, in, out) == up::UpscaleStatus::InvalidInputs &&
              sr->last_validation().status() == nv::InputStatus::MissingDepth,
          "unbound depth -> InvalidInputs / MissingDepth (never reaches the provider)");

    sr->bind_gpu_frame(gpu(FUSE_NV_FEATURE_DLSS_SR));
    in = inputs(104);
    in.resolution = fuse::renderer::makeUpscaleResolution(1920, 1080, up::QualityMode::Balanced);
    check(sr->evaluate({}, in, out) == up::UpscaleStatus::Ok && sr->last_reset_reason() == up::HistoryResetReason::ResolutionChange,
          "render size change -> ResolutionChange");

    std::unique_ptr<up::IUpscaler> rrBase = reg.create(nv::kDlssRrName);
    auto* rr = dynamic_cast<nv::NvTemporalUpscaler*>(rrBase.get());
    if (rr) {
        nv::NvGpuFrame g = gpu(FUSE_NV_FEATURE_DLSS_RR);
        g.resources[FUSE_NV_BUFFER_NORMALS] = FuseNvResource{};
        rr->bind_gpu_frame(g);
        check(rr->evaluate({}, inputs(110), out) == up::UpscaleStatus::InvalidInputs &&
                  rr->last_validation().status() == nv::InputStatus::MissingNormals,
              "dlss_rr without normals -> MissingNormals");
        rr->bind_gpu_frame(gpu(FUSE_NV_FEATURE_DLSS_RR));
        check(rr->evaluate({}, inputs(111), out) == up::UpscaleStatus::Ok && echo(*p).last_feature == FUSE_NV_FEATURE_DLSS_RR,
              "dlss_rr evaluate with all guides");
        check(!rr->caps().supports_dynamic_resolution, "RR: no dynamic resolution");
    }
    nv::unregister_nvidia_backends(reg, ext);
    // A backend created before unregistration keeps its plugin alive (shared ownership).
    sr->bind_gpu_frame(gpu(FUSE_NV_FEATURE_DLSS_SR));
    check(sr->evaluate({}, inputs(120), out) == up::UpscaleStatus::Ok, "existing backend survives unregistration");
}

void test_camera_mapping() {
    const up::UpscaleInputs in = inputs(5, true);
    const nv::NvFrameInputs f = nv::map_upscale_inputs(in, gpu(FUSE_NV_FEATURE_DLSS_SR), FUSE_NV_FEATURE_DLSS_SR);
    // Row-vector check: prev_clip = cur_clip * clip_to_prev_clip (row-major, Streamline convention).
    const math::Vec3 world(0.7f, 1.3f, -4.f);
    auto clip = [](const fuse::renderer::UpscaleCamera& c, const math::Vec3& p, float (&out)[4]) {
        const math::Mat4 vp = c.projection * c.view;
        for (int r = 0; r < 4; ++r) {
            out[r] = vp.at(u32(r), 0) * p.x + vp.at(u32(r), 1) * p.y + vp.at(u32(r), 2) * p.z + vp.at(u32(r), 3);
        }
    };
    float cur[4], prev[4], mapped[4];
    clip(in.camera, world, cur);
    clip(in.previous_camera, world, prev);
    for (int j = 0; j < 4; ++j) {
        mapped[j] = 0.f;
        for (int i = 0; i < 4; ++i) {
            mapped[j] += cur[i] * f.constants.clip_to_prev_clip[i * 4 + j];
        }
    }
    bool close = true;
    for (int j = 0; j < 4; ++j) {
        close = close && std::fabs(mapped[j] - prev[j]) < 1e-3f * (1.f + std::fabs(prev[j]));
    }
    check(close, "clip_to_prev_clip reprojects a world point to the previous camera's clip space");
    check(f.constants.camera_fwd[2] < -0.99f && std::fabs(f.constants.camera_right[0] - 1.f) < 1e-4f, "camera basis (looks down -Z)");
    check(f.constants.camera_view_to_clip[11] == -1.f, "projection copied as row-vector matrix (w = -z term at [11])");
    check(f.constants.exposure_scale == 0.5f && f.constants.render_width == 960u, "exposure + render width");
    const nv::NvFrameInputs same = nv::map_upscale_inputs(inputs(5, false), gpu(FUSE_NV_FEATURE_DLSS_SR), FUSE_NV_FEATURE_DLSS_SR);
    bool ident = true;
    for (int i = 0; i < 16; ++i) {
        ident = ident && std::fabs(same.constants.clip_to_prev_clip[i] - ((i % 5 == 0) ? 1.f : 0.f)) < 1e-4f;
    }
    check(ident, "static camera -> clip_to_prev_clip identity");
    check(nv::validate_inputs(FUSE_NV_FEATURE_DLSS_SR, f).ok(), "mapped frame passes host validation");
}

void test_frame_generation_and_look() {
    std::shared_ptr<nv::NvPlugin> p40 = load("gpu=rtx40");
    nv::NvFrameGenerator fg(p40);
    check(fg.available() && fg.max_generated_frames() == 1u, "FG on RTX 40: 2x only");
    check(fg.present(inputs(200), gpu(FUSE_NV_FEATURE_DLSS_FG), 1) == FUSE_NV_OK && echo(*p40).last_feature == FUSE_NV_FEATURE_DLSS_FG,
          "dlss_fg present through the mock");
    check(fg.present(inputs(201), gpu(FUSE_NV_FEATURE_DLSS_FG), 3) == FUSE_NV_ERR_INVALID_ARGUMENT, "MFG 4x refused on RTX 40");
    nv::NvGpuFrame noHud = gpu(FUSE_NV_FEATURE_DLSS_FG);
    noHud.resources[FUSE_NV_BUFFER_HUDLESS_COLOR] = FuseNvResource{};
    check(fg.present(inputs(202), noHud, 1) == FUSE_NV_ERR_MISSING_INPUT &&
              fg.last_validation().status() == nv::InputStatus::MissingHudlessColor,
          "FG without HUD-less colour -> MissingHudlessColor");
    check(!nv::NvNeuralLookPass(p40).available(), "dlss5_look unavailable on RTX 40");

    std::shared_ptr<nv::NvPlugin> p50 = load("gpu=rtx50");
    nv::NvNeuralLookPass look(p50);
    check(look.available(), "dlss5_look available on RTX 50");
    nv::NvGpuFrame g = gpu(FUSE_NV_FEATURE_DLSS_NR);
    FuseNvResource mask{};
    mask.native = 0x7777u;
    g.set(FUSE_NV_BUFFER_NEURAL_CONTROL_MASK, mask);
    check(look.evaluate(inputs(300), g, 0.6f, 2.f) == FUSE_NV_OK, "dlss5_look evaluate");
    const FuseNvMockEcho e = echo(*p50);
    check(e.last_feature == FUSE_NV_FEATURE_DLSS_NR && e.last_render_width == 1920u && e.last_output_width == 1920u,
          "NR runs 1-in-1-out at display resolution");
    check(e.last_nr_structure_intensity == 0.6f && e.last_nr_tone_intensity == 1.f && e.last_jitter_px[0] == 0.f,
          "intensities forwarded (clamped to [0,1]); no jitter");
    check(nv::NvFrameGenerator(p50).max_generated_frames() == 5u &&
              nv::NvFrameGenerator(p50).present(inputs(301), gpu(FUSE_NV_FEATURE_DLSS_FG), 5) == FUSE_NV_OK,
          "6x MFG on RTX 50");

    const lk::LookEffectGraph graph = lk::LookEffectGraph::makeDefault();
    const nv::NvLookPlacement at = nv::plan_dlss5_look_placement(graph);
    check(at.ok, std::string("dlss5_look placement: ") + at.reason);
    if (at.ok) {
        for (u32 i = 0; i < at.insert_index; ++i) {
            check(lk::look_effect_info(graph.at(i)).stage == lk::LookStage::PreUpscale, "only pre-upscale nodes before dlss5_look");
        }
        check(at.insert_index < graph.size() && lk::look_effect_info(graph.at(at.insert_index)).input == lk::LookDomain::SceneHdr &&
                  at.insert_index < graph.indexOf(lk::LookEffect::ToneMap),
              "dlss5_look precedes tone mapping and feeds a scene-HDR node");
    }
    lk::LookEffectGraph broken;
    broken.push(lk::LookEffect::Bloom);
    check(!nv::plan_dlss5_look_placement(broken).ok, "invalid look graph -> no placement");
}

void test_probe_default_off() {
    const nv::LoadResult r = nv::probe_and_register_nvidia_backends({});
    if (!nv::NvPluginLoader::enabled_at_build()) {
        check(r.status == nv::PluginStatus::DisabledAtBuild && !up::UpscalerRegistry::instance().find(nv::kDlssSrName),
              "default build: probe disabled, process registry has no DLSS backend");
    }
}

} // namespace

int main() {
    test_registration_follows_detection();
    test_upscaler_through_registry();
    test_camera_mapping();
    test_frame_generation_and_look();
    test_probe_default_off();
    std::printf("fuse_nvidia_backend_gates: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
