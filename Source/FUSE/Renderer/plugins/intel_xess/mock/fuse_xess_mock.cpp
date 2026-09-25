// Mock libxess (MIT, part of FUSE; WP-4.3). Exports the XeSS entry points FUSE resolves (xess_api.h) without any Intel
// code or GPU, so CI can gate discovery, load failures, parameter marshalling and the upscaler's call sequence. It
// validates like the documented runtime (context required, init before execute, input <= output, jitter in range),
// records every call and echoes them through fuse_xess_mock_echo.h. Output is a deterministic signature.
//
// Build variants (FUSE_XESSMOCK_VARIANT): 0 normal (reports XeSS 1.3.0), 1 reports version 0.9.0 (unsupported),
// 2 lacks xessVKExecute.

#include "fuse_xess_mock_echo.h"

#include <fuse/renderer/xess/xess_api.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#ifndef FUSE_XESSMOCK_VARIANT
#define FUSE_XESSMOCK_VARIANT 0
#endif

#if defined(_WIN32)
#define FUSE_XESSMOCK_EXPORT __declspec(dllexport)
#else
#define FUSE_XESSMOCK_EXPORT __attribute__((visibility("default")))
#endif

struct FuseXessContext_T {
    bool initialised = false;
    FuseXess2d output{0u, 0u};
    FuseXessQuality quality = 0;
};

namespace {

std::mutex g_mutex;
FuseXessMockEcho g_echo{};
FuseXessMockControl g_control{};

float ratio(FuseXessQuality q) {
    switch (q) {
        case FUSE_XESS_QUALITY_ULTRA_PERFORMANCE: return 3.0f;
        case FUSE_XESS_QUALITY_PERFORMANCE: return 2.3f;
        case FUSE_XESS_QUALITY_BALANCED: return 2.0f;
        case FUSE_XESS_QUALITY_QUALITY: return 1.7f;
        case FUSE_XESS_QUALITY_ULTRA_QUALITY: return 1.5f;
        case FUSE_XESS_QUALITY_ULTRA_QUALITY_PLUS: return 1.3f;
        case FUSE_XESS_QUALITY_AA: return 1.0f;
        default: return 0.f;
    }
}

uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

} // namespace

extern "C" {

FUSE_XESSMOCK_EXPORT FuseXessResult xessGetVersion(FuseXessVersion* v) {
    if (!v) {
        return FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT;
    }
#if FUSE_XESSMOCK_VARIANT == 1
    *v = FuseXessVersion{0u, 9u, 0u, 0u};
#else
    *v = FuseXessVersion{1u, 3u, 0u, 0u};
#endif
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessVKCreateContext(void* instance, void* physical_device, void* device, FuseXessContext* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.create_calls;
    g_echo.last_instance = instance;
    g_echo.last_physical_device = physical_device;
    g_echo.last_device = device;
    if (!out || !instance || !physical_device || !device) {
        return FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT;
    }
    if (g_control.create_result < 0) {
        *out = nullptr;
        return g_control.create_result;
    }
    *out = new FuseXessContext_T();
    ++g_echo.live_contexts;
    return g_control.create_result;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessDestroyContext(FuseXessContext ctx) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    delete ctx;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.destroy_calls;
    --g_echo.live_contexts;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessVKBuildPipelines(FuseXessContext ctx, uint64_t, bool blocking, uint32_t flags) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.build_calls;
    g_echo.last_build_flags = flags;
    g_echo.last_build_blocking = blocking ? 1u : 0u;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessVKInit(FuseXessContext ctx, const FuseXessVkInitParams* p) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.init_calls;
    if (!p || p->output_resolution.x == 0u || p->output_resolution.y == 0u || ratio(p->quality_setting) == 0.f) {
        return FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT;
    }
    g_echo.last_init = *p;
    if (g_control.init_result < 0) {
        ctx->initialised = false;
        return g_control.init_result;
    }
    ctx->initialised = true;
    ctx->output = p->output_resolution;
    ctx->quality = p->quality_setting;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessGetInputResolution(FuseXessContext ctx, const FuseXess2d* out, FuseXessQuality q, FuseXess2d* in) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    const float r = ratio(q);
    if (!out || !in || r == 0.f) {
        return FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT;
    }
    in->x = static_cast<uint32_t>(std::lround(static_cast<double>(out->x) / r));
    in->y = static_cast<uint32_t>(std::lround(static_cast<double>(out->y) / r));
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.input_resolution_calls;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessSetVelocityScale(FuseXessContext ctx, float x, float y) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.velocity_calls;
    g_echo.velocity_scale[0] = x;
    g_echo.velocity_scale[1] = y;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessSetJitterScale(FuseXessContext ctx, float x, float y) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.jitter_scale_calls;
    g_echo.jitter_scale[0] = x;
    g_echo.jitter_scale[1] = y;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT FuseXessResult xessSetExposureMultiplier(FuseXessContext ctx, float s) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.exposure_multiplier_calls;
    g_echo.exposure_multiplier = s;
    return FUSE_XESS_RESULT_SUCCESS;
}

#if FUSE_XESSMOCK_VARIANT == 2
// Exported under a misspelt name: the loader must report EntryPointMissing naming xessVKExecute.
#define FUSE_XESSMOCK_EXECUTE xessVKExecuteMisspelt
#else
#define FUSE_XESSMOCK_EXECUTE xessVKExecute
#endif
FUSE_XESSMOCK_EXPORT FuseXessResult FUSE_XESSMOCK_EXECUTE(FuseXessContext ctx, void* cmd, const FuseXessVkExecuteParams* p) {
    if (!ctx) {
        return FUSE_XESS_RESULT_ERROR_INVALID_CONTEXT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.execute_calls;
    if (!ctx->initialised) {
        return FUSE_XESS_RESULT_ERROR_UNINITIALIZED;
    }
    if (!p || !cmd || p->color_texture.image == 0u || p->velocity_texture.image == 0u || p->depth_texture.image == 0u ||
        p->output_texture.image == 0u || p->input_width == 0u || p->input_height == 0u ||
        p->input_width > ctx->output.x || p->input_height > ctx->output.y || std::fabs(p->jitter_offset_x) > 0.5f ||
        std::fabs(p->jitter_offset_y) > 0.5f || p->output_texture.width != ctx->output.x ||
        p->output_texture.height != ctx->output.y) {
        return FUSE_XESS_RESULT_ERROR_INVALID_ARGUMENT;
    }
    if (g_control.execute_result < 0) {
        return g_control.execute_result;
    }
    g_echo.last_execute = *p;
    g_echo.last_command_buffer = cmd;
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &ctx->output, sizeof(ctx->output));
    h = fnv(h, &ctx->quality, sizeof(ctx->quality));
    h = fnv(h, &p->input_width, sizeof(uint32_t));
    h = fnv(h, &p->input_height, sizeof(uint32_t));
    h = fnv(h, &p->jitter_offset_x, sizeof(float));
    h = fnv(h, &p->jitter_offset_y, sizeof(float));
    h = fnv(h, &p->reset_history, sizeof(uint32_t));
    h = fnv(h, &p->color_texture.image, sizeof(uint64_t));
    h = fnv(h, &p->output_texture.image, sizeof(uint64_t));
    g_echo.output_signature = h;
    return FUSE_XESS_RESULT_SUCCESS;
}

FUSE_XESSMOCK_EXPORT void fuseXessMockEcho(FuseXessMockEcho* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (out) {
        *out = g_echo;
    }
}

FUSE_XESSMOCK_EXPORT void fuseXessMockControl(const FuseXessMockControl* control) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_control = control ? *control : FuseXessMockControl{};
}

FUSE_XESSMOCK_EXPORT void fuseXessMockReset(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t live = g_echo.live_contexts;
    g_echo = FuseXessMockEcho{};
    g_echo.live_contexts = live;
    g_control = FuseXessMockControl{};
}

} // extern "C"
