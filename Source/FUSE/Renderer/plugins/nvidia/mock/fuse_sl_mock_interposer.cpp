// Mock NVIDIA Streamline interposer (MIT, part of FUSE; test-only). Exports the subset of the
// Streamline core C API the FUSE Streamline provider calls, with the exact signatures of the vendored
// v2.14.1 headers, and records every argument so CI can check the provider's Streamline calls without
// Windows, an NVIDIA GPU or any NVIDIA binary. It implements no NVIDIA functionality.
//
// Adapter model comes from env FUSE_SL_MOCK_GPU: rtx50 | rtx40 (default) | rtx20 | none.

#include "fuse_sl_mock_echo.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
FuseSlMockEcho g_echo{};
bool g_initialized = false;

struct MockToken final : sl::FrameToken {
    uint32_t value = 0;
    operator uint32_t() const override { return value; }
};
MockToken g_tokens[8];
uint32_t g_token_counter = 0;

uint32_t generation() {
    const char* v = std::getenv("FUSE_SL_MOCK_GPU"); // NOLINT(concurrency-mt-unsafe): test-only
    const std::string s = v ? v : "rtx40";
    if (s == "none") {
        return 0;
    }
    if (s == "rtx50") {
        return 50;
    }
    if (s == "rtx20") {
        return 20;
    }
    return 40;
}

int32_t boolean(sl::Boolean b) { return static_cast<int32_t>(b); }

sl::Result mock_dlss_optimal(const sl::DLSSOptions& o, sl::DLSSOptimalSettings& s) {
    static const double kScale[] = {1.0, 0.5, 0.58, 1.0 / 1.5, 1.0 / 3.0, 0.77, 1.0}; // indexed by sl::DLSSMode
    const auto m = static_cast<uint32_t>(o.mode);
    if (m == 0 || m >= 7) {
        return sl::Result::eErrorInvalidParameter;
    }
    s.optimalRenderWidth = uint32_t(o.outputWidth * kScale[m] + 0.5);
    s.optimalRenderHeight = uint32_t(o.outputHeight * kScale[m] + 0.5);
    s.renderWidthMin = o.outputWidth / 3;
    s.renderHeightMin = o.outputHeight / 3;
    s.renderWidthMax = o.outputWidth;
    s.renderHeightMax = o.outputHeight;
    return sl::Result::eOk;
}

sl::Result mock_dlssd_optimal(const sl::DLSSDOptions& o, sl::DLSSDOptimalSettings& s) {
    sl::DLSSOptions tmp{};
    tmp.mode = o.mode;
    tmp.outputWidth = o.outputWidth;
    tmp.outputHeight = o.outputHeight;
    sl::DLSSOptimalSettings st{};
    const sl::Result r = mock_dlss_optimal(tmp, st);
    s.optimalRenderWidth = st.optimalRenderWidth;
    s.optimalRenderHeight = st.optimalRenderHeight;
    s.renderWidthMin = st.renderWidthMin;
    s.renderHeightMin = st.renderHeightMin;
    s.renderWidthMax = st.renderWidthMax;
    s.renderHeightMax = st.renderHeightMax;
    return r;
}

sl::Result mock_dlss_set_options(const sl::ViewportHandle&, const sl::DLSSOptions& o) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.dlss_options_calls;
    g_echo.last_dlss_mode = static_cast<uint32_t>(o.mode);
    g_echo.last_dlss_output_w = o.outputWidth;
    g_echo.last_dlss_output_h = o.outputHeight;
    g_echo.last_dlss_hdr = boolean(o.colorBuffersHDR);
    g_echo.last_dlss_auto_exposure = boolean(o.useAutoExposure);
    g_echo.last_dlss_pre_exposure = o.preExposure;
    return sl::Result::eOk;
}

sl::Result mock_dlssd_set_options(const sl::ViewportHandle&, const sl::DLSSDOptions& o) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.dlssd_options_calls;
    g_echo.last_dlssd_world_to_view_30 = o.worldToCameraView.row[3].x;
    return sl::Result::eOk;
}

sl::Result mock_dlssg_set_options(const sl::ViewportHandle&, const sl::DLSSGOptions& o) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.dlssg_options_calls;
    g_echo.last_dlssg_mode = static_cast<uint32_t>(o.mode);
    g_echo.last_dlssg_frames = o.numFramesToGenerate;
    return sl::Result::eOk;
}

} // namespace

extern "C" {

SL_API sl::Result slInit(const sl::Preferences& pref, uint64_t sdkVersion) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_echo = FuseSlMockEcho{};
    ++g_echo.init_count;
    g_echo.sdk_version = sdkVersion;
    g_echo.render_api = static_cast<uint32_t>(pref.renderAPI);
    g_echo.preference_flags = static_cast<uint64_t>(pref.flags);
    g_echo.num_features_to_load = pref.numFeaturesToLoad;
    g_echo.num_plugin_paths = pref.numPathsToPlugins;
    g_echo.application_id = pref.applicationId;
    if (sdkVersion != sl::kSDKVersion) {
        return sl::Result::eErrorInvalidParameter;
    }
    g_initialized = true;
    return sl::Result::eOk;
}

SL_API sl::Result slShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_echo.shutdown_count;
    g_initialized = false;
    return sl::Result::eOk;
}

SL_API sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo&) {
    const uint32_t gen = generation();
    if (!g_initialized) {
        return sl::Result::eErrorInitNotCalled;
    }
    if (gen == 0) {
        return sl::Result::eErrorNoSupportedAdapterFound;
    }
    uint32_t need = 0xffffffffu;
    if (feature == sl::kFeatureDLSS || feature == sl::kFeatureDLSS_RR || feature == sl::kFeatureReflex ||
        feature == sl::kFeaturePCL) {
        need = 20;
    } else if (feature == sl::kFeatureDLSS_G) {
        need = 40;
    } else if (feature == sl::kFeatureDLSS_NR) {
        need = 50;
    }
    return gen >= need ? sl::Result::eOk : sl::Result::eErrorFeatureNotSupported;
}

SL_API sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex) {
    std::lock_guard<std::mutex> lock(g_mutex);
    MockToken& t = g_tokens[g_token_counter++ % 8u];
    t.value = frameIndex ? *frameIndex : g_token_counter;
    token = &t;
    return sl::Result::eOk;
}

SL_API sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                   const sl::ResourceTag* resources, uint32_t numResources, sl::CommandBuffer*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        return sl::Result::eErrorInitNotCalled;
    }
    ++g_echo.set_tag_calls;
    g_echo.last_tag_frame = frame;
    g_echo.last_tag_viewport = viewport;
    g_echo.last_tag_count = numResources;
    for (uint32_t i = 0; i < numResources && i < FUSE_SL_MOCK_MAX_TAGS; ++i) {
        g_echo.last_tag_types[i] = resources[i].type;
        g_echo.last_tag_native[i] = resources[i].resource ? uint64_t(reinterpret_cast<uintptr_t>(resources[i].resource->native)) : 0u;
        g_echo.last_tag_width[i] = resources[i].resource ? resources[i].resource->width : 0u;
    }
    return sl::Result::eOk;
}

SL_API sl::Result slSetConstants(const sl::Constants& c, const sl::FrameToken& frame, const sl::ViewportHandle&) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        return sl::Result::eErrorInitNotCalled;
    }
    // Streamline treats eInvalid booleans as missing constants.
    if (c.depthInverted == sl::Boolean::eInvalid || c.cameraMotionIncluded == sl::Boolean::eInvalid ||
        c.motionVectors3D == sl::Boolean::eInvalid || c.reset == sl::Boolean::eInvalid) {
        return sl::Result::eErrorMissingConstants;
    }
    ++g_echo.set_constants_calls;
    g_echo.last_constants_frame = frame;
    g_echo.last_jitter[0] = c.jitterOffset.x;
    g_echo.last_jitter[1] = c.jitterOffset.y;
    g_echo.last_mvec_scale[0] = c.mvecScale.x;
    g_echo.last_mvec_scale[1] = c.mvecScale.y;
    g_echo.last_depth_inverted = boolean(c.depthInverted);
    g_echo.last_reset = boolean(c.reset);
    g_echo.last_camera_motion_included = boolean(c.cameraMotionIncluded);
    g_echo.last_view_to_clip_00 = c.cameraViewToClip.row[0].x;
    g_echo.last_prev_clip_to_clip_30 = c.prevClipToClip.row[3].x;
    g_echo.last_clip_to_lens_clip_11 = c.clipToLensClip.row[1].y;
    return sl::Result::eOk;
}

SL_API sl::Result slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs,
                                    uint32_t numInputs, sl::CommandBuffer*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        return sl::Result::eErrorInitNotCalled;
    }
    ++g_echo.evaluate_calls;
    g_echo.last_eval_feature = feature;
    g_echo.last_eval_frame = frame;
    g_echo.last_eval_input_count = numInputs;
    g_echo.last_eval_local_tag_count = 0;
    bool has_viewport = false;
    for (uint32_t i = 0; i < numInputs; ++i) {
        if (!inputs[i]) {
            return sl::Result::eErrorInvalidParameter;
        }
        if (inputs[i]->structType == sl::ViewportHandle::s_structType) {
            has_viewport = true;
        } else if (inputs[i]->structType == sl::ResourceTag::s_structType &&
                   g_echo.last_eval_local_tag_count < FUSE_SL_MOCK_MAX_TAGS) {
            const auto* tag = static_cast<const sl::ResourceTag*>(inputs[i]);
            g_echo.last_eval_local_tag_types[g_echo.last_eval_local_tag_count++] = tag->type;
        }
    }
    return has_viewport ? sl::Result::eOk : sl::Result::eErrorMissingInputParameter;
}

SL_API sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) {
    function = nullptr;
    const std::string name = functionName ? functionName : "";
    if (feature == sl::kFeatureDLSS && name == "slDLSSGetOptimalSettings") {
        function = reinterpret_cast<void*>(&mock_dlss_optimal);
    } else if (feature == sl::kFeatureDLSS && name == "slDLSSSetOptions") {
        function = reinterpret_cast<void*>(&mock_dlss_set_options);
    } else if (feature == sl::kFeatureDLSS_RR && name == "slDLSSDGetOptimalSettings") {
        function = reinterpret_cast<void*>(&mock_dlssd_optimal);
    } else if (feature == sl::kFeatureDLSS_RR && name == "slDLSSDSetOptions") {
        function = reinterpret_cast<void*>(&mock_dlssd_set_options);
    } else if (feature == sl::kFeatureDLSS_G && name == "slDLSSGSetOptions") {
        function = reinterpret_cast<void*>(&mock_dlssg_set_options);
    }
    return function ? sl::Result::eOk : sl::Result::eErrorMissingOrInvalidAPI;
}

SL_API sl::Result slSetD3DDevice(void* device) {
    return device ? sl::Result::eOk : sl::Result::eErrorInvalidParameter;
}

#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void fuseSlMockGetEcho(FuseSlMockEcho* out) {
    if (out) {
        std::lock_guard<std::mutex> lock(g_mutex);
        *out = g_echo;
    }
}

} // extern "C"
