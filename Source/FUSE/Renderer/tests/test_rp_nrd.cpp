// WP-6.4b NVIDIA NRD plugin + denoiser interface: CPU gates against the mock provider (plugins/nvidia_nrd/mock). No
// NRD code, GPU or SDK; hardware behaviour is a manual check (docs/unification/RENDERER-EXECUTION.md WP-6.4b).
//
//   discovery    not configured / not found / not a library / no entry point / ABI major / truncated table / runtime
//                missing / graphics API / no denoisers, each with its reason; probe() compiled out unless
//                FUSE_ENABLE_NRD_PLUGIN; full and partial denoiser discovery
//   marshalling  (signal, method) -> SIGMA / REBLUR / RELAX; common settings (matrices, motion scale (-1, -1, 0), jitter,
//                rect, reset, disocclusion, range) and denoiser settings (history clamp, anti-firefly) exactly as the
//                mock receives them; instance lifetime (method change, resize, retire); failure paths; deterministic
//                output signature
//   graph        one "denoise.nrd" pass on render graph v2 with ExternalRead / ExternalWrite on every bound slot; the
//                graph compiles and plans; dispatch passes GENERAL layouts and the command buffer through
//   registry     DenoiserRegistry: in-tree "svgf" always, "nrd" only with a provider; selection by method / signal /
//                inputs and fallback to SVGF with reasons; SvgfDenoiserAdapter settings mapping
//   zero_alloc   64 steady-state frames (bind, beginFrame, importInto, addPasses, dispatch; graph reset each frame):
//                0 operator new
#include <fuse/renderer/denoise/denoiser.hpp>
#include <fuse/renderer/nrd/nrd_denoiser.hpp>
#include <fuse/renderer/rg/graph.hpp>

#include "fuse_nrd_mock_echo.h"

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
using namespace fuse::renderer::nrd;
using denoise::DenoiserMethod;
using denoise::DenoiseSignal;
namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

constexpr u64 kCmd = 0xC0FFEEu;

struct Mock {
    std::shared_ptr<NrdPlugin> plugin;
    FuseNrdMockEchoFn echo = nullptr;
    FuseNrdMockResetFn reset = nullptr;
    FuseNrdMockEcho get() const {
        FuseNrdMockEcho e{};
        echo(&e);
        return e;
    }
};

Mock load_mock(const char* options = "") {
    NrdPluginConfig c;
    c.options = options;
    c.graphics_api = FUSE_NRD_API_VULKAN;
    NrdLoadResult r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK, c);
    if (r.status != NrdPluginStatus::Available) {
        std::printf("  mock failed to load: %s\n", r.detail.c_str());
        std::exit(1);
    }
    Mock m;
    m.plugin = std::move(r.plugin);
    m.echo = reinterpret_cast<FuseNrdMockEchoFn>(m.plugin->symbol(FUSE_NRD_MOCK_ECHO_NAME));
    m.reset = reinterpret_cast<FuseNrdMockResetFn>(m.plugin->symbol(FUSE_NRD_MOCK_RESET_NAME));
    if (!m.echo || !m.reset) {
        std::printf("  mock test hooks missing\n");
        std::exit(1);
    }
    m.reset();
    return m;
}

FuseNrdResource res(u64 id, u32 w, u32 h, u32 fmt = 97u) {
    FuseNrdResource r{};
    r.native = id;
    r.view = id + 0x1000u;
    r.native_format = fmt;
    r.width = w;
    r.height = h;
    r.layout = 5u; // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    return r;
}

math::Mat4 mat(f32 seed) {
    math::Mat4 m{};
    for (u32 i = 0; i < 16; ++i) {
        m.data[i] = seed + static_cast<f32>(i) * 0.5f;
    }
    return m;
}

/// All NRD slots bound (inputs + every output) for w x h.
NrdNativeFrame native_frame(u32 w, u32 h, u32 frame) {
    NrdNativeFrame f{};
    for (u32 s = 0; s < FUSE_NRD_OUT_VALIDATION; ++s) {
        f.set(s, res(0x100u + s, w, h));
    }
    f.view = mat(1.f);
    f.projection = mat(2.f);
    f.prev_view = mat(3.f);
    f.prev_projection = mat(4.f);
    f.jitter_px = math::Vec2(0.125f * static_cast<f32>(frame % 4u) - 0.25f, -0.2f);
    f.prev_jitter_px = math::Vec2(0.1f, 0.3f);
    f.denoising_range = 500.f;
    f.time_delta_ms = 16.f;
    return f;
}

denoise::DenoiserSettings settings(DenoiseSignal s, DenoiserMethod m) {
    denoise::DenoiserSettings d{};
    d.signal = s;
    d.method = m;
    d.max_history_frames = 30u;
    d.disocclusion_depth = 0.02f;
    d.anti_firefly = true;
    return d;
}

denoise::DenoiseFrameDesc frame_desc(u32 w, u32 h, bool reset = false) {
    denoise::DenoiseFrameDesc d{};
    d.width = w;
    d.height = h;
    d.reset = reset;
    return d;
}

// ---- discovery ---------------------------------------------------------------------------------------------------

void test_discovery() {
    {
        const NrdLoadResult r = NrdPluginLoader::load(NrdPluginConfig{});
        CHECK(r.status == NrdPluginStatus::NotConfigured && contains(r.detail, "FUSE_NRD_SDK_DIR"));
    }
    {
        NrdPluginConfig c;
        c.sdk_dir = FUSE_NRD_TEST_SCRATCH "/missing";
        const NrdLoadResult r = NrdPluginLoader::load(c);
        CHECK(r.status == NrdPluginStatus::LibraryNotFound && contains(r.detail, std::string(default_nrd_provider_library_name()).c_str()));
    }
    {
        std::error_code ec;
        fs::create_directories(FUSE_NRD_TEST_SCRATCH, ec);
        const std::string bogus = std::string(FUSE_NRD_TEST_SCRATCH) + "/" + std::string(default_nrd_provider_library_name());
        std::ofstream(bogus) << "not a shared library";
        CHECK(NrdPluginLoader::load_file(bogus, {}).status == NrdPluginStatus::LoadFailed);
    }
    {
        const NrdLoadResult r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK_NOENTRY, {});
        CHECK(r.status == NrdPluginStatus::EntryPointMissing && contains(r.detail, FUSE_NRD_PLUGIN_ENTRY_NAME));
    }
    {
        const NrdLoadResult r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK_BADABI, {});
        CHECK(r.status == NrdPluginStatus::AbiMismatch && contains(r.detail, "provider ABI 2.0"));
    }
    {
        const NrdLoadResult r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK_TRUNCATED, {});
        CHECK(r.status == NrdPluginStatus::AbiMismatch && contains(r.detail, "incomplete"));
    }
    NrdPluginConfig c;
    c.options = "runtime=missing";
    NrdLoadResult r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK, c);
    CHECK(r.status == NrdPluginStatus::RuntimeMissing && contains(r.detail, "runtime missing"));
    c.options = "api=d3d12";
    c.graphics_api = FUSE_NRD_API_VULKAN;
    r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK, c);
    CHECK(r.status == NrdPluginStatus::GraphicsApi);
    c.options = "disable=sigma,reblur,relax";
    r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK, c);
    CHECK(r.status == NrdPluginStatus::NoDenoisers && !r.plugin && contains(r.detail, "no usable denoiser"));
    c.options = "disable=relax";
    r = NrdPluginLoader::load_file(FUSE_NRD_TEST_MOCK, c);
    CHECK(r.status == NrdPluginStatus::Available && r.plugin);
    if (r.plugin) {
        CHECK(r.plugin->denoiser_mask() == (FUSE_NRD_DENOISER_BIT(0) | FUSE_NRD_DENOISER_BIT(1) | FUSE_NRD_DENOISER_BIT(2)));
        CHECK(r.plugin->denoiser(FUSE_NRD_DENOISER_RELAX_DIFFUSE).status == FUSE_NRD_ERR_UNSUPPORTED_DENOISER);
    }
    c.options = "";
    c.sdk_dir = fs::path(FUSE_NRD_TEST_MOCK).parent_path().string();
    c.library_name = fs::path(FUSE_NRD_TEST_MOCK).filename().string();
    r = NrdPluginLoader::load(c);
    CHECK(r.status == NrdPluginStatus::Available && r.plugin && contains(r.detail, "provider 'mock'"));
    if (r.plugin) {
        CHECK(r.plugin->denoiser_mask() == 0x1Fu);
        const FuseNrdDenoiserSupport& s = r.plugin->denoiser(FUSE_NRD_DENOISER_SIGMA_SHADOW);
        CHECK((s.required_inputs & FUSE_NRD_SLOT_BIT(FUSE_NRD_IN_PENUMBRA)) != 0u);
        CHECK(s.outputs == FUSE_NRD_SLOT_BIT(FUSE_NRD_OUT_SHADOW_TRANSLUCENCY));
    }
    const NrdLoadResult p = NrdPluginLoader::probe(NrdPluginConfig{});
    CHECK(NrdPluginLoader::enabled_at_build() ? p.status != NrdPluginStatus::DisabledAtBuild : p.status == NrdPluginStatus::DisabledAtBuild);
}

// ---- marshalling -------------------------------------------------------------------------------------------------

void test_marshalling() {
    CHECK(to_nrd_denoiser(DenoiseSignal::Shadow, DenoiserMethod::Sigma) == FUSE_NRD_DENOISER_SIGMA_SHADOW);
    CHECK(to_nrd_denoiser(DenoiseSignal::Gi, DenoiserMethod::Reblur) == FUSE_NRD_DENOISER_REBLUR_DIFFUSE);
    CHECK(to_nrd_denoiser(DenoiseSignal::Reflection, DenoiserMethod::Reblur) == FUSE_NRD_DENOISER_REBLUR_SPECULAR);
    CHECK(to_nrd_denoiser(DenoiseSignal::Gi, DenoiserMethod::Relax) == FUSE_NRD_DENOISER_RELAX_DIFFUSE);
    CHECK(to_nrd_denoiser(DenoiseSignal::Reflection, DenoiserMethod::Relax) == FUSE_NRD_DENOISER_RELAX_SPECULAR);
    CHECK(to_nrd_denoiser(DenoiseSignal::Shadow, DenoiserMethod::Reblur) == FUSE_NRD_DENOISER_COUNT);
    CHECK(to_nrd_denoiser(DenoiseSignal::Gi, DenoiserMethod::Sigma) == FUSE_NRD_DENOISER_COUNT);
    CHECK(to_nrd_denoiser(DenoiseSignal::Gi, DenoiserMethod::Svgf) == FUSE_NRD_DENOISER_COUNT);
    CHECK(nrd_output_slot(FUSE_NRD_DENOISER_SIGMA_SHADOW) == FUSE_NRD_OUT_SHADOW_TRANSLUCENCY);
    CHECK(nrd_output_slot(FUSE_NRD_DENOISER_RELAX_SPECULAR) == FUSE_NRD_OUT_SPEC_RADIANCE_HITDIST);
    CHECK(nrd_output_slot(FUSE_NRD_DENOISER_REBLUR_DIFFUSE) == FUSE_NRD_OUT_DIFF_RADIANCE_HITDIST);

    // Pure mapping.
    NrdNativeFrame f = native_frame(320, 180, 1);
    f.rect_width = 300;
    f.rect_height = 900; // clamped to the resource
    const denoise::DenoiserSettings st = settings(DenoiseSignal::Gi, DenoiserMethod::Reblur);
    FuseNrdCommonSettings c = map_common_settings(f, 320, 180, 7, true, st);
    CHECK(c.struct_size == sizeof(FuseNrdCommonSettings) && c.frame_index == 7u && c.flags == FUSE_NRD_COMMON_RESET);
    CHECK(c.resource_width == 320u && c.resource_height == 180u && c.rect_width == 300u && c.rect_height == 180u);
    CHECK(std::memcmp(c.view_to_clip, f.projection.data.data(), 64) == 0 && std::memcmp(c.world_to_view, f.view.data.data(), 64) == 0);
    CHECK(std::memcmp(c.view_to_clip_prev, f.prev_projection.data.data(), 64) == 0 &&
          std::memcmp(c.world_to_view_prev, f.prev_view.data.data(), 64) == 0);
    CHECK(c.motion_vector_scale[0] == -1.f && c.motion_vector_scale[1] == -1.f && c.motion_vector_scale[2] == 0.f);
    CHECK(c.camera_jitter[0] == f.jitter_px.x && c.camera_jitter[1] == f.jitter_px.y);
    CHECK(c.camera_jitter_prev[0] == 0.1f && c.camera_jitter_prev[1] == 0.3f);
    CHECK(c.resolution_scale[0] == 300.f / 320.f && c.resolution_scale[1] == 1.f);
    CHECK(c.denoising_range == 500.f && c.disocclusion_threshold == 0.02f && c.time_delta_ms == 16.f);
    f.set(FUSE_NRD_OUT_VALIDATION, res(0x999, 320, 180));
    c = map_common_settings(f, 320, 180, 8, false, st);
    CHECK(c.flags == FUSE_NRD_COMMON_VALIDATION);
    FuseNrdDenoiserSettings ds = map_denoiser_settings(FUSE_NRD_DENOISER_REBLUR_DIFFUSE, st);
    CHECK(ds.denoiser == FUSE_NRD_DENOISER_REBLUR_DIFFUSE && ds.max_accumulated_frames == 30u && ds.max_fast_accumulated_frames == 6u);
    CHECK(ds.anti_firefly == 1u && ds.hit_distance_a == 3.f && ds.hit_distance_b == 0.1f && ds.hit_distance_c == 20.f);
    denoise::DenoiserSettings big = st;
    big.max_history_frames = 500u;
    CHECK(map_denoiser_settings(FUSE_NRD_DENOISER_RELAX_DIFFUSE, big).max_accumulated_frames == 63u);
    CHECK(map_denoiser_settings(FUSE_NRD_DENOISER_SIGMA_SHADOW, st).max_accumulated_frames == 0u);

    // Through the mock.
    const Mock mk = load_mock();
    u64 sig[2] = {0, 0};
    for (u32 run = 0; run < 2; ++run) {
        mk.reset();
        NrdDenoiser d(mk.plugin);
        CHECK(d.caps().supports(DenoiserMethod::Sigma) && d.caps().supports(DenoiserMethod::Reblur) && d.caps().supports(DenoiserMethod::Relax));
        CHECK(!d.configure(settings(DenoiseSignal::Shadow, DenoiserMethod::Reblur)));
        CHECK(d.last_status() == FUSE_NRD_ERR_UNSUPPORTED_DENOISER);
        CHECK(!d.beginFrame(1, frame_desc(320, 180))); // not configured
        CHECK(d.configure(settings(DenoiseSignal::Gi, DenoiserMethod::Reblur)));
        CHECK(!d.beginFrame(1, frame_desc(320, 180)) && d.last_status() == FUSE_NRD_ERR_MISSING_INPUT); // nothing bound
        CHECK(d.dispatch(kCmd) == FUSE_NRD_ERR_MISSING_SETTINGS);
        NrdNativeFrame nf = native_frame(320, 180, 0);
        nf.set(FUSE_NRD_IN_VIEWZ, FuseNrdResource{});
        d.bindNativeFrame(nf);
        CHECK(!d.beginFrame(1, frame_desc(320, 180)) && d.last_status() == FUSE_NRD_ERR_MISSING_INPUT);
        d.bindNativeFrame(native_frame(320, 180, 0));
        CHECK(!d.beginFrame(1, frame_desc(0, 180)) && d.last_status() == FUSE_NRD_ERR_INVALID_ARGUMENT);
        CHECK(mk.get().create_calls == 0u);
        for (u32 i = 0; i < 4; ++i) {
            d.bindNativeFrame(native_frame(320, 180, i));
            CHECK(d.beginFrame(10 + i, frame_desc(320, 180, i == 2)));
            CHECK(d.dispatch(kCmd) == FUSE_NRD_OK);
            const FuseNrdMockEcho e = mk.get();
            CHECK(std::memcmp(&e.last_common, &d.common_settings(), sizeof(FuseNrdCommonSettings)) == 0);
            CHECK(e.last_common.frame_index == i);
            CHECK(e.last_common.flags == ((i == 0 || i == 2) ? FUSE_NRD_COMMON_RESET : 0u));
            CHECK(e.last_common.camera_jitter[0] == native_frame(320, 180, i).jitter_px.x);
            CHECK(e.last_command_buffer == kCmd && e.last_dispatch_denoiser == FUSE_NRD_DENOISER_REBLUR_DIFFUSE);
            CHECK(e.last_binding_count == 9u);
        }
        FuseNrdMockEcho e = mk.get();
        CHECK(e.create_calls == 1u && e.settings_calls == 1u && e.common_calls == 4u && e.dispatch_calls == 4u);
        CHECK(e.last_create_width == 320u && e.last_create_height == 180u);
        CHECK(e.last_settings.max_accumulated_frames == 30u && e.last_settings.anti_firefly == 1u);
        CHECK(e.last_bindings[0].slot == FUSE_NRD_IN_MV && e.last_bindings[0].resource.native == 0x100u &&
              e.last_bindings[0].resource.layout == 5u);
        sig[run] = e.output_signature;
        // Method change: new instance, history restarts.
        CHECK(d.configure(settings(DenoiseSignal::Gi, DenoiserMethod::Relax)));
        d.bindNativeFrame(native_frame(320, 180, 4));
        CHECK(d.beginFrame(20, frame_desc(320, 180)) && d.dispatch(kCmd) == FUSE_NRD_OK);
        e = mk.get();
        CHECK(e.create_calls == 2u && e.last_create_denoiser == FUSE_NRD_DENOISER_RELAX_DIFFUSE);
        CHECK(e.last_common.flags == FUSE_NRD_COMMON_RESET && e.settings_calls == 2u);
        // Resize: new instance; the old ones are retired until their serial completes.
        d.bindNativeFrame(native_frame(640, 360, 5));
        CHECK(d.beginFrame(21, frame_desc(640, 360)) && d.dispatch(kCmd) == FUSE_NRD_OK);
        CHECK(mk.get().create_calls == 3u && mk.get().live_instances == 3u && d.instances_created() == 3u);
        CHECK(d.collectRetired(19) == 1u); // the REBLUR instance, last used at serial 13
        CHECK(d.collectRetired(21) == 1u); // the first RELAX instance, last used at serial 20
        CHECK(mk.get().live_instances == 1u && mk.get().destroy_calls == 2u);
        // Settings-only change: same instance, settings re-sent.
        denoise::DenoiserSettings s2 = settings(DenoiseSignal::Gi, DenoiserMethod::Relax);
        s2.anti_firefly = false;
        CHECK(d.configure(s2));
        d.bindNativeFrame(native_frame(640, 360, 6));
        CHECK(d.beginFrame(22, frame_desc(640, 360)) && d.dispatch(kCmd) == FUSE_NRD_OK);
        CHECK(mk.get().create_calls == 3u && mk.get().last_settings.anti_firefly == 0u && mk.get().last_common.flags == 0u);
        // The next frame needs a new bind.
        CHECK(!d.beginFrame(23, frame_desc(640, 360)) && d.last_status() == FUSE_NRD_ERR_MISSING_INPUT);
    }
    CHECK(sig[0] != 0u && sig[0] == sig[1]);
    CHECK(mk.get().live_instances == 0u);
}

// ---- graph -------------------------------------------------------------------------------------------------------

void test_graph() {
    const Mock mk = load_mock();
    NrdDenoiser d(mk.plugin);
    CHECK(d.configure(settings(DenoiseSignal::Shadow, DenoiserMethod::Sigma)));
    NrdNativeFrame nf{};
    nf.set(FUSE_NRD_IN_MV, res(0x201, 256, 128, 103u));
    nf.set(FUSE_NRD_IN_NORMAL_ROUGHNESS, res(0x202, 256, 128));
    nf.set(FUSE_NRD_IN_VIEWZ, res(0x203, 256, 128, 100u));
    nf.set(FUSE_NRD_IN_PENUMBRA, res(0x204, 256, 128, 76u));
    nf.set(FUSE_NRD_OUT_SHADOW_TRANSLUCENCY, res(0x205, 256, 128, 76u));
    d.bindNativeFrame(nf);
    CHECK(d.beginFrame(1, frame_desc(256, 128)));
    rg::Graph g;
    const denoise::DenoiseGraphRefs refs = d.importInto(g);
    CHECK(refs.outputImage.valid() && refs.outputImage.id == d.slot_ref(FUSE_NRD_OUT_SHADOW_TRANSLUCENCY).id);
    CHECK(!refs.output.valid() && !refs.state.valid());
    CHECK(g.resourceCount() == 5u);
    d.addPasses(g, refs, denoise::DenoiseGraphInputs{});
    CHECK(g.passCount() == 1u && std::string(g.passName(0)) == "denoise.nrd");
    CHECK(g.compile());
    CHECK(!g.passCulled(0) && g.executionOrder().size() == 1u);
    CHECK(g.plan());
    std::printf("  graph: %u image barriers before denoise.nrd\n", g.passBarriers(0).imageCount);
    // What the pass callback does (no executor on the CPU): dispatch with the recorded command buffer.
    CHECK(d.dispatch(kCmd) == FUSE_NRD_OK);
    const FuseNrdMockEcho e = mk.get();
    CHECK(e.last_binding_count == 5u && e.last_dispatch_denoiser == FUSE_NRD_DENOISER_SIGMA_SHADOW);
    for (u32 i = 0; i < e.last_binding_count; ++i) {
        CHECK(e.last_bindings[i].resource.layout == 1u); // VK_IMAGE_LAYOUT_GENERAL after External* accesses
    }
    // No frame begun: nothing imported, no pass.
    rg::Graph g2;
    const denoise::DenoiseGraphRefs none = d.importInto(g2);
    d.addPasses(g2, none, {});
    CHECK(!none.outputImage.valid() && g2.passCount() == 0u);
}

// ---- registry ----------------------------------------------------------------------------------------------------

void test_registry() {
    using denoise::DenoiserRegistry;
    using denoise::DenoiserRequirements;
    DenoiserRegistry reg;
    reg.register_builtin_denoisers();
    CHECK(reg.backends().size() == 1u && reg.find(denoise::kSvgfDenoiserName));
    CHECK(!reg.register_backend(denoise::svgf_denoiser_caps(), nullptr));

    DenoiserRequirements shadow{};
    shadow.signal = DenoiseSignal::Shadow;
    shadow.method = DenoiserMethod::Sigma;
    denoise::DenoiserSelection s = reg.select(shadow);
    CHECK(s.caps && std::string_view(s.caps->name) == "svgf" && s.method == DenoiserMethod::Svgf && s.fallback);
    CHECK(contains(s.reason, "unavailable"));
    shadow.allow_fallback = false;
    CHECK(reg.select(shadow).caps == nullptr);
    shadow.allow_fallback = true;

    CHECK(!register_nrd_denoiser(reg, nullptr));
    const Mock mk = load_mock();
    CHECK(register_nrd_denoiser(reg, mk.plugin));
    CHECK(!register_nrd_denoiser(reg, mk.plugin)); // name taken
    const denoise::DenoiserCaps* nc = reg.find(kNrdDenoiserName);
    CHECK(nc && nc->needs_native_frame && nc->needs_hit_distance(DenoiserMethod::Reblur) && !nc->needs_hit_distance(DenoiserMethod::Sigma) && !nc->in_tree && nc->signals == denoise::kAllDenoiseSignals);
    s = reg.select(shadow);
    CHECK(s.caps == nc && s.method == DenoiserMethod::Sigma && !s.fallback);
    shadow.have_native_frame = false;
    s = reg.select(shadow);
    CHECK(s.caps && std::string_view(s.caps->name) == "svgf" && s.fallback);

    DenoiserRequirements gi{};
    gi.signal = DenoiseSignal::Gi;
    gi.method = DenoiserMethod::Reblur;
    s = reg.select(gi); // no hit distance from the producer
    CHECK(s.fallback && s.method == DenoiserMethod::Svgf);
    gi.have_hit_distance = true;
    s = reg.select(gi);
    CHECK(s.caps == nc && s.method == DenoiserMethod::Reblur && !s.fallback);
    gi.method = DenoiserMethod::Relax;
    CHECK(reg.select(gi).caps == nc);
    DenoiserRequirements bad = gi;
    bad.signal = DenoiseSignal::Shadow;
    bad.method = DenoiserMethod::Reblur;
    s = reg.select(bad);
    CHECK(s.fallback && contains(s.reason, "cannot denoise this signal"));
    DenoiserRequirements asvgf{};
    asvgf.method = DenoiserMethod::ASvgf;
    s = reg.select(asvgf);
    CHECK(s.fallback && s.method == DenoiserMethod::Svgf); // no gradient samples yet
    asvgf.have_gradients = true;
    s = reg.select(asvgf);
    CHECK(!s.fallback && s.method == DenoiserMethod::ASvgf && std::string_view(s.caps->name) == "svgf");

    std::unique_ptr<denoise::IDenoiser> made = reg.create(kNrdDenoiserName, {});
    CHECK(made && std::string_view(made->caps().name) == kNrdDenoiserName);
    CHECK(made && made->configure(settings(DenoiseSignal::Reflection, DenoiserMethod::Relax)));
    CHECK(reg.create(denoise::kSvgfDenoiserName, {}) == nullptr); // no device: SVGF init fails cleanly
    CHECK(reg.create("nope", {}) == nullptr);
    made.reset();
    unregister_nrd_denoiser(reg);
    CHECK(reg.find(kNrdDenoiserName) == nullptr && registered_nrd_plugin() == nullptr);
    CHECK(reg.select(gi).fallback);

    // Partial provider: RELAX disabled -> no Relax in caps, falls back.
    const Mock partial = load_mock("disable=relax");
    CHECK(register_nrd_denoiser(reg, partial.plugin));
    CHECK(!reg.find(kNrdDenoiserName)->supports(DenoiserMethod::Relax));
    CHECK(reg.select(gi).fallback);
    gi.method = DenoiserMethod::Reblur;
    CHECK(!reg.select(gi).fallback);
    unregister_nrd_denoiser(reg);

    // SVGF adapter mapping (no device needed).
    denoise::SvgfDenoiserAdapter svgf;
    CHECK(!svgf.configure(settings(DenoiseSignal::Shadow, DenoiserMethod::Sigma)));
    denoise::DenoiserSettings ss = settings(DenoiseSignal::Shadow, DenoiserMethod::ASvgf);
    ss.max_history_frames = 16u;
    CHECK(svgf.configure(ss));
    const denoise::SvgfSettings& out = svgf.svgf().settings();
    const denoise::SvgfSettings preset = denoise::svgf_preset(DenoiseSignal::Shadow);
    CHECK(out.signal == DenoiseSignal::Shadow && out.gradients && out.maxHistory == 16.f && out.reprojDepth == 0.02f);
    CHECK(out.sigmaLuminance == preset.sigmaLuminance && out.atrousIterations == preset.atrousIterations);
    CHECK(!svgf.beginFrame(1, frame_desc(64, 64))); // not initialised
    CHECK(denoise::DenoiserRegistry::instance().find(denoise::kSvgfDenoiserName) != nullptr);
}

// ---- zero_alloc --------------------------------------------------------------------------------------------------

void test_zero_alloc() {
    const Mock mk = load_mock();
    NrdDenoiser d(mk.plugin);
    CHECK(d.configure(settings(DenoiseSignal::Reflection, DenoiserMethod::Reblur)));
    rg::Graph g;
    NrdNativeFrame frames[64];
    for (u32 f = 0; f < 64; ++f) {
        frames[f] = native_frame(320, 180, f);
    }
    auto run = [&](u32 f, bool reset) {
        g.reset();
        d.bindNativeFrame(frames[f % 64u]);
        bool ok = d.beginFrame(100 + f, frame_desc(320, 180, reset));
        const denoise::DenoiseGraphRefs refs = d.importInto(g);
        d.addPasses(g, refs, {});
        ok = ok && g.compile();
        ok = ok && d.dispatch(kCmd) == FUSE_NRD_OK;
        d.collectRetired(100 + f);
        return ok;
    };
    for (u32 f = 0; f < 4; ++f) {
        CHECK(run(f, false));
    }
    u32 ok = 0;
    t_allocations = 0;
    t_count = true;
    for (u32 f = 0; f < 64; ++f) {
        ok += run(4 + f, (f % 13u) == 12u) ? 1u : 0u;
    }
    t_count = false;
    std::printf("  zero_alloc: 64 frames (bind, beginFrame, importInto, addPasses, compile, dispatch), %llu operator new, %u ok\n",
                t_allocations, ok);
    CHECK(t_allocations == 0u);
    CHECK(ok == 64u);
    CHECK(mk.get().create_calls == 1u && mk.get().dispatch_calls == 68u);
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
    if (all || suite == "graph") {
        std::printf("graph\n");
        test_graph();
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
