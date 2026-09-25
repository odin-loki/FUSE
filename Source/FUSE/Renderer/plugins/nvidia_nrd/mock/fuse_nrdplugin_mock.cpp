// Mock NRD provider (MIT, part of FUSE; WP-6.4b). Implements fuse_nrd_plugin_abi.h without NRD or a GPU so CI can
// gate the loader, denoiser discovery, parameter marshalling and failure paths. It validates on its own (required
// slots per denoiser, settings before dispatch), records every call and echoes them via fuse_nrd_mock_echo.h.
//
// Options ("k=v;k=v"): runtime=missing | api=d3d12 (reject Vulkan) | disable=sigma,reblur,relax
// Build variants (FUSE_NRDMOCK_VARIANT): 0 normal, 1 ABI major 2, 2 no entry point, 3 truncated table.

#include "fuse_nrd_mock_echo.h"

#include <fuse/renderer/nrd/fuse_nrd_plugin_abi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#ifndef FUSE_NRDMOCK_VARIANT
#define FUSE_NRDMOCK_VARIANT 0
#endif

struct FuseNrdContext {
    uint32_t disabled = 0; // FUSE_NRD_DENOISER_BIT set
};

struct FuseNrdInstance {
    FuseNrdContext* ctx = nullptr;
    FuseNrdDenoiser denoiser = 0;
    uint32_t width = 0, height = 0;
    bool has_settings = false;
    bool has_common = false;
    FuseNrdCommonSettings common{};
};

namespace {

std::mutex g_mutex;
FuseNrdMockEcho g_echo{};

constexpr uint32_t slot(uint32_t s) { return 1u << s; }

uint32_t required_inputs(FuseNrdDenoiser d) {
    const uint32_t base = slot(FUSE_NRD_IN_MV) | slot(FUSE_NRD_IN_NORMAL_ROUGHNESS) | slot(FUSE_NRD_IN_VIEWZ);
    switch (d) {
        case FUSE_NRD_DENOISER_SIGMA_SHADOW: return base | slot(FUSE_NRD_IN_PENUMBRA);
        case FUSE_NRD_DENOISER_REBLUR_DIFFUSE:
        case FUSE_NRD_DENOISER_RELAX_DIFFUSE: return base | slot(FUSE_NRD_IN_DIFF_RADIANCE_HITDIST);
        case FUSE_NRD_DENOISER_REBLUR_SPECULAR:
        case FUSE_NRD_DENOISER_RELAX_SPECULAR: return base | slot(FUSE_NRD_IN_SPEC_RADIANCE_HITDIST);
        default: return 0;
    }
}

uint32_t outputs(FuseNrdDenoiser d) {
    switch (d) {
        case FUSE_NRD_DENOISER_SIGMA_SHADOW: return slot(FUSE_NRD_OUT_SHADOW_TRANSLUCENCY);
        case FUSE_NRD_DENOISER_REBLUR_SPECULAR:
        case FUSE_NRD_DENOISER_RELAX_SPECULAR: return slot(FUSE_NRD_OUT_SPEC_RADIANCE_HITDIST);
        default: return slot(FUSE_NRD_OUT_DIFF_RADIANCE_HITDIST);
    }
}

bool option_has(const char* options, const char* needle) {
    return options != nullptr && std::strstr(options, needle) != nullptr;
}

uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

FuseNrdStatus mock_init(const FuseNrdInitInfo* info, FuseNrdContext** out_ctx) {
    if (!info || !out_ctx || info->struct_size < sizeof(FuseNrdInitInfo)) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    if (FUSE_NRD_PLUGIN_ABI_MAJOR_OF(info->host_abi_version) != FUSE_NRD_PLUGIN_ABI_MAJOR) {
        return FUSE_NRD_ERR_ABI_MISMATCH;
    }
    if (option_has(info->options, "runtime=missing")) {
        return FUSE_NRD_ERR_RUNTIME_MISSING;
    }
    if (option_has(info->options, "api=d3d12") && info->graphics_api == FUSE_NRD_API_VULKAN) {
        return FUSE_NRD_ERR_GRAPHICS_API;
    }
    auto* ctx = new FuseNrdContext();
    if (option_has(info->options, "sigma")) {
        ctx->disabled |= FUSE_NRD_DENOISER_BIT(FUSE_NRD_DENOISER_SIGMA_SHADOW);
    }
    if (option_has(info->options, "reblur")) {
        ctx->disabled |= FUSE_NRD_DENOISER_BIT(FUSE_NRD_DENOISER_REBLUR_DIFFUSE) | FUSE_NRD_DENOISER_BIT(FUSE_NRD_DENOISER_REBLUR_SPECULAR);
    }
    if (option_has(info->options, "relax")) {
        ctx->disabled |= FUSE_NRD_DENOISER_BIT(FUSE_NRD_DENOISER_RELAX_DIFFUSE) | FUSE_NRD_DENOISER_BIT(FUSE_NRD_DENOISER_RELAX_SPECULAR);
    }
    *out_ctx = ctx;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.init_calls;
    return FUSE_NRD_OK;
}

void mock_shutdown(FuseNrdContext* ctx) {
    delete ctx;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.shutdown_calls;
}

FuseNrdStatus mock_query(FuseNrdContext* ctx, FuseNrdDenoiser d, FuseNrdDenoiserSupport* out) {
    if (!ctx || !out) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    out->denoiser = d;
    if (d >= FUSE_NRD_DENOISER_COUNT || (ctx->disabled & FUSE_NRD_DENOISER_BIT(d)) != 0u) {
        out->status = FUSE_NRD_ERR_UNSUPPORTED_DENOISER;
        return FUSE_NRD_OK;
    }
    out->status = FUSE_NRD_OK;
    out->required_inputs = required_inputs(d);
    out->outputs = outputs(d);
    out->version_major = 0;
    out->version_minor = 0;
    out->version_patch = 1;
    return FUSE_NRD_OK;
}

FuseNrdStatus mock_create(FuseNrdContext* ctx, FuseNrdDenoiser d, uint32_t w, uint32_t h, FuseNrdInstance** out) {
    if (!ctx || !out || w == 0u || h == 0u || d >= FUSE_NRD_DENOISER_COUNT) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    if ((ctx->disabled & FUSE_NRD_DENOISER_BIT(d)) != 0u) {
        return FUSE_NRD_ERR_UNSUPPORTED_DENOISER;
    }
    auto* inst = new FuseNrdInstance();
    inst->ctx = ctx;
    inst->denoiser = d;
    inst->width = w;
    inst->height = h;
    *out = inst;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.create_calls;
    ++g_echo.live_instances;
    g_echo.last_create_denoiser = d;
    g_echo.last_create_width = w;
    g_echo.last_create_height = h;
    return FUSE_NRD_OK;
}

void mock_destroy(FuseNrdContext*, FuseNrdInstance* inst) {
    if (!inst) {
        return;
    }
    delete inst;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.destroy_calls;
    --g_echo.live_instances;
}

FuseNrdStatus mock_set_common(FuseNrdInstance* inst, const FuseNrdCommonSettings* s) {
    if (!inst || !s || s->struct_size < sizeof(FuseNrdCommonSettings)) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    if (s->resource_width != inst->width || s->resource_height != inst->height || s->rect_width > s->resource_width ||
        s->rect_height > s->resource_height) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    inst->common = *s;
    inst->has_common = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.common_calls;
    g_echo.last_common = *s;
    return FUSE_NRD_OK;
}

FuseNrdStatus mock_set_settings(FuseNrdInstance* inst, const FuseNrdDenoiserSettings* s) {
    if (!inst || !s || s->struct_size < sizeof(FuseNrdDenoiserSettings) || s->denoiser != inst->denoiser ||
        s->max_accumulated_frames > 63u) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    inst->has_settings = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.settings_calls;
    g_echo.last_settings = *s;
    return FUSE_NRD_OK;
}

FuseNrdStatus mock_dispatch(FuseNrdInstance* inst, const FuseNrdResourceBinding* b, uint32_t count, uint64_t cmd) {
    FuseNrdStatus st = FUSE_NRD_OK;
    uint32_t bound = 0;
    if (!inst || (count && !b) || count > FUSE_NRD_SLOT_COUNT) {
        st = FUSE_NRD_ERR_INVALID_ARGUMENT;
    } else if (!inst->has_common || !inst->has_settings) {
        st = FUSE_NRD_ERR_MISSING_SETTINGS;
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            if (b[i].slot < FUSE_NRD_SLOT_COUNT && b[i].resource.native != 0u) {
                bound |= slot(b[i].slot);
            }
        }
        const uint32_t need = required_inputs(inst->denoiser) | outputs(inst->denoiser);
        if ((bound & need) != need) {
            st = FUSE_NRD_ERR_MISSING_INPUT;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.dispatch_calls;
    g_echo.last_dispatch_status = st;
    g_echo.last_command_buffer = cmd;
    if (st == FUSE_NRD_OK) {
        g_echo.last_binding_count = count;
        std::memset(g_echo.last_bindings, 0, sizeof(g_echo.last_bindings));
        std::memcpy(g_echo.last_bindings, b, sizeof(FuseNrdResourceBinding) * count);
        g_echo.last_dispatch_denoiser = inst->denoiser;
        uint64_t h = 1469598103934665603ull;
        h = fnv(h, &inst->denoiser, sizeof(inst->denoiser));
        h = fnv(h, &inst->common.frame_index, sizeof(uint32_t));
        h = fnv(h, &inst->common.flags, sizeof(uint32_t));
        h = fnv(h, &inst->width, sizeof(uint32_t));
        h = fnv(h, &inst->height, sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i) {
            h = fnv(h, &b[i].slot, sizeof(uint32_t));
            h = fnv(h, &b[i].resource.native, sizeof(uint64_t));
        }
        g_echo.output_signature = h;
        inst->has_common = false; // common settings are per frame
    }
    return st;
}

const char* mock_status_string(FuseNrdStatus s) {
    switch (s) {
        case FUSE_NRD_OK: return "ok";
        case FUSE_NRD_ERR_ABI_MISMATCH: return "abi mismatch";
        case FUSE_NRD_ERR_UNSUPPORTED_DENOISER: return "unsupported denoiser";
        case FUSE_NRD_ERR_RUNTIME_MISSING: return "NRD runtime missing (mock)";
        case FUSE_NRD_ERR_GRAPHICS_API: return "graphics API not supported (mock)";
        case FUSE_NRD_ERR_INVALID_ARGUMENT: return "invalid argument";
        case FUSE_NRD_ERR_MISSING_INPUT: return "missing input";
        case FUSE_NRD_ERR_MISSING_SETTINGS: return "missing settings";
        default: return "error";
    }
}

} // namespace

extern "C" {

#if FUSE_NRDMOCK_VARIANT == 2
// Exports the table under a misspelt name: the loader must report EntryPointMissing.
#define FUSE_NRDMOCK_ENTRY fuseNrdPluginGetApiMisspelt
#else
#define FUSE_NRDMOCK_ENTRY fuseNrdPluginGetApi
#endif
FUSE_NRD_PLUGIN_EXPORT FuseNrdStatus FUSE_NRDMOCK_ENTRY(uint32_t host_abi_version, FuseNrdApi* out) {
    if (!out) {
        return FUSE_NRD_ERR_INVALID_ARGUMENT;
    }
    FuseNrdApi api{};
    api.struct_size = sizeof(FuseNrdApi);
#if FUSE_NRDMOCK_VARIANT == 1
    api.abi_version = (2u << 16); // a provider from a future, incompatible ABI
#else
    api.abi_version = FUSE_NRD_PLUGIN_ABI_VERSION;
#endif
    api.provider_name = "mock";
    api.runtime_version = "mock-0.0.1";
    api.init = &mock_init;
    api.shutdown = &mock_shutdown;
    api.query_denoiser = &mock_query;
    api.create_instance = &mock_create;
    api.destroy_instance = &mock_destroy;
    api.set_common_settings = &mock_set_common;
    api.set_denoiser_settings = &mock_set_settings;
    api.dispatch = &mock_dispatch;
    api.status_string = &mock_status_string;
#if FUSE_NRDMOCK_VARIANT == 3
    api.struct_size = uint32_t(offsetof(FuseNrdApi, set_common_settings)); // truncated table
    api.set_common_settings = nullptr;
    api.set_denoiser_settings = nullptr;
    api.dispatch = nullptr;
    api.status_string = nullptr;
#endif
    const uint32_t host_size = out->struct_size ? out->struct_size : uint32_t(sizeof(FuseNrdApi));
    std::memcpy(out, &api, host_size < sizeof(FuseNrdApi) ? host_size : sizeof(FuseNrdApi));
#if FUSE_NRDMOCK_VARIANT == 1
    (void)host_abi_version;
    return FUSE_NRD_OK; // lies: the host must still reject on api.abi_version
#else
    return FUSE_NRD_PLUGIN_ABI_MAJOR_OF(host_abi_version) == FUSE_NRD_PLUGIN_ABI_MAJOR ? FUSE_NRD_OK : FUSE_NRD_ERR_ABI_MISMATCH;
#endif
}

FUSE_NRD_PLUGIN_EXPORT void fuseNrdMockEcho(FuseNrdMockEcho* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (out) {
        *out = g_echo;
    }
}

FUSE_NRD_PLUGIN_EXPORT void fuseNrdMockReset(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t live = g_echo.live_instances;
    g_echo = FuseNrdMockEcho{};
    g_echo.live_instances = live;
}

} // extern "C"
