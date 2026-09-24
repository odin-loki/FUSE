// Mock NVIDIA provider (MIT, part of FUSE). Implements the FUSE NVIDIA provider C ABI
// (fuse_nv_plugin_abi.h) without any NVIDIA code or GPU, so CI can gate the loader, feature
// detection, backend registration, input mapping and failure paths.
//
// It validates independently of the host (its own required-tag table, modelled on Streamline's
// eErrorMissingInputParameter / eErrorMissingConstants behaviour) and echoes what it received through
// the test-only exports in fuse_nv_mock_echo.h.
//
// Options ("k=v;k=v", from FuseNvInitInfo::options):
//   gpu=rtx50|rtx40|rtx30|rtx20|none   adapter generation (default rtx40)
//   driver=MAJOR.MINOR                  driver version (default 580.0; < 525 => DRIVER_TOO_OLD)
//   runtime=missing                     simulate an absent NVIDIA runtime (interposer / NGX)
//   disable=sr,rr,fg,nr,reflex          force features off
//
// Build variants (FUSE_NVMOCK_VARIANT): 0 normal, 1 reports ABI major 2, 2 exports no entry
// point, 3 returns a truncated function table.

#include "fuse_nv_mock_echo.h"

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#ifndef FUSE_NVMOCK_VARIANT
#define FUSE_NVMOCK_VARIANT 0
#endif

struct FuseNvContext {
    FuseNvGraphicsApi api = FUSE_NV_API_NONE;
    uint32_t generation = 40;
    uint32_t driver_major = 580, driver_minor = 0;
    uint32_t disabled_mask = 0;
    struct Frame {
        uint32_t tag_mask = 0;
        FuseNvResourceTag tags[FUSE_NV_BUFFER_KIND_COUNT] = {};
        bool has_constants = false;
        FuseNvConstants constants{};
    };
    std::map<std::pair<uint32_t, uint32_t>, Frame> frames; // (frame_index, viewport)
};

namespace {

std::mutex g_mutex;
FuseNvMockEcho g_echo{};
std::atomic<uint32_t> g_live{0};

constexpr uint32_t bit(uint32_t k) { return 1u << k; }

uint32_t required_mask(FuseNvFeature f) {
    const uint32_t sr = bit(FUSE_NV_BUFFER_COLOR_IN) | bit(FUSE_NV_BUFFER_COLOR_OUT) | bit(FUSE_NV_BUFFER_DEPTH) |
                        bit(FUSE_NV_BUFFER_MOTION_VECTORS);
    switch (f) {
        case FUSE_NV_FEATURE_DLSS_SR: return sr;
        case FUSE_NV_FEATURE_DLSS_RR:
            return sr | bit(FUSE_NV_BUFFER_DIFFUSE_ALBEDO) | bit(FUSE_NV_BUFFER_SPECULAR_ALBEDO) | bit(FUSE_NV_BUFFER_NORMALS) |
                   bit(FUSE_NV_BUFFER_ROUGHNESS) | bit(FUSE_NV_BUFFER_SPECULAR_HIT_DISTANCE);
        case FUSE_NV_FEATURE_DLSS_FG:
            return bit(FUSE_NV_BUFFER_DEPTH) | bit(FUSE_NV_BUFFER_MOTION_VECTORS) | bit(FUSE_NV_BUFFER_HUDLESS_COLOR);
        case FUSE_NV_FEATURE_DLSS_NR: return bit(FUSE_NV_BUFFER_COLOR_IN) | bit(FUSE_NV_BUFFER_MOTION_VECTORS);
        default: return 0;
    }
}

uint32_t min_generation(FuseNvFeature f) {
    switch (f) {
        case FUSE_NV_FEATURE_DLSS_SR:
        case FUSE_NV_FEATURE_DLSS_RR:
        case FUSE_NV_FEATURE_REFLEX: return 20;
        case FUSE_NV_FEATURE_DLSS_FG: return 40;
        case FUSE_NV_FEATURE_DLSS_NR: return 50;
        default: return 0xffffffffu;
    }
}

std::string option(const std::string& opts, const std::string& key) {
    size_t pos = 0;
    while (pos <= opts.size()) {
        const size_t end = std::min(opts.find(';', pos), opts.size());
        const std::string kv = opts.substr(pos, end - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }
        pos = end + 1;
    }
    return {};
}

const char* mock_status_string(FuseNvStatus s) {
    switch (s) {
        case FUSE_NV_OK: return "ok";
        case FUSE_NV_ERR_ABI_MISMATCH: return "abi mismatch";
        case FUSE_NV_ERR_NOT_INITIALIZED: return "not initialized";
        case FUSE_NV_ERR_UNSUPPORTED_FEATURE: return "unsupported feature";
        case FUSE_NV_ERR_NO_NVIDIA_GPU: return "no NVIDIA GPU (mock)";
        case FUSE_NV_ERR_DRIVER_TOO_OLD: return "driver too old (mock)";
        case FUSE_NV_ERR_RUNTIME_MISSING: return "runtime missing (mock)";
        case FUSE_NV_ERR_INVALID_ARGUMENT: return "invalid argument";
        case FUSE_NV_ERR_MISSING_INPUT: return "missing input";
        case FUSE_NV_ERR_MISSING_CONSTANTS: return "missing constants";
        case FUSE_NV_ERR_INVALID_CONSTANTS: return "invalid constants";
        case FUSE_NV_ERR_GRAPHICS_API: return "graphics api";
        default: return "runtime failure";
    }
}

FuseNvStatus feature_status(const FuseNvContext* ctx, FuseNvFeature f) {
    if (f >= FUSE_NV_FEATURE_COUNT) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    if (ctx->disabled_mask & bit(f)) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    return ctx->generation >= min_generation(f) ? FUSE_NV_OK : FUSE_NV_ERR_UNSUPPORTED_FEATURE;
}

FuseNvStatus mock_init(const FuseNvInitInfo* info, FuseNvContext** out_ctx) {
    if (!info || !out_ctx || info->struct_size < sizeof(FuseNvInitInfo)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    *out_ctx = nullptr;
    if (FUSE_NV_PLUGIN_ABI_MAJOR_OF(info->host_abi_version) != FUSE_NV_PLUGIN_ABI_MAJOR) {
        return FUSE_NV_ERR_ABI_MISMATCH;
    }
    if (info->graphics_api > FUSE_NV_API_D3D12) {
        return FUSE_NV_ERR_GRAPHICS_API;
    }
    const std::string opts = info->options ? info->options : "";
    if (option(opts, "runtime") == "missing") {
        return FUSE_NV_ERR_RUNTIME_MISSING;
    }
    const std::string gpu = option(opts, "gpu");
    uint32_t generation = 40;
    if (gpu == "none") {
        return FUSE_NV_ERR_NO_NVIDIA_GPU;
    } else if (gpu == "rtx50") {
        generation = 50;
    } else if (gpu == "rtx30") {
        generation = 30;
    } else if (gpu == "rtx20") {
        generation = 20;
    }
    uint32_t dmaj = 580, dmin = 0;
    const std::string driver = option(opts, "driver");
    if (!driver.empty() && std::sscanf(driver.c_str(), "%u.%u", &dmaj, &dmin) < 1) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    if (dmaj < 525) {
        return FUSE_NV_ERR_DRIVER_TOO_OLD;
    }
    uint32_t disabled = 0;
    const std::string dis = "," + option(opts, "disable") + ",";
    const char* names[FUSE_NV_FEATURE_COUNT] = {",sr,", ",rr,", ",fg,", ",nr,", ",reflex,"};
    for (uint32_t f = 0; f < FUSE_NV_FEATURE_COUNT; ++f) {
        disabled |= dis.find(names[f]) != std::string::npos ? bit(f) : 0u;
    }
    auto* ctx = new FuseNvContext();
    ctx->api = info->graphics_api;
    ctx->generation = generation;
    ctx->driver_major = dmaj;
    ctx->driver_minor = dmin;
    ctx->disabled_mask = disabled;
    // DLSS-G requires Reflex (Streamline: "It is required for sl.reflex to be integrated").
    if (ctx->disabled_mask & bit(FUSE_NV_FEATURE_REFLEX)) {
        ctx->disabled_mask |= bit(FUSE_NV_FEATURE_DLSS_FG);
    }
    *out_ctx = ctx;
    g_live.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_echo = FuseNvMockEcho{};
    g_echo.init_count = 1;
    g_echo.last_missing = FUSE_NV_BUFFER_KIND_COUNT;
    g_echo.graphics_api = info->graphics_api;
    return FUSE_NV_OK;
}

void mock_shutdown(FuseNvContext* ctx) {
    if (ctx) {
        delete ctx;
        g_live.fetch_sub(1);
    }
}

FuseNvStatus mock_adapter(FuseNvContext* ctx, FuseNvAdapterInfo* out) {
    if (!ctx || !out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const uint32_t size = out->struct_size;
    *out = FuseNvAdapterInfo{};
    out->struct_size = size;
    out->vendor_id = 0x10DE;
    out->device_id = 0xF00D;
    out->rtx_generation = ctx->generation;
    out->driver_major = ctx->driver_major;
    out->driver_minor = ctx->driver_minor;
    std::snprintf(out->name, sizeof(out->name), "FUSE Mock RTX %u", ctx->generation);
    return FUSE_NV_OK;
}

FuseNvStatus mock_query(FuseNvContext* ctx, FuseNvFeature f, FuseNvFeatureSupport* out) {
    if (!ctx || !out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    const uint32_t size = out->struct_size;
    *out = FuseNvFeatureSupport{};
    out->struct_size = size;
    out->feature = f;
    out->status = feature_status(ctx, f);
    out->min_rtx_generation = f < FUSE_NV_FEATURE_COUNT ? min_generation(f) : 0u;
    out->version_major = 0; // "mock" runtime
    out->version_minor = 1;
    return FUSE_NV_OK;
}

FuseNvStatus mock_render_size(FuseNvContext* ctx, FuseNvFeature f, FuseNvQuality q, uint32_t dw, uint32_t dh,
                              FuseNvRenderSize* out) {
    if (!ctx || !out || dw == 0 || dh == 0 || q >= FUSE_NV_QUALITY_COUNT) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    if (f != FUSE_NV_FEATURE_DLSS_SR && f != FUSE_NV_FEATURE_DLSS_RR) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    if (feature_status(ctx, f) != FUSE_NV_OK) {
        return FUSE_NV_ERR_UNSUPPORTED_FEATURE;
    }
    // Published DLSS per-axis scale factors: DLAA 1.0, Quality 1/1.5, Balanced 0.58, Perf 0.5, UP 1/3,
    // Ultra Quality 0.77.
    static const double kScale[FUSE_NV_QUALITY_COUNT] = {1.0, 1.0 / 1.5, 0.58, 0.5, 1.0 / 3.0, 0.77};
    const uint32_t size = out->struct_size;
    *out = FuseNvRenderSize{};
    out->struct_size = size;
    out->render_width = uint32_t(double(dw) * kScale[q] + 0.5);
    out->render_height = uint32_t(double(dh) * kScale[q] + 0.5);
    out->min_width = uint32_t(double(dw) / 3.0 + 0.5);
    out->min_height = uint32_t(double(dh) / 3.0 + 0.5);
    out->max_width = dw;
    out->max_height = dh;
    return FUSE_NV_OK;
}

void trim_frames(FuseNvContext* ctx, uint32_t newest) {
    // Keep a small window like Streamline's frame-token ring; drop anything 8+ frames old.
    for (auto it = ctx->frames.begin(); it != ctx->frames.end();) {
        it = (newest >= it->first.first && newest - it->first.first >= 8u) ? ctx->frames.erase(it) : std::next(it);
    }
}

FuseNvStatus mock_set_tags(FuseNvContext* ctx, uint32_t frame, uint32_t viewport, const FuseNvResourceTag* tags,
                           uint32_t count, uint64_t /*cmd*/) {
    if (!ctx || (count && !tags)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (tags[i].kind >= FUSE_NV_BUFFER_KIND_COUNT || tags[i].resource.native == 0u ||
            tags[i].lifecycle > FUSE_NV_LIFECYCLE_ONLY_VALID_NOW) {
            return FUSE_NV_ERR_INVALID_ARGUMENT;
        }
    }
    FuseNvContext::Frame& fr = ctx->frames[{frame, viewport}];
    for (uint32_t i = 0; i < count; ++i) {
        fr.tags[tags[i].kind] = tags[i];
        fr.tag_mask |= bit(tags[i].kind);
    }
    trim_frames(ctx, frame);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.set_tags_count;
    g_echo.pending_frames = uint32_t(ctx->frames.size());
    return FUSE_NV_OK;
}

FuseNvStatus mock_set_constants(FuseNvContext* ctx, uint32_t viewport, const FuseNvConstants* c) {
    if (!ctx || !c || c->struct_size < sizeof(FuseNvConstants)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    FuseNvContext::Frame& fr = ctx->frames[{c->frame_index, viewport}];
    fr.constants = *c;
    fr.has_constants = true;
    trim_frames(ctx, c->frame_index);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.set_constants_count;
    g_echo.pending_frames = uint32_t(ctx->frames.size());
    return FUSE_NV_OK;
}

FuseNvStatus mock_evaluate(FuseNvContext* ctx, uint32_t frame, uint32_t viewport, const FuseNvFeatureOptions* o,
                           uint64_t /*cmd*/) {
    if (!ctx || !o || o->struct_size < sizeof(FuseNvFeatureOptions)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto reject = [&](FuseNvStatus s) {
        ++g_echo.rejected_count;
        g_echo.last_status = s;
        return s;
    };
    g_echo.last_feature = o->feature;
    g_echo.last_frame_index = frame;
    g_echo.last_viewport = viewport;
    g_echo.last_missing = FUSE_NV_BUFFER_KIND_COUNT;
    if (feature_status(ctx, o->feature) != FUSE_NV_OK || o->feature == FUSE_NV_FEATURE_REFLEX) {
        return reject(FUSE_NV_ERR_UNSUPPORTED_FEATURE);
    }
    const auto it = ctx->frames.find({frame, viewport});
    if (it == ctx->frames.end() || !it->second.has_constants) {
        return reject(FUSE_NV_ERR_MISSING_CONSTANTS);
    }
    const FuseNvContext::Frame& fr = it->second;
    g_echo.last_tag_mask = fr.tag_mask;
    const uint32_t missing = required_mask(o->feature) & ~fr.tag_mask;
    if (missing) {
        uint32_t k = 0;
        while (!(missing & bit(k))) {
            ++k;
        }
        g_echo.last_missing = k;
        return reject(FUSE_NV_ERR_MISSING_INPUT);
    }
    const FuseNvConstants& c = fr.constants;
    if (c.mvec_scale[0] == 0.0f || c.mvec_scale[1] == 0.0f || c.render_width == 0u || c.render_height == 0u) {
        return reject(FUSE_NV_ERR_INVALID_CONSTANTS);
    }
    if (o->feature == FUSE_NV_FEATURE_DLSS_FG && (o->frames_to_generate < 1u || o->frames_to_generate > 5u ||
                                                  (o->frames_to_generate > 1u && ctx->generation < 50u))) {
        return reject(FUSE_NV_ERR_INVALID_ARGUMENT); // multi-frame generation is RTX 50 only
    }
    ++g_echo.evaluate_count;
    g_echo.last_status = FUSE_NV_OK;
    g_echo.last_flags = c.flags;
    g_echo.last_render_width = c.render_width;
    g_echo.last_render_height = c.render_height;
    g_echo.last_output_width = c.output_width;
    g_echo.last_output_height = c.output_height;
    g_echo.last_jitter_px[0] = c.jitter_offset_px[0];
    g_echo.last_jitter_px[1] = c.jitter_offset_px[1];
    g_echo.last_mvec_scale[0] = c.mvec_scale[0];
    g_echo.last_mvec_scale[1] = c.mvec_scale[1];
    g_echo.last_pre_exposure = c.pre_exposure;
    g_echo.last_quality = o->quality;
    g_echo.last_frames_to_generate = o->frames_to_generate;
    g_echo.last_nr_structure_intensity = o->nr_structure_intensity;
    g_echo.last_nr_tone_intensity = o->nr_tone_intensity;
    g_echo.last_color_in_native = fr.tags[FUSE_NV_BUFFER_COLOR_IN].resource.native;
    return FUSE_NV_OK;
}

} // namespace

extern "C" {

#if FUSE_NVMOCK_VARIANT == 2
// Exports the table under a misspelt name: the loader must report EntryPointMissing.
#define FUSE_NVMOCK_ENTRY fuseNvPluginGetApiMisspelt
#else
#define FUSE_NVMOCK_ENTRY fuseNvPluginGetApi
#endif
FUSE_NV_PLUGIN_EXPORT FuseNvStatus FUSE_NVMOCK_ENTRY(uint32_t host_abi_version, FuseNvApi* out) {
    if (!out) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    FuseNvApi api{};
    api.struct_size = sizeof(FuseNvApi);
#if FUSE_NVMOCK_VARIANT == 1
    api.abi_version = (2u << 16) | 0u; // a provider from a future, incompatible ABI
#else
    api.abi_version = FUSE_NV_PLUGIN_ABI_VERSION;
#endif
    api.provider_name = "mock";
    api.runtime_version = "0.1.0-mock";
    api.init = mock_init;
    api.shutdown = mock_shutdown;
    api.get_adapter_info = mock_adapter;
    api.query_feature = mock_query;
    api.get_render_size = mock_render_size;
    api.set_tags = mock_set_tags;
    api.set_constants = mock_set_constants;
    api.evaluate = mock_evaluate;
    api.status_string = mock_status_string;
#if FUSE_NVMOCK_VARIANT == 3
    api.struct_size = uint32_t(offsetof(FuseNvApi, set_tags)); // truncated table
    api.set_tags = nullptr;
    api.set_constants = nullptr;
    api.evaluate = nullptr;
    api.status_string = nullptr;
#endif
    const uint32_t host_size = out->struct_size ? out->struct_size : uint32_t(sizeof(FuseNvApi));
    std::memcpy(out, &api, host_size < sizeof(FuseNvApi) ? host_size : sizeof(FuseNvApi));
#if FUSE_NVMOCK_VARIANT == 1
    (void)host_abi_version;
    return FUSE_NV_OK; // lies: the host must still reject on api.abi_version
#else
    return FUSE_NV_PLUGIN_ABI_MAJOR_OF(host_abi_version) == FUSE_NV_PLUGIN_ABI_MAJOR ? FUSE_NV_OK : FUSE_NV_ERR_ABI_MISMATCH;
#endif
}

FUSE_NV_PLUGIN_EXPORT void fuseNvMockGetEcho(FuseNvMockEcho* out) {
    if (out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        *out = g_echo;
    }
}

FUSE_NV_PLUGIN_EXPORT uint32_t fuseNvMockLiveContexts(void) {
    return g_live.load();
}

} // extern "C"
