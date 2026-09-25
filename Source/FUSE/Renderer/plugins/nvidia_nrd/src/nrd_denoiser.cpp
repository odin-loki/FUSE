// WP-6.4b NRD adapter (MIT): provider loader, parameter mapping, NrdDenoiser, registration. nrd_denoiser.hpp.
#include <fuse/renderer/nrd/nrd_denoiser.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>

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

namespace fuse::renderer::nrd {

namespace {

namespace fs = std::filesystem;
using denoise::DenoiserMethod;
using denoise::DenoiseSignal;

constexpr u32 kVkImageLayoutGeneral = 1u; // what rg Access::External* leaves an image in

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read at start-up only
    return v ? std::string(v) : std::string();
}

void* open_library(const std::string& path, std::string& error) {
#if defined(_WIN32)
    const std::wstring wide = fs::path(path).wstring();
    HMODULE h = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
        error = "LoadLibraryExW failed, GetLastError=" + std::to_string(GetLastError());
    }
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        error = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void close_library(void* h) {
    if (!h) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

void* find_symbol(void* h, const char* name) {
    if (!h) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

NrdLoadResult fail(NrdPluginStatus status, std::string detail) {
    NrdLoadResult r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

NrdPluginStatus status_from_init(FuseNrdStatus s) {
    switch (s) {
        case FUSE_NRD_ERR_ABI_MISMATCH: return NrdPluginStatus::AbiMismatch;
        case FUSE_NRD_ERR_RUNTIME_MISSING: return NrdPluginStatus::RuntimeMissing;
        case FUSE_NRD_ERR_GRAPHICS_API: return NrdPluginStatus::GraphicsApi;
        default: return NrdPluginStatus::InitFailed;
    }
}

constexpr u32 kMinApiSize = u32(offsetof(FuseNrdApi, status_string) + sizeof(FuseNrdApi::status_string));

std::mutex g_pluginMutex;
std::shared_ptr<NrdPlugin> g_plugin;

std::shared_ptr<NrdPlugin> current_plugin() {
    std::lock_guard<std::mutex> lock(g_pluginMutex);
    return g_plugin;
}

std::unique_ptr<denoise::IDenoiser> make_nrd(const denoise::DenoiserCreateInfo&) {
    std::shared_ptr<NrdPlugin> p = current_plugin();
    if (!p) {
        return nullptr;
    }
    return std::make_unique<NrdDenoiser>(std::move(p));
}

bool is_output(FuseNrdSlot s) { return s >= FUSE_NRD_OUT_DIFF_RADIANCE_HITDIST; }

} // namespace

// ---- loader ---------------------------------------------------------------------------------------------------

std::string_view nrd_plugin_status_name(NrdPluginStatus s) noexcept {
    switch (s) {
        case NrdPluginStatus::Available: return "available";
        case NrdPluginStatus::DisabledAtBuild: return "disabled at build (FUSE_ENABLE_NRD_PLUGIN=OFF)";
        case NrdPluginStatus::NotConfigured: return "not configured (set FUSE_NRD_SDK_DIR)";
        case NrdPluginStatus::LibraryNotFound: return "provider library not found";
        case NrdPluginStatus::LoadFailed: return "provider library failed to load";
        case NrdPluginStatus::EntryPointMissing: return "provider entry point missing";
        case NrdPluginStatus::AbiMismatch: return "provider ABI mismatch";
        case NrdPluginStatus::InitFailed: return "provider init failed";
        case NrdPluginStatus::RuntimeMissing: return "NRD runtime missing";
        case NrdPluginStatus::GraphicsApi: return "graphics API not supported by the provider";
        case NrdPluginStatus::NoDenoisers: return "no usable NRD denoiser";
    }
    return "unknown";
}

std::string_view default_nrd_provider_library_name() noexcept {
#if defined(_WIN32)
    return "fuse_nrdplugin_nri.dll";
#elif defined(__APPLE__)
    return "libfuse_nrdplugin_nri.dylib";
#else
    return "libfuse_nrdplugin_nri.so";
#endif
}

NrdPluginConfig resolve_nrd_plugin_config(const NrdPluginConfig& project) {
    NrdPluginConfig c = project;
    if (c.sdk_dir.empty()) {
        c.sdk_dir = env_or_empty("FUSE_NRD_SDK_DIR");
    }
    if (c.library_name.empty()) {
        c.library_name = env_or_empty("FUSE_NRD_PLUGIN_LIB");
    }
    if (c.options.empty()) {
        c.options = env_or_empty("FUSE_NRD_PLUGIN_OPTIONS");
    }
    return c;
}

NrdPlugin::~NrdPlugin() {
    if (m_ctx && m_api.shutdown) {
        m_api.shutdown(m_ctx);
    }
    m_ctx = nullptr;
    close_library(m_lib);
    m_lib = nullptr;
}

std::string_view NrdPlugin::provider_name() const noexcept {
    return m_api.provider_name ? std::string_view(m_api.provider_name) : std::string_view("unknown");
}

const FuseNrdDenoiserSupport& NrdPlugin::denoiser(FuseNrdDenoiser d) const noexcept {
    static const FuseNrdDenoiserSupport kNone{sizeof(FuseNrdDenoiserSupport), 0u, FUSE_NRD_ERR_UNSUPPORTED_DENOISER, 0u, 0u, 0u, 0u, 0u};
    return d < FUSE_NRD_DENOISER_COUNT ? m_denoisers[d] : kNone;
}

bool NrdPlugin::supports(FuseNrdDenoiser d) const noexcept {
    return d < FUSE_NRD_DENOISER_COUNT && m_denoisers[d].status == FUSE_NRD_OK;
}

u32 NrdPlugin::denoiser_mask() const noexcept {
    u32 m = 0;
    for (u32 d = 0; d < FUSE_NRD_DENOISER_COUNT; ++d) {
        m |= supports(d) ? FUSE_NRD_DENOISER_BIT(d) : 0u;
    }
    return m;
}

void* NrdPlugin::symbol(const char* name) const noexcept { return find_symbol(m_lib, name); }

bool NrdPluginLoader::enabled_at_build() noexcept {
#if defined(FUSE_NRD_PLUGIN_ENABLED) && FUSE_NRD_PLUGIN_ENABLED
    return true;
#else
    return false;
#endif
}

NrdLoadResult NrdPluginLoader::load(const NrdPluginConfig& config) {
    if (config.sdk_dir.empty()) {
        return fail(NrdPluginStatus::NotConfigured, "no NRD provider directory configured (project setting or FUSE_NRD_SDK_DIR)");
    }
    const std::string name = config.library_name.empty() ? std::string(default_nrd_provider_library_name()) : config.library_name;
    return load_file((fs::path(config.sdk_dir) / name).string(), config);
}

NrdLoadResult NrdPluginLoader::load_file(const std::string& library_path, const NrdPluginConfig& config) {
    std::error_code ec;
    if (library_path.empty() || !fs::is_regular_file(fs::path(library_path), ec)) {
        return fail(NrdPluginStatus::LibraryNotFound, "no NRD provider library at '" + library_path + "'");
    }
    std::string err;
    void* lib = open_library(library_path, err);
    if (!lib) {
        return fail(NrdPluginStatus::LoadFailed, library_path + ": " + err);
    }
    std::unique_ptr<NrdPlugin> p(new NrdPlugin());
    p->m_lib = lib;
    p->m_path = library_path;

    auto* get_api = reinterpret_cast<FuseNrdPluginGetApiFn>(find_symbol(lib, FUSE_NRD_PLUGIN_ENTRY_NAME));
    if (!get_api) {
        return fail(NrdPluginStatus::EntryPointMissing, library_path + ": no export '" FUSE_NRD_PLUGIN_ENTRY_NAME "'");
    }
    FuseNrdApi api{};
    api.struct_size = sizeof(FuseNrdApi);
    const FuseNrdStatus gs = get_api(FUSE_NRD_PLUGIN_ABI_VERSION, &api);
    const u32 provider_major = FUSE_NRD_PLUGIN_ABI_MAJOR_OF(api.abi_version);
    if (gs == FUSE_NRD_ERR_ABI_MISMATCH || provider_major != FUSE_NRD_PLUGIN_ABI_MAJOR) {
        return fail(NrdPluginStatus::AbiMismatch, library_path + ": provider ABI " + std::to_string(provider_major) + "." +
                                                      std::to_string(api.abi_version & 0xffffu) + ", host ABI " +
                                                      std::to_string(FUSE_NRD_PLUGIN_ABI_MAJOR) + "." +
                                                      std::to_string(FUSE_NRD_PLUGIN_ABI_MINOR));
    }
    if (gs != FUSE_NRD_OK || api.struct_size < kMinApiSize || !api.init || !api.shutdown || !api.query_denoiser ||
        !api.create_instance || !api.destroy_instance || !api.set_common_settings || !api.set_denoiser_settings ||
        !api.dispatch) {
        return fail(NrdPluginStatus::AbiMismatch, library_path + ": incomplete provider function table (" +
                                                      std::to_string(api.struct_size) + " bytes, status " + std::to_string(gs) + ")");
    }
    p->m_api = api;

    const std::string dir = fs::path(library_path).parent_path().string();
    FuseNrdInitInfo info{};
    info.struct_size = sizeof(info);
    info.host_abi_version = FUSE_NRD_PLUGIN_ABI_VERSION;
    info.graphics_api = config.graphics_api;
    info.instance = config.instance;
    info.physical_device = config.physical_device;
    info.device = config.device;
    info.runtime_dir = dir.c_str();
    info.options = config.options.c_str();
    FuseNrdContext* ctx = nullptr;
    const FuseNrdStatus is = api.init(&info, &ctx);
    if (is != FUSE_NRD_OK || !ctx) {
        if (ctx) {
            api.shutdown(ctx);
        }
        const FuseNrdStatus eff = is == FUSE_NRD_OK ? FUSE_NRD_ERR_RUNTIME_FAILURE : is;
        const char* text = api.status_string ? api.status_string(eff) : nullptr;
        return fail(status_from_init(eff), library_path + ": init failed, provider status " + std::to_string(eff) +
                                               (text ? std::string(" (") + text + ")" : std::string()));
    }
    p->m_ctx = ctx;
    for (u32 d = 0; d < FUSE_NRD_DENOISER_COUNT; ++d) {
        FuseNrdDenoiserSupport s{};
        s.struct_size = sizeof(s);
        const FuseNrdStatus qs = api.query_denoiser(ctx, d, &s);
        if (qs != FUSE_NRD_OK && s.status == FUSE_NRD_OK) {
            s.status = qs; // a failed query never reads as supported
        }
        s.denoiser = d;
        p->m_denoisers[d] = s;
    }
    if (p->denoiser_mask() == 0u) {
        return fail(NrdPluginStatus::NoDenoisers,
                    library_path + ": provider '" + std::string(p->provider_name()) + "' initialised but reports no usable denoiser");
    }
    NrdLoadResult r;
    r.status = NrdPluginStatus::Available;
    r.detail = library_path + ": provider '" + std::string(p->provider_name()) + "' NRD " +
               (api.runtime_version ? api.runtime_version : "?");
    r.plugin = std::move(p);
    return r;
}

NrdLoadResult NrdPluginLoader::probe(const NrdPluginConfig& project) {
    if (!enabled_at_build()) {
        return fail(NrdPluginStatus::DisabledAtBuild, "configure with -DFUSE_ENABLE_NRD_PLUGIN=ON to probe for an NRD provider");
    }
    return load(resolve_nrd_plugin_config(project));
}

// ---- mapping ----------------------------------------------------------------------------------------------------

FuseNrdDenoiser to_nrd_denoiser(DenoiseSignal signal, DenoiserMethod method) noexcept {
    const bool spec = signal == DenoiseSignal::Reflection;
    switch (method) {
        case DenoiserMethod::Sigma: return signal == DenoiseSignal::Shadow ? FUSE_NRD_DENOISER_SIGMA_SHADOW : FUSE_NRD_DENOISER_COUNT;
        case DenoiserMethod::Reblur:
            if (signal == DenoiseSignal::Shadow) {
                return FUSE_NRD_DENOISER_COUNT;
            }
            return spec ? FUSE_NRD_DENOISER_REBLUR_SPECULAR : FUSE_NRD_DENOISER_REBLUR_DIFFUSE;
        case DenoiserMethod::Relax:
            if (signal == DenoiseSignal::Shadow) {
                return FUSE_NRD_DENOISER_COUNT;
            }
            return spec ? FUSE_NRD_DENOISER_RELAX_SPECULAR : FUSE_NRD_DENOISER_RELAX_DIFFUSE;
        default: return FUSE_NRD_DENOISER_COUNT;
    }
}

FuseNrdSlot nrd_output_slot(FuseNrdDenoiser d) noexcept {
    switch (d) {
        case FUSE_NRD_DENOISER_SIGMA_SHADOW: return FUSE_NRD_OUT_SHADOW_TRANSLUCENCY;
        case FUSE_NRD_DENOISER_REBLUR_SPECULAR:
        case FUSE_NRD_DENOISER_RELAX_SPECULAR: return FUSE_NRD_OUT_SPEC_RADIANCE_HITDIST;
        default: return FUSE_NRD_OUT_DIFF_RADIANCE_HITDIST;
    }
}

FuseNrdCommonSettings map_common_settings(const NrdNativeFrame& frame, u32 width, u32 height, u32 frame_index, bool reset,
                                          const denoise::DenoiserSettings& settings) noexcept {
    FuseNrdCommonSettings c{};
    c.struct_size = sizeof(c);
    c.frame_index = frame_index;
    c.flags = reset ? FUSE_NRD_COMMON_RESET : 0u;
    if (frame.resources[FUSE_NRD_OUT_VALIDATION].native != 0u) {
        c.flags |= FUSE_NRD_COMMON_VALIDATION;
    }
    c.resource_width = width;
    c.resource_height = height;
    c.rect_width = frame.rect_width ? std::min(frame.rect_width, width) : width;
    c.rect_height = frame.rect_height ? std::min(frame.rect_height, height) : height;
    // Column-major column-vector on both sides: the 16 floats are copied as stored.
    std::memcpy(c.view_to_clip, frame.projection.data.data(), sizeof(c.view_to_clip));
    std::memcpy(c.view_to_clip_prev, frame.prev_projection.data.data(), sizeof(c.view_to_clip_prev));
    std::memcpy(c.world_to_view, frame.view.data.data(), sizeof(c.world_to_view));
    std::memcpy(c.world_to_view_prev, frame.prev_view.data.data(), sizeof(c.world_to_view_prev));
    c.motion_vector_scale[0] = -1.f; // FUSE uv_cur - uv_prev -> NRD prev = cur + mv * scale
    c.motion_vector_scale[1] = -1.f;
    c.motion_vector_scale[2] = 0.f;
    c.camera_jitter[0] = frame.jitter_px.x;
    c.camera_jitter[1] = frame.jitter_px.y;
    c.camera_jitter_prev[0] = frame.prev_jitter_px.x;
    c.camera_jitter_prev[1] = frame.prev_jitter_px.y;
    c.resolution_scale[0] = width ? static_cast<f32>(c.rect_width) / static_cast<f32>(width) : 1.f;
    c.resolution_scale[1] = height ? static_cast<f32>(c.rect_height) / static_cast<f32>(height) : 1.f;
    c.denoising_range = frame.denoising_range;
    c.disocclusion_threshold = settings.disocclusion_depth;
    c.time_delta_ms = frame.time_delta_ms;
    return c;
}

FuseNrdDenoiserSettings map_denoiser_settings(FuseNrdDenoiser d, const denoise::DenoiserSettings& settings) noexcept {
    FuseNrdDenoiserSettings s{};
    s.struct_size = sizeof(s);
    s.denoiser = d;
    const u32 frames = std::clamp(settings.max_history_frames, 1u, 63u); // NRD caps history at 63 frames
    s.max_accumulated_frames = d == FUSE_NRD_DENOISER_SIGMA_SHADOW ? 0u : frames;
    s.max_fast_accumulated_frames = d == FUSE_NRD_DENOISER_SIGMA_SHADOW ? 0u : std::max(1u, frames / 5u);
    s.anti_firefly = settings.anti_firefly ? 1u : 0u;
    s.atrous_iterations = 0u; // provider default
    // REBLUR hit-distance normalisation (NRD defaults A = 3, B = 0.1, C = 20); FUSE producers write world units.
    s.hit_distance_a = 3.f;
    s.hit_distance_b = 0.1f;
    s.hit_distance_c = 20.f;
    s.plane_distance_sensitivity = 0.f;
    return s;
}

// ---- NrdDenoiser ------------------------------------------------------------------------------------------------

NrdDenoiser::NrdDenoiser(std::shared_ptr<NrdPlugin> plugin) : m_plugin(std::move(plugin)), m_caps(nrd_denoiser_caps(m_plugin.get())) {
    m_settings.method = DenoiserMethod::Reblur;
}

NrdDenoiser::~NrdDenoiser() {
    if (m_plugin) {
        for (Retired& r : m_retired) {
            if (r.instance) {
                m_plugin->api().destroy_instance(m_plugin->context(), r.instance);
                r.instance = nullptr;
            }
        }
        if (m_instance) {
            m_plugin->api().destroy_instance(m_plugin->context(), m_instance);
        }
    }
    m_instance = nullptr;
}

bool NrdDenoiser::configure(const denoise::DenoiserSettings& settings) {
    const FuseNrdDenoiser d = to_nrd_denoiser(settings.signal, settings.method);
    if (!m_plugin || d == FUSE_NRD_DENOISER_COUNT || !m_plugin->supports(d)) {
        m_lastStatus = FUSE_NRD_ERR_UNSUPPORTED_DENOISER;
        return false;
    }
    if (d != m_denoiser) {
        m_pendingReset = true;
    }
    m_settings = settings;
    m_denoiser = d;
    m_settingsDirty = true;
    return true;
}

void NrdDenoiser::bindNativeFrame(const NrdNativeFrame& frame) {
    m_frame = frame;
    m_frameBound = true;
}

void NrdDenoiser::retireInstance() {
    if (!m_instance) {
        return;
    }
    for (Retired& r : m_retired) {
        if (!r.instance) {
            r.instance = m_instance;
            r.serial = m_instanceSerial;
            m_instance = nullptr;
            return;
        }
    }
    // Ring full: the oldest retired instance is destroyed now (the caller retires frames before resizing this often).
    m_plugin->api().destroy_instance(m_plugin->context(), m_retired[0].instance);
    m_retired[0].instance = m_instance;
    m_retired[0].serial = m_instanceSerial;
    m_instance = nullptr;
}

bool NrdDenoiser::beginFrame(u64 frameSerial, const denoise::DenoiseFrameDesc& frame) {
    m_frameBegun = false;
    m_bindingCount = 0;
    if (!m_plugin || m_denoiser == FUSE_NRD_DENOISER_COUNT) {
        m_lastStatus = m_plugin ? FUSE_NRD_ERR_UNSUPPORTED_DENOISER : FUSE_NRD_ERR_NOT_INITIALIZED;
        return false;
    }
    if (frame.width == 0u || frame.height == 0u) {
        m_lastStatus = FUSE_NRD_ERR_INVALID_ARGUMENT;
        return false;
    }
    if (!m_frameBound) {
        m_lastStatus = FUSE_NRD_ERR_MISSING_INPUT;
        return false;
    }
    m_serial = frameSerial;
    // Required inputs + the output slot must be bound.
    const FuseNrdDenoiserSupport& sup = m_plugin->denoiser(m_denoiser);
    const u32 required = sup.required_inputs | FUSE_NRD_SLOT_BIT(nrd_output_slot(m_denoiser));
    for (u32 s = 0; s < FUSE_NRD_SLOT_COUNT; ++s) {
        if ((required & FUSE_NRD_SLOT_BIT(s)) != 0u && m_frame.resources[s].native == 0u) {
            m_lastStatus = FUSE_NRD_ERR_MISSING_INPUT;
            return false;
        }
    }
    const FuseNrdApi& api = m_plugin->api();
    if (!m_instance || m_instanceDenoiser != m_denoiser || m_instanceW != frame.width || m_instanceH != frame.height) {
        retireInstance();
        FuseNrdInstance* inst = nullptr;
        const FuseNrdStatus cs = api.create_instance(m_plugin->context(), m_denoiser, frame.width, frame.height, &inst);
        if (cs != FUSE_NRD_OK || !inst) {
            m_lastStatus = cs == FUSE_NRD_OK ? FUSE_NRD_ERR_RUNTIME_FAILURE : cs;
            return false;
        }
        m_instance = inst;
        m_instanceDenoiser = m_denoiser;
        m_instanceW = frame.width;
        m_instanceH = frame.height;
        m_instanceSerial = frameSerial;
        m_pendingReset = true;
        m_settingsDirty = true;
        ++m_instancesCreated;
    }
    if (m_settingsDirty) {
        const FuseNrdDenoiserSettings ds = map_denoiser_settings(m_denoiser, m_settings);
        const FuseNrdStatus ss = api.set_denoiser_settings(m_instance, &ds);
        if (ss != FUSE_NRD_OK) {
            m_lastStatus = ss;
            return false;
        }
        m_settingsDirty = false;
    }
    m_common = map_common_settings(m_frame, frame.width, frame.height, m_frameIndex, m_pendingReset || frame.reset, m_settings);
    const FuseNrdStatus cs = api.set_common_settings(m_instance, &m_common);
    if (cs != FUSE_NRD_OK) {
        m_lastStatus = cs;
        return false;
    }
    for (u32 s = 0; s < FUSE_NRD_SLOT_COUNT; ++s) {
        if (m_frame.resources[s].native != 0u) {
            FuseNrdResourceBinding& b = m_bindings[m_bindingCount++];
            b.slot = s;
            b.reserved = 0u;
            b.resource = m_frame.resources[s];
        }
    }
    m_refs.fill(rg::TextureRef{});
    m_frameBegun = true;
    m_lastStatus = FUSE_NRD_OK;
    return true;
}

denoise::DenoiseGraphRefs NrdDenoiser::importInto(rg::Graph& graph) {
    denoise::DenoiseGraphRefs refs{};
    if (!m_frameBegun) {
        return refs;
    }
    static constexpr const char* kSlotNames[FUSE_NRD_SLOT_COUNT] = {
        "nrd.in.mv", "nrd.in.normal_roughness", "nrd.in.viewz", "nrd.in.diff", "nrd.in.spec", "nrd.in.penumbra",
        "nrd.out.diff", "nrd.out.spec", "nrd.out.shadow", "nrd.out.validation"};
    for (u32 i = 0; i < m_bindingCount; ++i) {
        const FuseNrdResourceBinding& b = m_bindings[i];
        rg::ImportedImage img{};
        img.image = reinterpret_cast<void*>(static_cast<uintptr_t>(b.resource.native));
        img.view = reinterpret_cast<void*>(static_cast<uintptr_t>(b.resource.view));
        img.format = b.resource.native_format;
        img.width = b.resource.width ? b.resource.width : m_instanceW;
        img.height = b.resource.height ? b.resource.height : m_instanceH;
        img.initialLayout = b.resource.layout;
        img.name = kSlotNames[b.slot];
        m_refs[b.slot] = graph.importImage(img);
    }
    refs.outputImage = m_refs[nrd_output_slot(m_denoiser)];
    return refs;
}

void NrdDenoiser::addPasses(rg::Graph& graph, const denoise::DenoiseGraphRefs& refs, const denoise::DenoiseGraphInputs& inputs) {
    (void)refs;
    (void)inputs; // NRD reads its own NRD-packed textures (bindNativeFrame), not the SVGF buffers
    if (!m_frameBegun) {
        return;
    }
    rg::PassBuilder pass = graph.addPass("denoise.nrd", &NrdDenoiser::record, this, rg::QueueClass::Graphics);
    for (u32 i = 0; i < m_bindingCount; ++i) {
        FuseNrdResourceBinding& b = m_bindings[i];
        if (!m_refs[b.slot].valid()) {
            continue;
        }
        pass.use(m_refs[b.slot], is_output(b.slot) ? rg::Access::ExternalWrite : rg::Access::ExternalRead);
        b.resource.layout = kVkImageLayoutGeneral; // the layout External* accesses leave the image in
    }
    pass.neverCull();
}

void NrdDenoiser::record(const rg::PassContext& context, void* user) {
    static_cast<NrdDenoiser*>(user)->dispatch(static_cast<u64>(reinterpret_cast<uintptr_t>(context.commandBuffer)));
}

FuseNrdStatus NrdDenoiser::dispatch(u64 commandBuffer) {
    if (!m_frameBegun || !m_instance) {
        m_lastStatus = FUSE_NRD_ERR_MISSING_SETTINGS;
        return m_lastStatus;
    }
    m_lastStatus = m_plugin->api().dispatch(m_instance, m_bindings.data(), m_bindingCount, commandBuffer);
    m_frameBegun = false;
    m_frameBound = false; // one bind per frame
    if (m_lastStatus == FUSE_NRD_OK) {
        m_pendingReset = false;
        m_instanceSerial = m_serial;
        ++m_frameIndex;
        ++m_dispatches;
    }
    return m_lastStatus;
}

u32 NrdDenoiser::collectRetired(u64 completedSerial) {
    u32 n = 0;
    for (Retired& r : m_retired) {
        if (r.instance && r.serial <= completedSerial) {
            m_plugin->api().destroy_instance(m_plugin->context(), r.instance);
            r.instance = nullptr;
            ++n;
        }
    }
    return n;
}

// ---- registration -------------------------------------------------------------------------------------------------

denoise::DenoiserCaps nrd_denoiser_caps(const NrdPlugin* plugin) {
    denoise::DenoiserCaps c{};
    c.name = kNrdDenoiserName;
    c.display_name = "NVIDIA Real-time Denoisers (NRD)";
    c.license = "LicenseRef-NVIDIA-RTX-SDKs"; // adapter MIT; the runtime is the developer's NRD
    c.needs_native_frame = true;
    c.gpu_only = true;
    c.in_tree = false;
    if (!plugin) {
        return c;
    }
    if (plugin->supports(FUSE_NRD_DENOISER_SIGMA_SHADOW)) {
        c.methods |= denoise::denoiser_method_bit(DenoiserMethod::Sigma);
        c.signals |= denoise::denoise_signal_bit(DenoiseSignal::Shadow);
    }
    const bool reblur = plugin->supports(FUSE_NRD_DENOISER_REBLUR_DIFFUSE) && plugin->supports(FUSE_NRD_DENOISER_REBLUR_SPECULAR);
    const bool relax = plugin->supports(FUSE_NRD_DENOISER_RELAX_DIFFUSE) && plugin->supports(FUSE_NRD_DENOISER_RELAX_SPECULAR);
    if (reblur) {
        c.methods |= denoise::denoiser_method_bit(DenoiserMethod::Reblur);
    }
    if (relax) {
        c.methods |= denoise::denoiser_method_bit(DenoiserMethod::Relax);
    }
    if (reblur || relax) {
        c.signals |= denoise::denoise_signal_bit(DenoiseSignal::Reflection) | denoise::denoise_signal_bit(DenoiseSignal::Gi);
        // radiance + hit distance packing (REBLUR normalises it; RELAX uses it for specular); SIGMA takes a penumbra
        c.hit_distance_methods = denoise::denoiser_method_bit(DenoiserMethod::Reblur) | denoise::denoiser_method_bit(DenoiserMethod::Relax);
    }
    return c;
}

bool register_nrd_denoiser(denoise::DenoiserRegistry& registry, std::shared_ptr<NrdPlugin> plugin) {
    if (!plugin) {
        return false;
    }
    const denoise::DenoiserCaps caps = nrd_denoiser_caps(plugin.get());
    if (caps.methods == 0u) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_pluginMutex);
        g_plugin = plugin;
    }
    if (!registry.register_backend(caps, &make_nrd)) {
        return false;
    }
    return true;
}

void unregister_nrd_denoiser(denoise::DenoiserRegistry& registry) {
    registry.unregister_backend(kNrdDenoiserName);
    std::lock_guard<std::mutex> lock(g_pluginMutex);
    g_plugin.reset();
}

std::shared_ptr<NrdPlugin> registered_nrd_plugin() { return current_plugin(); }

NrdLoadResult probe_and_register_nrd_denoiser(const NrdPluginConfig& project) {
    NrdLoadResult r = NrdPluginLoader::probe(project);
    if (r.status != NrdPluginStatus::Available || !r.plugin) {
        return r;
    }
    std::shared_ptr<NrdPlugin> shared(std::move(r.plugin));
    if (!register_nrd_denoiser(denoise::DenoiserRegistry::instance(), shared)) {
        r.status = NrdPluginStatus::NoDenoisers;
        r.detail += " (nothing registered)";
    }
    return r;
}

} // namespace fuse::renderer::nrd
