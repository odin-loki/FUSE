#include <fuse/renderer/nvidia/nv_plugin_loader.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace fuse::renderer::nvidia {

namespace {

namespace fs = std::filesystem;

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read at start-up only
    return v ? std::string(v) : std::string();
}

void* open_library(const std::string& path, std::string& error) {
#if defined(_WIN32)
    const std::wstring wide = fs::path(path).wstring();
    // Search the provider's own directory for its dependencies (sl.interposer.dll, nvngx_*.dll),
    // never the CWD / PATH first.
    HMODULE h = LoadLibraryExW(wide.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
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

PluginStatus status_from_init(FuseNvStatus s) {
    switch (s) {
        case FUSE_NV_ERR_ABI_MISMATCH: return PluginStatus::AbiMismatch;
        case FUSE_NV_ERR_RUNTIME_MISSING: return PluginStatus::RuntimeMissing;
        case FUSE_NV_ERR_NO_NVIDIA_GPU: return PluginStatus::NoNvidiaGpu;
        case FUSE_NV_ERR_DRIVER_TOO_OLD: return PluginStatus::DriverTooOld;
        default: return PluginStatus::InitFailed;
    }
}

std::string provider_status(const FuseNvApi& api, FuseNvStatus s) {
    const char* text = api.status_string ? api.status_string(s) : nullptr;
    return "provider status " + std::to_string(s) + (text ? std::string(" (") + text + ")" : std::string());
}

LoadResult fail(PluginStatus status, std::string detail) {
    LoadResult r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

// Minimum table a v1 host needs: everything up to and including status_string.
constexpr u32 kMinApiSize = u32(offsetof(FuseNvApi, status_string) + sizeof(FuseNvApi::status_string));

} // namespace

std::string_view plugin_status_name(PluginStatus s) noexcept {
    switch (s) {
        case PluginStatus::Available: return "available";
        case PluginStatus::DisabledAtBuild: return "disabled at build (FUSE_ENABLE_NVIDIA_PLUGIN=OFF)";
        case PluginStatus::NotConfigured: return "not configured (set FUSE_NVIDIA_SDK_DIR)";
        case PluginStatus::LibraryNotFound: return "provider library not found";
        case PluginStatus::LoadFailed: return "provider library failed to load";
        case PluginStatus::EntryPointMissing: return "provider entry point missing";
        case PluginStatus::AbiMismatch: return "provider ABI mismatch";
        case PluginStatus::InitFailed: return "provider init failed";
        case PluginStatus::RuntimeMissing: return "NVIDIA runtime missing";
        case PluginStatus::NoNvidiaGpu: return "no NVIDIA RTX GPU";
        case PluginStatus::DriverTooOld: return "NVIDIA driver too old";
        case PluginStatus::NoFeatures: return "no supported NVIDIA features";
    }
    return "unknown";
}

std::string_view default_provider_library_name() noexcept {
#if defined(_WIN32)
    return "fuse_nvplugin_streamline.dll";
#elif defined(__APPLE__)
    return "libfuse_nvplugin_ngx.dylib"; // no NVIDIA runtime exists; kept for a uniform "not found"
#else
    return "libfuse_nvplugin_ngx.so";
#endif
}

PluginConfig resolve_plugin_config(const PluginConfig& project) {
    PluginConfig c = project;
    if (c.sdk_dir.empty()) {
        c.sdk_dir = env_or_empty("FUSE_NVIDIA_SDK_DIR");
    }
    if (c.library_name.empty()) {
        c.library_name = env_or_empty("FUSE_NVIDIA_PLUGIN_LIB");
    }
    if (c.options.empty()) {
        c.options = env_or_empty("FUSE_NVIDIA_PLUGIN_OPTIONS");
    }
    return c;
}

NvPlugin::~NvPlugin() {
    if (ctx_ && api_.shutdown) {
        api_.shutdown(ctx_);
    }
    ctx_ = nullptr;
    close_library(lib_);
    lib_ = nullptr;
}

std::string_view NvPlugin::provider_name() const noexcept {
    return api_.provider_name ? std::string_view(api_.provider_name) : std::string_view("unknown");
}

const FuseNvFeatureSupport& NvPlugin::feature(FuseNvFeature f) const noexcept {
    static const FuseNvFeatureSupport kNone{sizeof(FuseNvFeatureSupport), 0u, FUSE_NV_ERR_UNSUPPORTED_FEATURE, 0u, 0u, 0u, 0u, 0u};
    return f < FUSE_NV_FEATURE_COUNT ? features_[f] : kNone;
}

bool NvPlugin::supports(FuseNvFeature f) const noexcept {
    return f < FUSE_NV_FEATURE_COUNT && features_[f].status == FUSE_NV_OK;
}

u32 NvPlugin::feature_mask() const noexcept {
    u32 m = 0;
    for (u32 f = 0; f < FUSE_NV_FEATURE_COUNT; ++f) {
        m |= supports(f) ? FUSE_NV_FEATURE_BIT(f) : 0u;
    }
    return m;
}

void* NvPlugin::symbol(const char* name) const noexcept {
    return find_symbol(lib_, name);
}

bool NvPluginLoader::enabled_at_build() noexcept {
#if defined(FUSE_NVIDIA_PLUGIN_ENABLED) && FUSE_NVIDIA_PLUGIN_ENABLED
    return true;
#else
    return false;
#endif
}

LoadResult NvPluginLoader::load(const PluginConfig& config) {
    if (config.sdk_dir.empty()) {
        return fail(PluginStatus::NotConfigured, "no NVIDIA SDK directory configured (project setting or FUSE_NVIDIA_SDK_DIR)");
    }
    const std::string name = config.library_name.empty() ? std::string(default_provider_library_name()) : config.library_name;
    return load_file((fs::path(config.sdk_dir) / name).string(), config);
}

LoadResult NvPluginLoader::load_file(const std::string& library_path, const PluginConfig& config) {
    std::error_code ec;
    if (library_path.empty() || !fs::is_regular_file(fs::path(library_path), ec)) {
        return fail(PluginStatus::LibraryNotFound, "no provider library at '" + library_path + "'");
    }
    std::string err;
    void* lib = open_library(library_path, err);
    if (!lib) {
        return fail(PluginStatus::LoadFailed, library_path + ": " + err);
    }
    // From here on the NvPlugin owns the handle; early returns unload it via ~NvPlugin.
    std::unique_ptr<NvPlugin> p(new NvPlugin());
    p->lib_ = lib;
    p->path_ = library_path;

    auto* get_api = reinterpret_cast<FuseNvPluginGetApiFn>(find_symbol(lib, FUSE_NV_PLUGIN_ENTRY_NAME));
    if (!get_api) {
        return fail(PluginStatus::EntryPointMissing, library_path + ": no export '" FUSE_NV_PLUGIN_ENTRY_NAME "'");
    }
    FuseNvApi api{};
    api.struct_size = sizeof(FuseNvApi);
    const FuseNvStatus gs = get_api(FUSE_NV_PLUGIN_ABI_VERSION, &api);
    const u32 provider_major = FUSE_NV_PLUGIN_ABI_MAJOR_OF(api.abi_version);
    if (gs == FUSE_NV_ERR_ABI_MISMATCH || provider_major != FUSE_NV_PLUGIN_ABI_MAJOR) {
        return fail(PluginStatus::AbiMismatch, library_path + ": provider ABI " + std::to_string(provider_major) + "." +
                                                   std::to_string(api.abi_version & 0xffffu) + ", host ABI " +
                                                   std::to_string(FUSE_NV_PLUGIN_ABI_MAJOR) + "." +
                                                   std::to_string(FUSE_NV_PLUGIN_ABI_MINOR));
    }
    if (gs != FUSE_NV_OK || api.struct_size < kMinApiSize || !api.init || !api.shutdown || !api.query_feature ||
        !api.get_adapter_info || !api.get_render_size || !api.set_tags || !api.set_constants || !api.evaluate) {
        return fail(PluginStatus::AbiMismatch, library_path + ": incomplete provider function table (" +
                                                   std::to_string(api.struct_size) + " bytes, status " + std::to_string(gs) + ")");
    }
    p->api_ = api;

    const std::string dir = fs::path(library_path).parent_path().string();
    FuseNvInitInfo info{};
    info.struct_size = sizeof(FuseNvInitInfo);
    info.host_abi_version = FUSE_NV_PLUGIN_ABI_VERSION;
    info.graphics_api = config.graphics_api;
    info.application_id = config.application_id;
    info.instance = config.instance;
    info.physical_device = config.physical_device;
    info.device = config.device;
    info.runtime_dir = dir.c_str();
    info.options = config.options.c_str();
    FuseNvContext* ctx = nullptr;
    const FuseNvStatus is = api.init(&info, &ctx);
    if (is != FUSE_NV_OK || !ctx) {
        if (ctx) {
            api.shutdown(ctx);
        }
        return fail(status_from_init(is == FUSE_NV_OK ? FUSE_NV_ERR_RUNTIME_FAILURE : is),
                    library_path + ": init failed, " + provider_status(api, is));
    }
    p->ctx_ = ctx;

    p->adapter_.struct_size = sizeof(FuseNvAdapterInfo);
    if (api.get_adapter_info(ctx, &p->adapter_) != FUSE_NV_OK) {
        p->adapter_ = FuseNvAdapterInfo{};
    }
    for (u32 f = 0; f < FUSE_NV_FEATURE_COUNT; ++f) {
        FuseNvFeatureSupport s{};
        s.struct_size = sizeof(FuseNvFeatureSupport);
        const FuseNvStatus qs = api.query_feature(ctx, f, &s);
        if (qs != FUSE_NV_OK && s.status == FUSE_NV_OK) {
            s.status = qs; // a failed query never reads as "supported"
        }
        s.feature = f;
        p->features_[f] = s;
    }
    if (p->feature_mask() == 0u) {
        return fail(PluginStatus::NoFeatures, library_path + ": provider '" + std::string(p->provider_name()) +
                                                  "' initialised but reports no supported feature");
    }

    LoadResult r;
    r.status = PluginStatus::Available;
    r.detail = library_path + ": provider '" + std::string(p->provider_name()) + "' runtime " +
               (api.runtime_version ? api.runtime_version : "?");
    r.plugin = std::move(p);
    return r;
}

LoadResult NvPluginLoader::probe(const PluginConfig& project) {
    if (!enabled_at_build()) {
        return fail(PluginStatus::DisabledAtBuild, "configure with -DFUSE_ENABLE_NVIDIA_PLUGIN=ON to probe for NVIDIA runtimes");
    }
    return load(resolve_plugin_config(project));
}

} // namespace fuse::renderer::nvidia
