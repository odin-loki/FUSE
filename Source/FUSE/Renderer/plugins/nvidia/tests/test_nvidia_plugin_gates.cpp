// Gates for the optional NVIDIA plugin seam (docs/nvidia-plugin.md "What CI verifies").
// No NVIDIA hardware, SDK or binary: everything runs against the in-tree MIT mocks.

#include "fuse_nv_mock_echo.h"
#include "fuse_sl_mock_echo.h"
#include "nv_streamline_mapping.hpp"

#include <fuse/renderer/nvidia/nv_input_mapping.hpp>
#include <fuse/renderer/nvidia/nv_plugin_loader.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nv = fuse::renderer::nvidia;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void set_env(const char* k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

void unset_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

void clear_env() {
    unset_env("FUSE_NVIDIA_SDK_DIR");
    unset_env("FUSE_NVIDIA_PLUGIN_LIB");
    unset_env("FUSE_NVIDIA_PLUGIN_OPTIONS");
}

std::string status_text(const nv::LoadResult& r) {
    return std::string(nv::plugin_status_name(r.status)) + " / " + r.detail;
}

nv::LoadResult load_mock(const std::string& options, const char* file = FUSE_NV_TEST_MOCK) {
    nv::PluginConfig c;
    c.options = options;
    return nv::NvPluginLoader::load_file(file, c);
}

FuseNvMockEcho mock_echo(const nv::NvPlugin& p) {
    FuseNvMockEcho e{};
    auto fn = reinterpret_cast<FuseNvMockGetEchoFn>(p.symbol(FUSE_NV_MOCK_ECHO_NAME));
    if (fn) {
        fn(&e);
    }
    return e;
}

FuseNvResource res(uint64_t handle, uint32_t w, uint32_t h) {
    FuseNvResource r{};
    r.native = handle;
    r.width = w;
    r.height = h;
    r.native_format = 97; // VK_FORMAT_R16G16B16A16_SFLOAT
    return r;
}

void identity(float (&m)[16]) {
    for (int i = 0; i < 16; ++i) {
        m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
}

/// A complete, valid frame for `feature` at 960x540 -> 1920x1080.
nv::NvFrameInputs full_frame(FuseNvFeature feature, uint32_t frame = 7) {
    nv::NvFrameInputs in;
    const uint32_t rw = 960, rh = 540, ow = 1920, oh = 1080;
    FuseNvConstants& c = in.constants;
    c.frame_index = frame;
    c.render_width = rw;
    c.render_height = rh;
    c.output_width = feature == FUSE_NV_FEATURE_DLSS_NR ? rw : ow;
    c.output_height = feature == FUSE_NV_FEATURE_DLSS_NR ? rh : oh;
    identity(c.camera_view_to_clip);
    c.camera_view_to_clip[0] = 1.25f;
    identity(c.clip_to_camera_view);
    identity(c.clip_to_prev_clip);
    identity(c.prev_clip_to_clip);
    identity(c.world_to_camera_view);
    identity(c.camera_view_to_world);
    c.jitter_offset_px[0] = 0.25f;
    c.jitter_offset_px[1] = -0.125f;
    c.mvec_scale[0] = -1.0f;
    c.mvec_scale[1] = -1.0f;
    c.camera_near = 0.1f;
    c.camera_far = 1000.0f;
    c.camera_vfov_rad = 1.0f;
    c.camera_aspect = 16.0f / 9.0f;
    c.flags = FUSE_NV_CONST_HDR | FUSE_NV_CONST_CAMERA_MOTION_INCLUDED | FUSE_NV_CONST_DEPTH_INVERTED;
    const uint32_t required = nv::required_buffer_mask(feature);
    for (uint32_t k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
        if (!(required & (1u << k))) {
            continue;
        }
        const bool display = k == FUSE_NV_BUFFER_COLOR_OUT || k == FUSE_NV_BUFFER_HUDLESS_COLOR;
        const bool nr = feature == FUSE_NV_FEATURE_DLSS_NR;
        in.set(k, res(0x1000u + k, display && !nr ? ow : rw, display && !nr ? oh : rh));
    }
    return in;
}

FuseNvFeatureOptions options(FuseNvFeature f, FuseNvQuality q = FUSE_NV_QUALITY_QUALITY) {
    FuseNvFeatureOptions o{};
    o.struct_size = sizeof(o);
    o.feature = f;
    o.quality = q;
    o.frames_to_generate = 1;
    o.nr_structure_intensity = 0.5f;
    o.nr_tone_intensity = 0.25f;
    return o;
}

FuseNvStatus submit(const nv::NvPlugin& p, const nv::NvFrameInputs& in, FuseNvFeature f, uint32_t viewport = 0,
                    FuseNvQuality q = FUSE_NV_QUALITY_QUALITY, uint32_t frames_to_generate = 1) {
    const auto tags = in.packed();
    FuseNvStatus s = p.api().set_tags(p.context(), in.constants.frame_index, viewport, tags.data(), uint32_t(tags.size()), 0);
    if (s != FUSE_NV_OK) {
        return s;
    }
    s = p.api().set_constants(p.context(), viewport, &in.constants);
    if (s != FUSE_NV_OK) {
        return s;
    }
    FuseNvFeatureOptions o = options(f, q);
    o.frames_to_generate = frames_to_generate;
    return p.api().evaluate(p.context(), in.constants.frame_index, viewport, &o, 0);
}

// ---- 1. configuration / build gating ------------------------------------------------------------

void test_configuration() {
    clear_env();
    const nv::LoadResult none = nv::NvPluginLoader::load(nv::resolve_plugin_config({}));
    check(none.status == nv::PluginStatus::NotConfigured && !none.plugin, "no SDK dir configured -> NotConfigured, no plugin");

    const nv::LoadResult probe = nv::NvPluginLoader::probe({});
    if (nv::NvPluginLoader::enabled_at_build()) {
        check(probe.status == nv::PluginStatus::NotConfigured, "probe (plugin ON, env unset) -> NotConfigured");
    } else {
        check(probe.status == nv::PluginStatus::DisabledAtBuild && !probe.plugin,
              "probe with FUSE_ENABLE_NVIDIA_PLUGIN=OFF -> DisabledAtBuild without touching the file system");
    }
    const std::string def(nv::default_provider_library_name());
#if defined(_WIN32)
    check(def == "fuse_nvplugin_streamline.dll", "Windows default provider is the Streamline provider");
#else
    check(def.find("fuse_nvplugin_ngx") != std::string::npos, "Linux default provider is the NGX bridge");
#endif
}

// ---- 2. loader finds the mock through the environment ----------------------------------------------

void test_env_discovery() {
    const fs::path mock(FUSE_NV_TEST_MOCK);
    clear_env();
    set_env("FUSE_NVIDIA_SDK_DIR", mock.parent_path().string());
    set_env("FUSE_NVIDIA_PLUGIN_LIB", mock.filename().string());
    set_env("FUSE_NVIDIA_PLUGIN_OPTIONS", "gpu=rtx50");
    const nv::PluginConfig cfg = nv::resolve_plugin_config({});
    check(cfg.sdk_dir == mock.parent_path().string(), "FUSE_NVIDIA_SDK_DIR read into config");
    const nv::LoadResult r = nv::NvPluginLoader::load(cfg);
    check(r.status == nv::PluginStatus::Available && r.plugin, "loader finds mock via FUSE_NVIDIA_SDK_DIR: " + status_text(r));
    if (r.plugin) {
        check(r.plugin->provider_name() == "mock", "provider name reported");
        check(r.plugin->feature_mask() == 0x1Fu, "rtx50 mock: SR, RR, FG, NR, Reflex all detected");
        check(r.plugin->adapter().vendor_id == 0x10DEu && r.plugin->adapter().rtx_generation == 50u, "adapter info");
        check(mock_echo(*r.plugin).init_count == 1u, "mock init called once");
    }

    // A project setting wins over the environment, field by field.
    nv::PluginConfig project;
    project.sdk_dir = (fs::path(FUSE_NV_TEST_SCRATCH) / "no_such_dir").string();
    const nv::PluginConfig merged = nv::resolve_plugin_config(project);
    check(merged.sdk_dir == project.sdk_dir && merged.library_name == mock.filename().string(),
          "project sdk_dir overrides env, unset library_name still comes from env");
    const nv::LoadResult missing = nv::NvPluginLoader::load(merged);
    check(missing.status == nv::PluginStatus::LibraryNotFound && !missing.plugin, "missing library -> LibraryNotFound: " + status_text(missing));
    clear_env();
}

// ---- 3. failure paths: clean "unavailable", never a crash ----------------------------------------

void test_failure_paths() {
    const fs::path scratch(FUSE_NV_TEST_SCRATCH);
    std::error_code ec;
    fs::create_directories(scratch, ec);

    nv::PluginConfig c;
    c.sdk_dir = scratch.string();
    c.library_name = "does_not_exist.so";
    check(nv::NvPluginLoader::load(c).status == nv::PluginStatus::LibraryNotFound, "empty SDK dir -> LibraryNotFound");

    const fs::path garbage = scratch / "libgarbage_provider.so";
    {
        std::ofstream f(garbage, std::ios::binary);
        f << "this is not a shared library";
    }
    const nv::LoadResult g = nv::NvPluginLoader::load_file(garbage.string(), {});
    check(g.status == nv::PluginStatus::LoadFailed && !g.plugin, "corrupt library -> LoadFailed: " + status_text(g));

    const nv::LoadResult noentry = load_mock("", FUSE_NV_TEST_MOCK_NOENTRY);
    check(noentry.status == nv::PluginStatus::EntryPointMissing, "no fuseNvPluginGetApi export -> EntryPointMissing: " + status_text(noentry));

    const nv::LoadResult badabi = load_mock("", FUSE_NV_TEST_MOCK_BADABI);
    check(badabi.status == nv::PluginStatus::AbiMismatch && !badabi.plugin,
          "provider reporting ABI 2.0 (and lying with FUSE_NV_OK) rejected -> AbiMismatch: " + status_text(badabi));

    const nv::LoadResult trunc = load_mock("", FUSE_NV_TEST_MOCK_TRUNCATED);
    check(trunc.status == nv::PluginStatus::AbiMismatch, "truncated function table -> AbiMismatch: " + status_text(trunc));

    // Keep one healthy instance loaded so the mock's static counters survive the failing loads.
    const nv::LoadResult keep = load_mock("gpu=rtx40");
    check(keep.status == nv::PluginStatus::Available, "healthy mock loads");
    const auto live = keep.plugin ? reinterpret_cast<FuseNvMockLiveContextsFn>(keep.plugin->symbol(FUSE_NV_MOCK_LIVE_NAME)) : nullptr;
    check(live && live() == 1u, "one live provider context");

    struct Case {
        const char* options;
        nv::PluginStatus expect;
    } cases[] = {
        {"gpu=none", nv::PluginStatus::NoNvidiaGpu},
        {"runtime=missing", nv::PluginStatus::RuntimeMissing},
        {"driver=470.10", nv::PluginStatus::DriverTooOld},
        {"disable=sr,rr,fg,nr,reflex", nv::PluginStatus::NoFeatures},
        {"driver=garbage", nv::PluginStatus::InitFailed},
    };
    for (const Case& k : cases) {
        const nv::LoadResult r = load_mock(k.options);
        check(r.status == k.expect && !r.plugin,
              std::string("mock options '") + k.options + "' -> " + std::string(nv::plugin_status_name(k.expect)) + ", got " + status_text(r));
    }
    check(live && live() == 1u, "failed inits leak no provider context (NoFeatures path shuts down)");

    // Streamline provider with no interposer next to it -> RuntimeMissing (not a crash).
    nv::PluginConfig sl;
    sl.options = "interposer=sl.interposer.missing";
    const nv::LoadResult slr = nv::NvPluginLoader::load_file(FUSE_NV_TEST_SL_PROVIDER, sl);
    check(slr.status == nv::PluginStatus::RuntimeMissing, "Streamline provider without interposer -> RuntimeMissing: " + status_text(slr));
}

// ---- 4. capability-flagged detection ---------------------------------------------------------------

void test_capabilities() {
    struct Case {
        const char* options;
        uint32_t mask;
    } cases[] = {
        {"gpu=rtx50", 0x1Fu},
        {"gpu=rtx40", FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_SR) | FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_RR) |
                          FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_FG) | FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_REFLEX)},
        {"gpu=rtx20", FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_SR) | FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_RR) |
                          FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_REFLEX)},
        {"gpu=rtx50;disable=reflex", FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_SR) | FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_RR) |
                                         FUSE_NV_FEATURE_BIT(FUSE_NV_FEATURE_DLSS_NR)},
    };
    for (const Case& k : cases) {
        const nv::LoadResult r = load_mock(k.options);
        check(r.plugin && r.plugin->feature_mask() == k.mask,
              std::string("feature mask for '") + k.options + "' = " + std::to_string(r.plugin ? r.plugin->feature_mask() : 0u));
        if (r.plugin) {
            check(r.plugin->feature(FUSE_NV_FEATURE_DLSS_NR).min_rtx_generation == 50u, "NR flagged RTX 50");
            check(r.plugin->feature(FUSE_NV_FEATURE_DLSS_FG).min_rtx_generation == 40u, "FG flagged RTX 40");
            check(!r.plugin->supports(99u), "unknown feature id never supported");
        }
    }
    const nv::LoadResult r = load_mock("gpu=rtx40");
    if (r.plugin) {
        FuseNvRenderSize rs{};
        rs.struct_size = sizeof(rs);
        check(r.plugin->api().get_render_size(r.plugin->context(), FUSE_NV_FEATURE_DLSS_SR, FUSE_NV_QUALITY_PERFORMANCE, 3840,
                                              2160, &rs) == FUSE_NV_OK &&
                  rs.render_width == 1920u && rs.render_height == 1080u,
              "SR Performance at 4K renders 1920x1080");
        check(r.plugin->api().get_render_size(r.plugin->context(), FUSE_NV_FEATURE_DLSS_FG, FUSE_NV_QUALITY_QUALITY, 1920, 1080,
                                              &rs) == FUSE_NV_ERR_UNSUPPORTED_FEATURE,
              "render size only for SR/RR");
    }
}

// ---- 5. host-side validation statuses --------------------------------------------------------------

void expect_status(FuseNvFeature f, const nv::NvFrameInputs& in, nv::InputStatus s, const std::string& what) {
    const nv::ValidationReport r = nv::validate_inputs(f, in);
    check(r.status() == s, what + ": expected " + std::string(nv::input_status_name(s)) + ", got " +
                               std::string(nv::input_status_name(r.status())));
}

void test_validation() {
    for (FuseNvFeature f : {FUSE_NV_FEATURE_DLSS_SR, FUSE_NV_FEATURE_DLSS_RR, FUSE_NV_FEATURE_DLSS_FG, FUSE_NV_FEATURE_DLSS_NR}) {
        const nv::ValidationReport ok = nv::validate_inputs(f, full_frame(f));
        check(ok.ok(), "complete frame validates for feature " + std::to_string(f) +
                           (ok.ok() ? "" : ": " + std::string(nv::input_status_name(ok.status()))));
        // Dropping any single required buffer gives that buffer's specific status.
        const uint32_t req = nv::required_buffer_mask(f);
        for (uint32_t k = 0; k < FUSE_NV_BUFFER_KIND_COUNT; ++k) {
            if (!(req & (1u << k))) {
                continue;
            }
            nv::NvFrameInputs in = full_frame(f);
            in.clear(k);
            const nv::ValidationReport r = nv::validate_inputs(f, in);
            check(!r.ok() && r.errors.front().kind == k && nv::to_plugin_status(r.status()) == FUSE_NV_ERR_MISSING_INPUT,
                  "feature " + std::to_string(f) + " without " + std::string(nv::buffer_kind_name(k)) + " -> " +
                      std::string(nv::input_status_name(r.status())));
        }
    }
    {
        nv::NvFrameInputs in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
        in.clear(FUSE_NV_BUFFER_DEPTH);
        expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::MissingDepth, "SR without depth");
    }
    {
        nv::NvFrameInputs in = full_frame(FUSE_NV_FEATURE_DLSS_RR);
        in.clear(FUSE_NV_BUFFER_NORMALS);
        expect_status(FUSE_NV_FEATURE_DLSS_RR, in, nv::InputStatus::MissingNormals, "RR without normals");
        in = full_frame(FUSE_NV_FEATURE_DLSS_RR);
        in.constants.flags &= ~FUSE_NV_CONST_HDR;
        expect_status(FUSE_NV_FEATURE_DLSS_RR, in, nv::InputStatus::HdrRequired, "RR with LDR colour");
        in = full_frame(FUSE_NV_FEATURE_DLSS_RR);
        std::memset(in.constants.camera_view_to_clip, 0, sizeof(in.constants.camera_view_to_clip));
        expect_status(FUSE_NV_FEATURE_DLSS_RR, in, nv::InputStatus::MissingProjection, "RR without projection");
    }
    {
        nv::NvFrameInputs in = full_frame(FUSE_NV_FEATURE_DLSS_FG);
        in.clear(FUSE_NV_BUFFER_HUDLESS_COLOR);
        expect_status(FUSE_NV_FEATURE_DLSS_FG, in, nv::InputStatus::MissingHudlessColor, "FG without HUD-less colour");
        const nv::ValidationReport r = nv::validate_inputs(FUSE_NV_FEATURE_DLSS_FG, full_frame(FUSE_NV_FEATURE_DLSS_FG));
        check(r.ok() && r.has(nv::InputStatus::WarnMissingUiColorAlpha), "FG without UI alpha is a warning, not an error");
    }
    {
        nv::NvFrameInputs in = full_frame(FUSE_NV_FEATURE_DLSS_NR);
        in.constants.output_width = 1920;
        expect_status(FUSE_NV_FEATURE_DLSS_NR, in, nv::InputStatus::RenderOutputSizeMismatch, "NR is 1-in-1-out");
    }
    nv::NvFrameInputs in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.constants.jitter_offset_px[0] = 0.75f;
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::JitterOutOfRange, "jitter beyond half a pixel");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.constants.mvec_scale[1] = 0.0f;
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::InvalidMotionVectorScale, "zero MV scale");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.constants.clip_to_prev_clip[5] = std::numeric_limits<float>::quiet_NaN();
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::NonFiniteConstants, "NaN matrix");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.tags[FUSE_NV_BUFFER_MOTION_VECTORS].resource.width = 1920;
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::ResourceExtentMismatch, "display-sized MVs at render res");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.constants.output_width = 800;
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::OutputSmallerThanRender, "downscale request");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.constants.render_width = 0;
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::ZeroRenderSize, "zero render size");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.constants.exposure_scale = 0.0f;
    expect_status(FUSE_NV_FEATURE_DLSS_SR, in, nv::InputStatus::InvalidExposure, "no exposure texture and zero exposure");
    in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    in.set(FUSE_NV_BUFFER_EXPOSURE, res(0x99, 1, 1));
    const nv::ValidationReport withExposure = nv::validate_inputs(FUSE_NV_FEATURE_DLSS_SR, in);
    check(withExposure.ok() && withExposure.warnings.empty(), "exposure texture silences the exposure warning");
    check(nv::validate_inputs(FUSE_NV_FEATURE_REFLEX, in).status() == nv::InputStatus::UnknownFeature, "Reflex has no frame inputs");
}

// ---- 6. round trip through the mock provider -------------------------------------------------------

void test_mock_round_trip() {
    const nv::LoadResult r = load_mock("gpu=rtx40");
    if (!r.plugin) {
        check(false, "mock for round trip");
        return;
    }
    const nv::NvPlugin& p = *r.plugin;
    nv::NvFrameInputs sr = full_frame(FUSE_NV_FEATURE_DLSS_SR, 11);
    sr.set(FUSE_NV_BUFFER_REACTIVE_MASK, res(0x2000, 960, 540));
    check(nv::validate_inputs(FUSE_NV_FEATURE_DLSS_SR, sr).ok(), "SR frame valid");
    check(submit(p, sr, FUSE_NV_FEATURE_DLSS_SR, 0, FUSE_NV_QUALITY_BALANCED) == FUSE_NV_OK, "SR evaluate via mock");
    FuseNvMockEcho e = mock_echo(p);
    check(e.evaluate_count == 1u && e.last_feature == FUSE_NV_FEATURE_DLSS_SR && e.last_frame_index == 11u, "echo: SR frame 11");
    check(e.last_tag_mask == sr.present_mask(), "echo: every tag reached the provider");
    check(e.last_color_in_native == 0x1000u + FUSE_NV_BUFFER_COLOR_IN, "echo: COLOR_IN handle not reordered");
    check(e.last_jitter_px[0] == 0.25f && e.last_jitter_px[1] == -0.125f, "echo: jitter");
    check(e.last_mvec_scale[0] == -1.0f && e.last_render_width == 960u && e.last_output_width == 1920u, "echo: mv scale + sizes");
    check(e.last_quality == FUSE_NV_QUALITY_BALANCED && (e.last_flags & FUSE_NV_CONST_DEPTH_INVERTED), "echo: quality + flags");

    // Provider-side checks (host validation bypassed on purpose).
    nv::NvFrameInputs broken = full_frame(FUSE_NV_FEATURE_DLSS_SR, 12);
    broken.clear(FUSE_NV_BUFFER_DEPTH);
    check(submit(p, broken, FUSE_NV_FEATURE_DLSS_SR) == FUSE_NV_ERR_MISSING_INPUT, "mock rejects missing depth");
    check(mock_echo(p).last_missing == FUSE_NV_BUFFER_DEPTH, "mock names the missing buffer");
    FuseNvFeatureOptions o = options(FUSE_NV_FEATURE_DLSS_SR);
    check(p.api().evaluate(p.context(), 999u, 0, &o, 0) == FUSE_NV_ERR_MISSING_CONSTANTS, "evaluate without constants");
    check(submit(p, full_frame(FUSE_NV_FEATURE_DLSS_NR, 13), FUSE_NV_FEATURE_DLSS_NR) == FUSE_NV_ERR_UNSUPPORTED_FEATURE,
          "NR on RTX 40 -> unsupported");
    check(submit(p, full_frame(FUSE_NV_FEATURE_DLSS_FG, 14), FUSE_NV_FEATURE_DLSS_FG, 0, FUSE_NV_QUALITY_QUALITY, 3) ==
              FUSE_NV_ERR_INVALID_ARGUMENT,
          "multi-frame generation (3x) on RTX 40 rejected");
    check(submit(p, full_frame(FUSE_NV_FEATURE_DLSS_FG, 15), FUSE_NV_FEATURE_DLSS_FG) == FUSE_NV_OK, "2x FG on RTX 40");
    check(submit(p, full_frame(FUSE_NV_FEATURE_DLSS_RR, 16), FUSE_NV_FEATURE_DLSS_RR, 1) == FUSE_NV_OK, "RR on viewport 1");
    check(mock_echo(p).last_viewport == 1u, "viewport forwarded");
    FuseNvResourceTag bad{};
    bad.kind = FUSE_NV_BUFFER_KIND_COUNT;
    bad.resource.native = 1;
    check(p.api().set_tags(p.context(), 20, 0, &bad, 1, 0) == FUSE_NV_ERR_INVALID_ARGUMENT, "unknown buffer kind rejected");
    check(mock_echo(p).pending_frames <= 8u, "provider keeps a bounded frame window");

    const nv::LoadResult r50 = load_mock("gpu=rtx50");
    if (r50.plugin) {
        check(submit(*r50.plugin, full_frame(FUSE_NV_FEATURE_DLSS_FG, 30), FUSE_NV_FEATURE_DLSS_FG, 0, FUSE_NV_QUALITY_QUALITY, 5) ==
                  FUSE_NV_OK,
              "6x dynamic multi-frame generation on RTX 50");
        check(submit(*r50.plugin, full_frame(FUSE_NV_FEATURE_DLSS_NR, 31), FUSE_NV_FEATURE_DLSS_NR) == FUSE_NV_OK, "NR on RTX 50");
        const FuseNvMockEcho e50 = mock_echo(*r50.plugin);
        check(e50.last_nr_structure_intensity == 0.5f && e50.last_nr_tone_intensity == 0.25f, "NR intensities forwarded");
    }
}

// ---- 7. Streamline mapping (pinned MIT headers) ------------------------------------------------------

void test_streamline_mapping() {
    namespace sm = fuse::renderer::nvidia::streamline;
    sl::BufferType t{};
    struct Row {
        FuseNvBufferKind kind;
        FuseNvFeature feature;
        sl::BufferType expect;
    } rows[] = {
        {FUSE_NV_BUFFER_COLOR_IN, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeScalingInputColor},
        {FUSE_NV_BUFFER_COLOR_OUT, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeScalingOutputColor},
        {FUSE_NV_BUFFER_COLOR_IN, FUSE_NV_FEATURE_DLSS_NR, sl::kBufferTypeUpliftInputColor},
        {FUSE_NV_BUFFER_COLOR_OUT, FUSE_NV_FEATURE_DLSS_NR, sl::kBufferTypeUpliftOutputColor},
        {FUSE_NV_BUFFER_NEURAL_CONTROL_MASK, FUSE_NV_FEATURE_DLSS_NR, sl::kBufferTypeUpliftControlMask},
        {FUSE_NV_BUFFER_DEPTH, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeDepth},
        {FUSE_NV_BUFFER_MOTION_VECTORS, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeMotionVectors},
        {FUSE_NV_BUFFER_EXPOSURE, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeExposure},
        {FUSE_NV_BUFFER_REACTIVE_MASK, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeBiasCurrentColorHint},
        {FUSE_NV_BUFFER_TRANSPARENCY_MASK, FUSE_NV_FEATURE_DLSS_SR, sl::kBufferTypeTransparencyHint},
        {FUSE_NV_BUFFER_HUDLESS_COLOR, FUSE_NV_FEATURE_DLSS_FG, sl::kBufferTypeHUDLessColor},
        {FUSE_NV_BUFFER_UI_COLOR_ALPHA, FUSE_NV_FEATURE_DLSS_FG, sl::kBufferTypeUIColorAndAlpha},
        {FUSE_NV_BUFFER_DIFFUSE_ALBEDO, FUSE_NV_FEATURE_DLSS_RR, sl::kBufferTypeAlbedo},
        {FUSE_NV_BUFFER_SPECULAR_ALBEDO, FUSE_NV_FEATURE_DLSS_RR, sl::kBufferTypeSpecularAlbedo},
        {FUSE_NV_BUFFER_NORMALS, FUSE_NV_FEATURE_DLSS_RR, sl::kBufferTypeNormals},
        {FUSE_NV_BUFFER_ROUGHNESS, FUSE_NV_FEATURE_DLSS_RR, sl::kBufferTypeRoughness},
        {FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE, FUSE_NV_FEATURE_DLSS_RR, sl::kBufferTypeSpecularHitDistance},
    };
    for (const Row& row : rows) {
        check(sm::to_sl_buffer_type(row.kind, row.feature, t) && t == row.expect,
              "sl buffer type for " + std::string(nv::buffer_kind_name(row.kind)) + " / feature " + std::to_string(row.feature));
    }
    check(!sm::to_sl_buffer_type(FUSE_NV_BUFFER_NEURAL_CONTROL_MASK, FUSE_NV_FEATURE_DLSS_SR, t), "control mask only for NR");
    check(!sm::to_sl_buffer_type(FUSE_NV_BUFFER_KIND_COUNT, FUSE_NV_FEATURE_DLSS_SR, t), "unknown kind unmapped");
    sl::Feature f{};
    check(sm::to_sl_feature(FUSE_NV_FEATURE_DLSS_NR, f) && f == sl::kFeatureDLSS_NR, "NR -> kFeatureDLSS_NR (1004)");
    check(sm::to_sl_feature(FUSE_NV_FEATURE_DLSS_RR, f) && f == sl::kFeatureDLSS_RR, "RR -> kFeatureDLSS_RR");

    const nv::NvFrameInputs in = full_frame(FUSE_NV_FEATURE_DLSS_SR);
    sl::Constants c{};
    sm::to_sl_constants(in.constants, c);
    check(c.depthInverted == sl::Boolean::eTrue && c.cameraMotionIncluded == sl::Boolean::eTrue && c.reset == sl::Boolean::eFalse &&
              c.motionVectors3D == sl::Boolean::eFalse && c.motionVectorsJittered == sl::Boolean::eFalse,
          "flags become explicit sl::Boolean (never eInvalid)");
    check(c.cameraViewToClip.row[0].x == 1.25f && c.clipToLensClip.row[2].z == 1.0f && c.clipToLensClip.row[0].y == 0.0f,
          "row-major matrices copied, clipToLensClip identity");
    check(c.jitterOffset.x == 0.25f && c.mvecScale.y == -1.0f && c.cameraFar == 1000.0f, "jitter / mvec / planes");
    check(sm::to_sl_dlss_mode(FUSE_NV_QUALITY_QUALITY) == sl::DLSSMode::eMaxQuality &&
              sm::to_sl_dlss_mode(FUSE_NV_QUALITY_DLAA) == sl::DLSSMode::eDLAA &&
              sm::to_sl_dlss_mode(FUSE_NV_QUALITY_PERFORMANCE) == sl::DLSSMode::eMaxPerformance,
          "quality -> DLSSMode");
    check(sm::from_sl_result(sl::Result::eErrorMissingInputParameter) == FUSE_NV_ERR_MISSING_INPUT &&
              sm::from_sl_result(sl::Result::eErrorDriverOutOfDate) == FUSE_NV_ERR_DRIVER_TOO_OLD &&
              sm::from_sl_result(sl::Result::eErrorNoSupportedAdapterFound) == FUSE_NV_ERR_NO_NVIDIA_GPU &&
              sm::from_sl_result(sl::Result::eOk) == FUSE_NV_OK,
          "sl::Result -> provider status");
    const sl::Resource r = sm::to_sl_resource(res(0xABCD, 64, 32));
    check(reinterpret_cast<uintptr_t>(r.native) == 0xABCDu && r.width == 64u && r.height == 32u && r.nativeFormat == 97u,
          "resource handle / size / format");
}

// ---- 8. Streamline provider against the mock interposer -----------------------------------------------

FuseSlMockEcho sl_echo(const fs::path& interposer) {
    FuseSlMockEcho e{};
    // The provider already has it loaded; this lookup only bumps the refcount.
#if defined(_WIN32)
    HMODULE h = GetModuleHandleW(interposer.filename().wstring().c_str());
    auto fn = h ? reinterpret_cast<FuseSlMockGetEchoFn>(reinterpret_cast<void*>(GetProcAddress(h, FUSE_SL_MOCK_ECHO_NAME))) : nullptr;
    if (fn) {
        fn(&e);
    }
#else
    void* h = dlopen(interposer.string().c_str(), RTLD_NOW | RTLD_NOLOAD);
    auto fn = h ? reinterpret_cast<FuseSlMockGetEchoFn>(dlsym(h, FUSE_SL_MOCK_ECHO_NAME)) : nullptr;
    if (fn) {
        fn(&e);
    }
    if (h) {
        dlclose(h);
    }
#endif
    return e;
}

bool has_type(const uint32_t* types, uint32_t n, sl::BufferType t) {
    for (uint32_t i = 0; i < n; ++i) {
        if (types[i] == t) {
            return true;
        }
    }
    return false;
}

void test_streamline_provider() {
    const fs::path provider(FUSE_NV_TEST_SL_PROVIDER);
    const fs::path interposer = provider.parent_path() / FUSE_NV_TEST_SL_INTERPOSER;
    nv::PluginConfig c;
    c.options = std::string("interposer=") + FUSE_NV_TEST_SL_INTERPOSER;
    c.application_id = 1234;

    set_env("FUSE_SL_MOCK_GPU", "none");
    const nv::LoadResult none = nv::NvPluginLoader::load_file(provider.string(), c);
    check(none.status == nv::PluginStatus::NoFeatures && !none.plugin, "Streamline: no adapter -> NoFeatures: " + status_text(none));

    set_env("FUSE_SL_MOCK_GPU", "rtx40");
    {
        const nv::LoadResult r40 = nv::NvPluginLoader::load_file(provider.string(), c);
        check(r40.plugin && !r40.plugin->supports(FUSE_NV_FEATURE_DLSS_NR) && r40.plugin->supports(FUSE_NV_FEATURE_DLSS_FG),
              "Streamline rtx40: FG yes, NR (DLSS 5) no");
    }

    set_env("FUSE_SL_MOCK_GPU", "rtx50");
    const nv::LoadResult r = nv::NvPluginLoader::load_file(provider.string(), c);
    check(r.status == nv::PluginStatus::Available && r.plugin, "Streamline provider over mock interposer: " + status_text(r));
    if (!r.plugin) {
        return;
    }
    const nv::NvPlugin& p = *r.plugin;
    check(p.provider_name() == "streamline" && p.feature_mask() == 0x1Fu, "Streamline rtx50: all features");
    FuseSlMockEcho e = sl_echo(interposer);
    check(e.init_count == 1u && e.sdk_version == sl::kSDKVersion, "slInit with the pinned kSDKVersion");
    check(e.application_id == 1234u && e.num_plugin_paths == 1u && e.num_features_to_load == 6u, "slInit preferences");
    const uint64_t flags = e.preference_flags;
    check((flags & uint64_t(sl::PreferenceFlags::eUseManualHooking)) && (flags & uint64_t(sl::PreferenceFlags::eUseFrameBasedResourceTagging)) &&
              !(flags & uint64_t(sl::PreferenceFlags::eAllowOTA)),
          "manual hooking + frame-based tagging, OTA off by default");

    FuseNvRenderSize rs{};
    rs.struct_size = sizeof(rs);
    check(p.api().get_render_size(p.context(), FUSE_NV_FEATURE_DLSS_SR, FUSE_NV_QUALITY_PERFORMANCE, 3840, 2160, &rs) == FUSE_NV_OK &&
              rs.render_width == 1920u,
          "render size through slDLSSGetOptimalSettings");

    nv::NvFrameInputs sr = full_frame(FUSE_NV_FEATURE_DLSS_SR, 40);
    check(submit(p, sr, FUSE_NV_FEATURE_DLSS_SR) == FUSE_NV_OK, "Streamline SR evaluate");
    e = sl_echo(interposer);
    check(e.set_tag_calls == 1u && e.last_tag_frame == 40u && e.last_tag_count == 4u, "slSetTagForFrame: 4 SR tags for frame 40");
    check(has_type(e.last_tag_types, e.last_tag_count, sl::kBufferTypeScalingInputColor) &&
              has_type(e.last_tag_types, e.last_tag_count, sl::kBufferTypeScalingOutputColor) &&
              has_type(e.last_tag_types, e.last_tag_count, sl::kBufferTypeDepth) &&
              has_type(e.last_tag_types, e.last_tag_count, sl::kBufferTypeMotionVectors),
          "slSetTagForFrame buffer types");
    check(e.last_tag_native[0] == 0x1000u + FUSE_NV_BUFFER_COLOR_IN && e.last_tag_width[0] == 960u, "native handle + width reach Streamline");
    check(e.set_constants_calls == 1u && e.last_constants_frame == 40u && e.last_jitter[0] == 0.25f && e.last_mvec_scale[0] == -1.0f,
          "slSetConstants: frame / jitter / mvecScale");
    check(e.last_depth_inverted == sl::Boolean::eTrue && e.last_reset == sl::Boolean::eFalse && e.last_clip_to_lens_clip_11 == 1.0f,
          "slSetConstants booleans + lens matrix");
    check(e.dlss_options_calls == 1u && e.last_dlss_mode == uint32_t(sl::DLSSMode::eMaxQuality) && e.last_dlss_output_w == 1920u &&
              e.last_dlss_hdr == sl::Boolean::eTrue && e.last_dlss_auto_exposure == sl::Boolean::eTrue,
          "slDLSSSetOptions: mode / output / HDR / auto-exposure (no exposure tag)");
    check(e.evaluate_calls == 1u && e.last_eval_feature == sl::kFeatureDLSS && e.last_eval_frame == 40u && e.last_eval_input_count == 1u,
          "slEvaluateFeature(kFeatureDLSS) with the viewport");

    nv::NvFrameInputs rr = full_frame(FUSE_NV_FEATURE_DLSS_RR, 41);
    rr.constants.world_to_camera_view[12] = 3.5f;
    check(submit(p, rr, FUSE_NV_FEATURE_DLSS_RR) == FUSE_NV_OK, "Streamline RR evaluate");
    e = sl_echo(interposer);
    check(e.dlssd_options_calls == 1u && e.last_dlssd_world_to_view_30 == 3.5f && e.last_eval_feature == sl::kFeatureDLSS_RR,
          "slDLSSDSetOptions carries worldToCameraView; evaluate RR");
    check(e.last_tag_count == 9u && has_type(e.last_tag_types, e.last_tag_count, sl::kBufferTypeNormals), "RR guides tagged");

    nv::NvFrameInputs nr = full_frame(FUSE_NV_FEATURE_DLSS_NR, 42);
    nr.set(FUSE_NV_BUFFER_NEURAL_CONTROL_MASK, res(0x3000, 960, 540));
    check(submit(p, nr, FUSE_NV_FEATURE_DLSS_NR) == FUSE_NV_OK, "Streamline NR (DLSS 5) evaluate");
    e = sl_echo(interposer);
    check(e.last_eval_feature == sl::kFeatureDLSS_NR && e.last_eval_local_tag_count == 3u &&
              has_type(e.last_eval_local_tag_types, 3, sl::kBufferTypeUpliftInputColor) &&
              has_type(e.last_eval_local_tag_types, 3, sl::kBufferTypeUpliftControlMask),
          "NR evaluated with local uplift tags");

    check(submit(p, full_frame(FUSE_NV_FEATURE_DLSS_FG, 43), FUSE_NV_FEATURE_DLSS_FG, 0, FUSE_NV_QUALITY_QUALITY, 3) == FUSE_NV_OK,
          "Streamline FG options");
    e = sl_echo(interposer);
    check(e.dlssg_options_calls == 1u && e.last_dlssg_frames == 3u && e.last_dlssg_mode == uint32_t(sl::DLSSGMode::eOn),
          "slDLSSGSetOptions: on, 3 generated frames");
    check(e.evaluate_calls == 3u, "FG does not call slEvaluateFeature (present-driven)");

    FuseNvFeatureOptions o = options(FUSE_NV_FEATURE_DLSS_SR);
    check(p.api().evaluate(p.context(), 77u, 0, &o, 0) == FUSE_NV_ERR_MISSING_CONSTANTS, "Streamline: evaluate w/o constants");
    unset_env("FUSE_SL_MOCK_GPU");
}

} // namespace

int main() {
    test_configuration();
    test_env_discovery();
    test_failure_paths();
    test_capabilities();
    test_validation();
    test_mock_round_trip();
    test_streamline_mapping();
    test_streamline_provider();
    std::printf("fuse_nvidia_plugin_gates: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
