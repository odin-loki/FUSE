// WP-4.3 Intel XeSS plugin: CPU gates against the mock libxess (plugins/intel_xess/mock). No Intel code, GPU or SDK;
// hardware behaviour is a manual check (docs/unification/RENDERER-EXECUTION.md WP-4.3).
//
//   discovery    not configured / not found / not a library / missing entry point / unsupported version, each with
//                its reason; probe() compiled out unless FUSE_ENABLE_XESS_PLUGIN; device probe: no device,
//                unsupported device, unsupported driver, warning results, success (contexts balanced)
//   marshalling  quality modes (XeSS ratio == FUSE ratio for every mode), velocity scale (-render size), jitter sign
//                (-jitter_px, debug flip), exposure, reset, input size, view extents, init flags; the mock sees exactly
//                those values through xessVKInit / xessSetVelocityScale / xessSetJitterScale / xessVKExecute;
//                render_size via xessGetInputResolution; deterministic output signature
//   lifecycle    unbound / missing inputs / bad ratio / create, init and execute failures -> UpscaleStatus;
//                history: first frame, render-size change, camera cut, quality and display changes (re-init)
//   registry     "xess" registered only when a context can be created; selection and fallback to other / in-tree
//                backends (select_upscaler_with_fallback); factory-created instance evaluates through the mock
//   zero_alloc   64 steady-state frames (bind + evaluate, jitter cycling): 0 operator new
#include <fuse/renderer/upscale/upscaler.hpp>
#include <fuse/renderer/xess/xess_upscaler.hpp>

#include "fuse_xess_mock_echo.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

// --- allocation counter (operator new) -----------------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse;
using namespace fuse::renderer;
using namespace fuse::renderer::xess;
namespace fs = std::filesystem;
using upscale::QualityMode;
using upscale::UpscaleStatus;

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// Fake native handles (the mock never dereferences them).
void* const kInstance = reinterpret_cast<void*>(std::uintptr_t{0x1001});
void* const kPhysical = reinterpret_cast<void*>(std::uintptr_t{0x1002});
void* const kDevice = reinterpret_cast<void*>(std::uintptr_t{0x1003});
void* const kCmd = reinterpret_cast<void*>(std::uintptr_t{0xC0DE});

XessDeviceBinding device() { return XessDeviceBinding{kInstance, kPhysical, kDevice, 0x77u}; }

struct Mock {
    std::shared_ptr<XessRuntime> rt;
    FuseXessMockEchoFn echo = nullptr;
    FuseXessMockControlFn control = nullptr;
    FuseXessMockResetFn reset = nullptr;

    FuseXessMockEcho get() const {
        FuseXessMockEcho e{};
        echo(&e);
        return e;
    }
    void set(FuseXessResult create, FuseXessResult init, FuseXessResult exec) const {
        const FuseXessMockControl c{create, init, exec};
        control(&c);
    }
};

Mock load_mock() {
    Mock m;
    XessLoadResult r = XessLoader::load_file(FUSE_XESS_TEST_MOCK);
    if (r.status != XessStatus::Available) {
        std::printf("  mock failed to load: %s\n", r.detail.c_str());
        std::exit(1);
    }
    m.rt = std::move(r.runtime);
    m.echo = reinterpret_cast<FuseXessMockEchoFn>(m.rt->symbol(FUSE_XESS_MOCK_ECHO_NAME));
    m.control = reinterpret_cast<FuseXessMockControlFn>(m.rt->symbol(FUSE_XESS_MOCK_CONTROL_NAME));
    m.reset = reinterpret_cast<FuseXessMockResetFn>(m.rt->symbol(FUSE_XESS_MOCK_RESET_NAME));
    if (!m.echo || !m.control || !m.reset) {
        std::printf("  mock test hooks missing\n");
        std::exit(1);
    }
    m.reset();
    return m;
}

FuseXessVkImageViewInfo view(u64 id, u32 w = 0, u32 h = 0) {
    FuseXessVkImageViewInfo v{};
    v.image = id;
    v.image_view = id + 0x100u;
    v.format = 97; // VK_FORMAT_R16G16B16A16_SFLOAT
    v.width = w;
    v.height = h;
    return v;
}

XessGpuFrame gpu_frame() {
    XessGpuFrame g{};
    g.color = view(0xA1);
    g.velocity = view(0xA2);
    g.depth = view(0xA3);
    g.depth.subresource_range.aspect_mask = 2u; // VK_IMAGE_ASPECT_DEPTH_BIT
    g.output = view(0xA4);
    g.command_buffer = kCmd;
    return g;
}

UpscaleInputs inputs(u32 dw, u32 dh, QualityMode mode, u32 frame) {
    UpscaleInputs in{};
    const upscale::Extent2D r = upscale::render_extent({dw, dh}, mode);
    in.resolution = upscale::make_resolution(r, {dw, dh});
    const upscale::HaltonJitterProvider jp;
    in.jitter_px = jp.offset_px(frame, r, {dw, dh});
    in.frame_index = frame;
    in.exposure = 0.75f;
    return in;
}

// ---- discovery ---------------------------------------------------------------------------------------------------

void test_discovery() {
    {
        const XessLoadResult r = XessLoader::load(XessConfig{});
        CHECK(r.status == XessStatus::NotConfigured && !r.runtime);
        CHECK(contains(r.detail, "FUSE_XESS_SDK_DIR"));
    }
    {
        XessConfig c;
        c.sdk_dir = FUSE_XESS_TEST_SCRATCH "/does_not_exist";
        const XessLoadResult r = XessLoader::load(c);
        CHECK(r.status == XessStatus::LibraryNotFound);
        CHECK(contains(r.detail, std::string(default_xess_library_name()).c_str()));
    }
    {
        std::error_code ec;
        fs::create_directories(FUSE_XESS_TEST_SCRATCH, ec);
        const std::string bogus = std::string(FUSE_XESS_TEST_SCRATCH) + "/" + std::string(default_xess_library_name());
        std::ofstream(bogus) << "not a shared library";
        const XessLoadResult r = XessLoader::load_file(bogus);
        CHECK(r.status == XessStatus::LoadFailed);
        CHECK(contains(r.detail, bogus.c_str()));
    }
    {
        const XessLoadResult r = XessLoader::load_file(FUSE_XESS_TEST_MOCK_NOEXECUTE);
        CHECK(r.status == XessStatus::EntryPointMissing && !r.runtime);
        CHECK(contains(r.detail, "xessVKExecute"));
    }
    {
        const XessLoadResult r = XessLoader::load_file(FUSE_XESS_TEST_MOCK_OLDVERSION);
        CHECK(r.status == XessStatus::VersionUnsupported && !r.runtime);
        CHECK(contains(r.detail, "0.9.0"));
    }
    {
        XessConfig c;
        c.sdk_dir = fs::path(FUSE_XESS_TEST_MOCK).parent_path().string();
        c.library_name = fs::path(FUSE_XESS_TEST_MOCK).filename().string();
        const XessLoadResult r = XessLoader::load(c);
        CHECK(r.status == XessStatus::Available && r.runtime);
        if (r.runtime) {
            CHECK(r.runtime->version().major == 1 && r.runtime->version().minor == 3);
            CHECK(r.runtime->fn().vk_build_pipelines && r.runtime->fn().get_input_resolution &&
                  r.runtime->fn().set_jitter_scale && r.runtime->fn().set_exposure_multiplier);
        }
        CHECK(contains(r.detail, "XeSS 1.3.0"));
    }
    {
        const XessLoadResult r = XessLoader::probe(XessConfig{});
        if (XessLoader::enabled_at_build()) {
            CHECK(r.status != XessStatus::DisabledAtBuild);
        } else {
            CHECK(r.status == XessStatus::DisabledAtBuild);
        }
    }
    // Device probe.
    const Mock m = load_mock();
    std::string detail;
    CHECK(m.rt->probe_device(XessDeviceBinding{}, &detail) == XessStatus::ContextFailed);
    CHECK(contains(detail, "no Vulkan device"));
    m.set(FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE, 0, 0);
    CHECK(m.rt->probe_device(device(), &detail) == XessStatus::DeviceUnsupported);
    CHECK(contains(detail, "unsupported device"));
    m.set(FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER, 0, 0);
    CHECK(m.rt->probe_device(device(), &detail) == XessStatus::DriverUnsupported);
    m.set(FUSE_XESS_RESULT_ERROR_DEVICE, 0, 0);
    CHECK(m.rt->probe_device(device(), &detail) == XessStatus::ContextFailed);
    m.set(FUSE_XESS_RESULT_WARNING_OLD_DRIVER, 0, 0);
    CHECK(m.rt->probe_device(device(), &detail) == XessStatus::Available);
    CHECK(contains(detail, "old driver"));
    m.set(0, 0, 0);
    CHECK(m.rt->probe_device(device(), &detail) == XessStatus::Available);
    const FuseXessMockEcho e = m.get();
    CHECK(e.create_calls == 5u); // the no-device probe never reaches the runtime
    CHECK(e.destroy_calls == 2u && e.live_contexts == 0u);
    CHECK(e.last_instance == kInstance && e.last_physical_device == kPhysical && e.last_device == kDevice);
    CHECK(xess_status_name(XessStatus::DeviceUnsupported) != std::string_view("unknown"));
}

// ---- marshalling -------------------------------------------------------------------------------------------------

void test_marshalling() {
    const QualityMode modes[] = {QualityMode::NativeAA, QualityMode::UltraQuality, QualityMode::Quality,
                                 QualityMode::Balanced, QualityMode::Performance, QualityMode::UltraPerformance};
    const FuseXessQuality expect[] = {FUSE_XESS_QUALITY_AA, FUSE_XESS_QUALITY_ULTRA_QUALITY_PLUS, FUSE_XESS_QUALITY_ULTRA_QUALITY,
                                      FUSE_XESS_QUALITY_QUALITY, FUSE_XESS_QUALITY_BALANCED, FUSE_XESS_QUALITY_ULTRA_PERFORMANCE};
    for (u32 i = 0; i < 6; ++i) {
        CHECK(to_xess_quality(modes[i]) == expect[i]);
        CHECK(std::fabs(xess_quality_ratio(to_xess_quality(modes[i])) - upscale::quality_mode_ratio(modes[i])) < 1e-6f);
    }
    CHECK(to_upscale_status(FUSE_XESS_RESULT_WARNING_OLD_DRIVER) == UpscaleStatus::Ok);
    CHECK(to_upscale_status(FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT) == UpscaleStatus::InvalidInputs);
    CHECK(to_upscale_status(FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE) == UpscaleStatus::BackendUnavailable);
    CHECK(to_upscale_status(FUSE_XESS_RESULT_ERROR_DEVICE) == UpscaleStatus::LaunchFailed);

    // Pure mapping.
    UpscaleInputs in = inputs(1920, 1080, QualityMode::Quality, 3);
    in.jitter_px = math::Vec2(0.25f, -0.125f);
    XessGpuFrame g = gpu_frame();
    XessMappedFrame m = map_upscale_inputs(in, g, false);
    CHECK(m.velocity_scale[0] == -1280.f && m.velocity_scale[1] == -720.f);
    CHECK(m.execute.jitter_offset_x == -0.25f && m.execute.jitter_offset_y == 0.125f);
    CHECK(m.execute.exposure_scale == 0.75f);
    CHECK(m.execute.reset_history == 0u);
    CHECK(m.execute.input_width == 1280u && m.execute.input_height == 720u);
    CHECK(m.execute.color_texture.width == 1280u && m.execute.velocity_texture.height == 720u);
    CHECK(m.execute.output_texture.width == 1920u && m.execute.output_texture.height == 1080u);
    CHECK(m.execute.depth_texture.subresource_range.aspect_mask == 2u);
    CHECK(m.execute.color_texture.subresource_range.aspect_mask == 1u && m.execute.color_texture.subresource_range.level_count == 1u);
    CHECK(m.execute.exposure_scale_texture.image == 0u && m.execute.responsive_pixel_mask_texture.image_view == 0u);
    CHECK(m.init_flags == FUSE_XESS_INIT_FLAG_INVERTED_DEPTH);
    g.depth_reversed_z = false;
    g.hdr = false;
    g.responsive = view(0xA5);
    g.exposure = view(0xA6);
    g.debug_flip_jitter_sign = true;
    in.reset_history = true;
    m = map_upscale_inputs(in, g, false);
    CHECK(m.init_flags == (FUSE_XESS_INIT_FLAG_LDR_INPUT_COLOR | FUSE_XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK |
                           FUSE_XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE));
    CHECK(m.execute.jitter_offset_x == 0.25f && m.execute.reset_history == 1u);
    CHECK(m.execute.exposure_scale_texture.width == 1u && m.execute.responsive_pixel_mask_texture.width == 1280u);

    // Through the mock.
    const Mock mk = load_mock();
    u64 sig[2] = {0, 0};
    for (u32 run = 0; run < 2; ++run) {
        mk.reset();
        XessUpscaler up(mk.rt, device());
        CHECK(up.render_size({1920, 1080}, QualityMode::Performance) == upscale::render_extent({1920, 1080}, QualityMode::Performance));
        up.set_quality(QualityMode::Quality);
        for (u32 f = 0; f < 4; ++f) {
            const UpscaleInputs fi = inputs(1920, 1080, QualityMode::Quality, f);
            up.bind_gpu_frame(gpu_frame());
            CHECK(up.evaluate({}, fi, {}) == UpscaleStatus::Ok);
            const FuseXessMockEcho e = mk.get();
            CHECK(e.last_execute.jitter_offset_x == -fi.jitter_px.x && e.last_execute.jitter_offset_y == -fi.jitter_px.y);
            CHECK(e.last_execute.reset_history == (f == 0 ? 1u : 0u));
            CHECK(e.last_command_buffer == kCmd);
        }
        const FuseXessMockEcho e = mk.get();
        CHECK(e.create_calls == 1u && e.init_calls == 1u && e.build_calls == 1u && e.execute_calls == 4u);
        CHECK(e.last_build_blocking == 1u && e.last_build_flags == FUSE_XESS_INIT_FLAG_INVERTED_DEPTH);
        CHECK(e.last_init.output_resolution.x == 1920u && e.last_init.output_resolution.y == 1080u);
        CHECK(e.last_init.quality_setting == FUSE_XESS_QUALITY_ULTRA_QUALITY);
        CHECK(e.last_init.init_flags == FUSE_XESS_INIT_FLAG_INVERTED_DEPTH && e.last_init.pipeline_cache == 0x77u);
        CHECK(e.last_init.creation_node_mask == 1u && e.last_init.visible_node_mask == 1u);
        CHECK(e.velocity_calls == 1u && e.velocity_scale[0] == -1280.f && e.velocity_scale[1] == -720.f);
        CHECK(e.jitter_scale_calls == 1u && e.jitter_scale[0] == 1.f && e.jitter_scale[1] == 1.f);
        CHECK(e.exposure_multiplier_calls == 1u && e.exposure_multiplier == 1.f);
        CHECK(e.last_execute.exposure_scale == 0.75f && e.last_execute.input_width == 1280u);
        CHECK(up.render_size({1920, 1080}, QualityMode::Performance) == (upscale::Extent2D{960u, 540u}));
        CHECK(mk.get().input_resolution_calls == 1u);
        sig[run] = e.output_signature;
    }
    CHECK(sig[0] != 0u && sig[0] == sig[1]);
    CHECK(mk.get().live_contexts == 0u);
}

// ---- lifecycle ---------------------------------------------------------------------------------------------------

void test_lifecycle() {
    const Mock mk = load_mock();
    {
        XessUpscaler none(nullptr, device());
        none.bind_gpu_frame(gpu_frame());
        CHECK(none.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::BackendUnavailable);
    }
    XessUpscaler up(mk.rt, device());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::BackendUnavailable); // unbound
    XessGpuFrame g = gpu_frame();
    g.depth = FuseXessVkImageViewInfo{};
    up.bind_gpu_frame(g);
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::InvalidInputs);
    UpscaleInputs bad = inputs(1920, 1080, QualityMode::Quality, 0);
    bad.resolution = upscale::make_resolution({400, 200}, {1920, 1080});
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, bad, {}) == UpscaleStatus::UnsupportedRatio);
    CHECK(mk.get().create_calls == 0u); // nothing reached the runtime

    // Context creation failure.
    mk.set(FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE, 0, 0);
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::BackendUnavailable);
    CHECK(up.last_result() == FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE);
    // Init failure, then execute failure, then recovery.
    mk.set(0, FUSE_XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY, 0);
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::LaunchFailed);
    mk.set(0, 0, FUSE_XESS_RESULT_ERROR_DEVICE);
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::LaunchFailed);
    CHECK(up.last_result() == FUSE_XESS_RESULT_ERROR_DEVICE);
    mk.set(0, 0, 0);
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::Ok);
    CHECK(mk.get().last_execute.reset_history == 1u); // never accumulated yet
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, 1), {}) == UpscaleStatus::Ok);
    CHECK(mk.get().last_execute.reset_history == 0u && up.accumulated_frames() == 2u);

    // Dynamic resolution: render size change resets history, no re-init.
    const u32 gen0 = up.history_generation();
    const u32 inits0 = mk.get().init_calls;
    UpscaleInputs dyn = inputs(1920, 1080, QualityMode::Quality, 2);
    dyn.resolution = upscale::make_resolution({1200, 675}, {1920, 1080});
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, dyn, {}) == UpscaleStatus::Ok);
    CHECK(up.history_generation() == gen0 + 1u && up.last_reset_reason() == upscale::HistoryResetReason::ResolutionChange);
    CHECK(mk.get().init_calls == inits0 && mk.get().last_execute.reset_history == 1u);
    CHECK(mk.get().velocity_scale[0] == -1200.f && mk.get().velocity_scale[1] == -675.f);
    // Camera cut.
    dyn.reset_history = true;
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, dyn, {}) == UpscaleStatus::Ok);
    CHECK(up.last_reset_reason() == upscale::HistoryResetReason::CameraCut && mk.get().last_execute.reset_history == 1u);
    // Quality change: re-init.
    up.set_quality(QualityMode::Performance);
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Performance, 5), {}) == UpscaleStatus::Ok);
    CHECK(mk.get().init_calls == inits0 + 1u && mk.get().last_init.quality_setting == FUSE_XESS_QUALITY_BALANCED);
    CHECK(up.last_reset_reason() == upscale::HistoryResetReason::ResolutionChange ||
          up.last_reset_reason() == upscale::HistoryResetReason::QualityModeChange);
    CHECK(mk.get().last_execute.reset_history == 1u);
    // Display change: re-init with the new output.
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(2560, 1440, QualityMode::Performance, 6), {}) == UpscaleStatus::Ok);
    CHECK(mk.get().init_calls == inits0 + 2u && mk.get().last_init.output_resolution.x == 2560u);
    CHECK(up.last_reset_reason() == upscale::HistoryResetReason::ResolutionChange);
    // Unchanged frame: nothing re-sent.
    const FuseXessMockEcho before = mk.get();
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(2560, 1440, QualityMode::Performance, 7), {}) == UpscaleStatus::Ok);
    CHECK(mk.get().init_calls == before.init_calls && mk.get().velocity_calls == before.velocity_calls);
    CHECK(mk.get().last_execute.reset_history == 0u);
    // Explicit invalidation.
    up.invalidate_history(upscale::HistoryResetReason::Explicit);
    up.bind_gpu_frame(gpu_frame());
    CHECK(up.evaluate({}, inputs(2560, 1440, QualityMode::Performance, 8), {}) == UpscaleStatus::Ok);
    CHECK(mk.get().last_execute.reset_history == 1u && up.accumulated_frames() == 1u);
}

// ---- registry ----------------------------------------------------------------------------------------------------

void test_registry() {
    const Mock mk = load_mock();
    upscale::UpscalerRegistry reg;
    reg.register_builtin_backends();
    std::string detail;
    CHECK(register_xess_backend(reg, nullptr, device(), &detail) == XessStatus::NotConfigured);
    CHECK(register_xess_backend(reg, mk.rt, XessDeviceBinding{}, &detail) == XessStatus::ContextFailed);
    mk.set(FUSE_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE, 0, 0);
    CHECK(register_xess_backend(reg, mk.rt, device(), &detail) == XessStatus::DeviceUnsupported);
    CHECK(reg.find(kXessName) == nullptr);

    upscale::UpscalerRequirements vk{};
    vk.api = upscale::UpscalerApi::Vulkan;
    vk.mode = QualityMode::Quality;
    upscale::UpscalerRequirements cpu = vk;
    cpu.api = upscale::UpscalerApi::Cpu;
    // Not registered: Vulkan request falls back to the in-tree CPU path.
    UpscalerChoice c = select_upscaler_with_fallback(reg, kXessName, vk);
    CHECK(c.caps && c.fell_back && std::string_view(c.caps->name) == upscale::kNativeTaauName);
    CHECK(contains(c.reason, "in-tree"));

    mk.set(0, 0, 0);
    CHECK(register_xess_backend(reg, mk.rt, device(), &detail) == XessStatus::Available);
    const upscale::UpscalerCaps* caps = reg.find(kXessName);
    CHECK(caps && caps->temporal && caps->supports_api(upscale::UpscalerApi::Vulkan) && !caps->supports_api(upscale::UpscalerApi::Cpu));
    CHECK(caps && caps->accepts_reactive_mask && caps->supports_dynamic_resolution && caps->max_ratio == 3.f);
    c = select_upscaler_with_fallback(reg, kXessName, vk);
    CHECK(c.caps == caps && !c.fell_back);
    CHECK(reg.select(vk) == caps); // the only Vulkan backend in this registry
    // CPU dispatch never selects xess.
    c = select_upscaler_with_fallback(reg, kXessName, cpu);
    CHECK(c.caps && c.fell_back && std::string_view(c.caps->name) == upscale::kNativeTaauName);
    // Requirements xess cannot meet (no motion vectors) -> not xess.
    upscale::UpscalerRequirements nomv = vk;
    nomv.have_motion_vectors = false;
    c = select_upscaler_with_fallback(reg, kXessName, nomv);
    CHECK(c.fell_back && (c.caps == nullptr || std::string_view(c.caps->name) != kXessName));

    // Factory instance runs through the mock with the bound device.
    std::unique_ptr<upscale::IUpscaler> made = reg.create(kXessName);
    auto* xu = dynamic_cast<XessUpscaler*>(made.get());
    CHECK(xu != nullptr);
    if (xu) {
        xu->bind_gpu_frame(gpu_frame());
        CHECK(xu->evaluate({}, inputs(1920, 1080, QualityMode::Quality, 0), {}) == UpscaleStatus::Ok);
        CHECK(mk.get().last_device == kDevice);
    }
    made.reset();
    unregister_xess_backend(reg);
    CHECK(reg.find(kXessName) == nullptr);
    c = select_upscaler_with_fallback(reg, kXessName, vk);
    CHECK(c.fell_back && c.caps && std::string_view(c.caps->name) == upscale::kNativeTaauName);
    CHECK(mk.get().live_contexts == 0u);
}

// ---- zero_alloc --------------------------------------------------------------------------------------------------

void test_zero_alloc() {
    const Mock mk = load_mock();
    XessUpscaler up(mk.rt, device());
    const XessGpuFrame g = gpu_frame();
    for (u32 f = 0; f < 4; ++f) { // warm-up: context, init
        up.bind_gpu_frame(g);
        CHECK(up.evaluate({}, inputs(1920, 1080, QualityMode::Quality, f), {}) == UpscaleStatus::Ok);
    }
    UpscaleInputs frames[64];
    for (u32 f = 0; f < 64; ++f) {
        frames[f] = inputs(1920, 1080, QualityMode::Quality, 4 + f);
        frames[f].reset_history = (f % 17) == 16;
    }
    u32 ok = 0;
    t_allocations = 0;
    t_count = true;
    for (u32 f = 0; f < 64; ++f) {
        up.bind_gpu_frame(g);
        ok += up.evaluate({}, frames[f], {}) == UpscaleStatus::Ok ? 1u : 0u;
    }
    t_count = false;
    std::printf("  zero_alloc: 64 frames, %llu operator new, %u ok\n", t_allocations, ok);
    CHECK(t_allocations == 0u);
    CHECK(ok == 64u);
    CHECK(mk.get().execute_calls == 68u);
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "discovery") {
        std::printf("discovery\n");
        test_discovery();
    }
    if (all || suite == "marshalling") {
        std::printf("marshalling\n");
        test_marshalling();
    }
    if (all || suite == "lifecycle") {
        std::printf("lifecycle\n");
        test_lifecycle();
    }
    if (all || suite == "registry") {
        std::printf("registry\n");
        test_registry();
    }
    if (all || suite == "zero_alloc") {
        std::printf("zero_alloc\n");
        test_zero_alloc();
    }
    std::printf("%s: %d failure(s)\n", suite.c_str(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
