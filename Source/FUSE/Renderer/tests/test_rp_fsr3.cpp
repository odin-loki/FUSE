// WP-4.2 Lavapipe gates for the FSR 3.1 backend (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). CPU gates: test_rp_fsr3_cpu.cpp.
//
// Scenes: the deterministic CPU reference sequences (upscale/reference_scene.hpp) — jittered render-resolution
// frames with every temporal input (colour, linear depth, UV motion incl. dynamic objects and sky, reactive /
// transparency masks, Halton jitter, mip bias) and a 16-spp (4x4 stratified) native ground truth with object ids.
// Display 192 x 108, render 128 x 72 (1.5x). FSR 3.1 runs through the "fsr3" ITemporalUpscaler adapter (Fsr3Gpu on a
// render graph), TAAU is the renderer's native TaauUpscaler (== TaauGpu within 2e-5, WP-4.1).
//
//   --mode quality      ThinFastObject (static camera, thin rod at ~10 px / frame: ghosting) and Showcase (strafing
//                       camera, disocclusion, particles), 32 frames, metrics over frames 12..31 in display space
//                       (exposure, clamp, sRGB): PSNR / SSIM / FLIP vs the 16-spp truth, ghosting = mean |luma error|
//                       on the rod's 6-frame trail, sharpness = sum |grad luma| / the truth's (1 = as sharp),
//                       temporal PSNR; FSR 3.1 without and with RCAS (0.6). Gates (kGate*, measured values next to
//                       them): finite; FSR3 better than bilinear; regression floors / ceilings; ghosting <= 1.5x TAAU;
//                       RCAS sharpness >= 0.95x TAAU; PSNR within 5 dB of TAAU; jitter-sign control: FSR fed +jitter
//                       (the wrong sign) must lose clearly (PSNR and sharpness), which pins the FSR <-> FUSE sign.
//                       Writes FUSE_FSR3_REPORT_PATH (JSON).
//   --mode determinism  the same 16-frame sequence through two fresh backends (and through the Slang and the GLSL
//                       convert kernel): outputs bit-identical every frame.
//   --mode reset        camera cut (Showcase -> MoireFlight) with reset_history: the first post-cut frame matches a
//                       fresh backend's first frame (PSNR within 0.25 dB, display values within 1e-3; measured: bit
//                       identical) and beats the same cut without reset (history of the old scene) by >= 1 dB; history generation / accumulated frames /
//                       Fsr3Gpu reset stats follow; invalidate_history() behaves like reset_history.
//   --mode switch       UpscalerRegistry with the built-ins + register_fsr3_backend: 60 frames of Showcase switching
//                       native_taau -> fsr1 -> nis -> fsr3 every 5 frames (instances kept, temporal ones invalidated on
//                       re-entry): no NaN / Inf, FLIP vs the 16-spp truth per backend reported and bounded.
//   --mode zero_alloc   80 frames (16 warm-up) of TaauGpu (bindless) + Fsr3Gpu on one render graph fed by the same
//                       inputs: 0 operator-new calls in Fsr3Gpu::beginFrame, the graph build (imports + passes) and the
//                       fsr3.* pass callbacks (validated run first; validation off for the count), 0 layout conflicts.
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer for the TAAU pass next to the
//                       FSR passes' classic descriptor sets (buffer: skip if absent)
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, passes not built).
#include "test_rp_temporal_common.hpp"

#include <fuse/renderer/quality/image_metrics.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/taa/taau.hpp>
#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/renderer/upscale/reference_scene.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_gpu.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_upscaler.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

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
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::fsr3;
namespace rs = fuse::renderer::refscene;
namespace q = fuse::renderer::quality;
namespace up = fuse::renderer::upscale;
namespace kernel = fuse::kernel;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;
using fuse::math::Vec4;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kDisplayW = 192;
constexpr u32 kDisplayH = 108;
constexpr f32 kRatio = 1.5f;
constexpr u32 kGtSamples = 4; // 4 x 4 = 16 spp
constexpr u32 kFrames = 32;
constexpr u32 kWarmup = 12;
constexpr u32 kTrailFrames = 6;

// Quality gates. Measured on Lavapipe (Mesa 25.2.8, Debug and Release identical: the passes are deterministic):
//                     PSNR    FLIP    ghost   sharpness      TAAU: PSNR  FLIP    ghost   sharpness
//   ThinFastObject    27.32   0.0821  0.0346  0.831  (RCAS 0.898)  31.69 0.0428  0.0264  0.918
//   Showcase          26.52   0.0796  0.0427  0.775  (RCAS 0.849)  27.29 0.0644  0.0363  0.818
// FSR 3.1 sits between bilinear and the native TAAU on these small, close-range synthetic scenes (its rectification
// tightens the clip box for pixels nearer than ~15 m and for low accumulation; the native TAAU is tuned on exactly
// these sequences). The gates therefore pin (a) the hard requirements — finite, better than bilinear, the jitter
// sign — and (b) regression floors / ceilings a little outside the measured values, plus a documented band
// against TAAU.
constexpr f32 kRcasSharpness = 0.6f;
struct SceneGate {
    f64 minPsnr, maxFlip, maxGhost, minSharpness, minSharpnessRcas;
};
constexpr SceneGate kGateThin{26.8, 0.090, 0.040, 0.80, 0.87};
constexpr SceneGate kGateShowcase{26.0, 0.088, 0.048, 0.74, 0.82};
constexpr f64 kGatePsnrBandVsTaau = 5.0;   ///< FSR3 PSNR >= TAAU PSNR - this (dB)
constexpr f64 kGateGhostVsTaau = 1.5;      ///< FSR3 trail error <= TAAU trail error * this
constexpr f64 kGateRcasSharpVsTaau = 0.95; ///< FSR3 + RCAS sharpness >= TAAU sharpness * this
constexpr f64 kGateSignPsnr = 1.0;         ///< correct jitter sign beats the flipped one by this (dB)

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                     data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

// --- Vulkan context ------------------------------------------------------------------------------------
struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    BindlessDescriptors bindless;
    u64 serial = 0;

    Fsr3BackendBinding binding() { return Fsr3BackendBinding{device.get(), allocator.get(), executor.get()}; }

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        if (device != nullptr) {
            bindless.collectRetired(~0ull);
            bindless.destroy(*device);
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

int setup(Context& ctx, bool descriptorBuffer, bool validation) {
    if (validation) {
        if (!layerAvailable(kValidationLayer)) {
            std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
            return kSkip;
        }
        setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        setenv("VK_INSTANCE_LAYERS", "", 1);
    }
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_fsr3";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr && validation) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    if (createMessenger != nullptr) {
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    if (descriptorBuffer && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const Fsr3Capabilities caps = queryFsr3Capabilities(ctx.device.get());
    if (!caps.supported) {
        std::printf("SKIP: FSR 3.1 unsupported: %s\n", caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

// --- display-space helpers -------------------------------------------------------------------------------
f32 srgb(f32 v) {
    v = std::clamp(v, 0.f, 1.f);
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

Vec3 display(const Vec3& linear, f32 exposure) {
    return Vec3(srgb(linear.x * exposure), srgb(linear.y * exposure), srgb(linear.z * exposure));
}

void toDisplay(const std::vector<Vec3>& in, f32 exposure, std::vector<Vec3>& out) {
    out.resize(in.size());
    for (usize i = 0; i < in.size(); ++i) {
        out[i] = display(in[i], exposure);
    }
}

f64 luma(const Vec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

/// sum |grad luma| over the pixels where mask != 0 (central differences, interior pixels).
f64 gradientEnergy(const std::vector<Vec3>& img, u32 w, u32 h, const std::vector<u8>& mask) {
    f64 sum = 0.0;
    for (u32 y = 1; y + 1 < h; ++y) {
        for (u32 x = 1; x + 1 < w; ++x) {
            const u32 i = y * w + x;
            if (mask[i] == 0u) {
                continue;
            }
            const f64 gx = luma(img[i + 1]) - luma(img[i - 1]);
            const f64 gy = luma(img[i + w]) - luma(img[i - w]);
            sum += std::sqrt(gx * gx + gy * gy);
        }
    }
    return sum;
}

UpscaleResolution resolution() { return makeUpscaleResolution(kDisplayW, kDisplayH, kRatio); }

struct GroundTruth {
    std::vector<rs::GroundTruthFrame> frames;
    std::vector<std::vector<Vec3>> displayed;
};

bool renderTruth(const rs::ReferenceScene& scene, u32 frames, f32 exposure, GroundTruth& gt) {
    gt.frames.resize(frames);
    gt.displayed.resize(frames);
    for (u32 f = 0; f < frames; ++f) {
        if (!scene.renderGroundTruth(f, kDisplayW, kDisplayH, kGtSamples, gt.frames[f])) {
            return false;
        }
        toDisplay(gt.frames[f].color, exposure, gt.displayed[f]);
    }
    return true;
}

// --- quality mode --------------------------------------------------------------------------------------
enum Method : u32 { kFsr3 = 0, kFsr3Rcas = 1, kFsr3FlippedJitter = 2, kTaau = 3, kBilinear = 4, kMethodCount = 5 };
const char* kMethodNames[kMethodCount] = {"FSR 3.1", "FSR 3.1 + RCAS", "FSR 3.1 (+jitter)", "TAAU", "bilinear"};

struct Scores {
    f64 psnr = 0.0, ssim = 0.0, flip = 0.0, tpsnr = 0.0, ghost = 0.0, sharpness = 0.0;
    bool finite = true;
};

bool evaluateScene(Context& ctx, rs::ReferenceSceneKind kind, Scores out[kMethodCount], std::string& json) {
    const rs::ReferenceScene scene(kind);
    const UpscaleResolution res = resolution();
    const usize dn = static_cast<usize>(kDisplayW) * kDisplayH;
    rs::ReferenceFrame frame{};
    if (!scene.renderFrame(0, res, frame)) {
        return false;
    }
    const f32 exposure = frame.exposure;
    GroundTruth gt;
    if (!renderTruth(scene, kFrames, exposure, gt)) {
        return false;
    }
    Fsr3TemporalUpscaler fsr(ctx.binding());
    Fsr3TemporalUpscaler fsrFlipped(ctx.binding());
    fsrFlipped.debugFlipJitterSign() = true;
    Fsr3TemporalUpscaler fsrSharp(ctx.binding());
    fsrSharp.sharpness() = kRcasSharpness;
    TaauUpscaler taau{};
    std::vector<Vec3> linear[kMethodCount], shown[kMethodCount], previous[kMethodCount];
    for (u32 m = 0; m < kMethodCount; ++m) {
        linear[m].resize(dn);
    }
    q::ImageQualityEvaluator eval{{q::QualityEncoding::Display, 1.f, kernel::Backend::CpuParallel}};
    std::vector<u8> trail(dn), trailDilated(dn), steady(dn);
    u64 ghostPixels = 0;
    f64 refEnergy = 0.0;
    f64 energy[kMethodCount] = {};
    const u32 evalFrames = kFrames - kWarmup;
    for (u32 f = 0; f < kFrames; ++f) {
        if (!scene.renderFrame(f, res, frame)) {
            return false;
        }
        const UpscaleInputs in = frame.inputs();
        const up::UpscaleDispatch dispatch{};
        const up::TemporalUpscaleOutputs o0{{linear[kFsr3].data(), kDisplayW, kDisplayH}};
        const up::TemporalUpscaleOutputs o1{{linear[kFsr3FlippedJitter].data(), kDisplayW, kDisplayH}};
        const up::TemporalUpscaleOutputs o2{{linear[kFsr3Rcas].data(), kDisplayW, kDisplayH}};
        expect(fsrSharp.evaluate(dispatch, in, o2) == up::UpscaleStatus::Ok, "fsr3 (RCAS) evaluate");
        expect(fsr.evaluate(dispatch, in, o0) == up::UpscaleStatus::Ok, "fsr3 evaluate");
        expect(fsrFlipped.evaluate(dispatch, in, o1) == up::UpscaleStatus::Ok, "fsr3 (flipped jitter) evaluate");
        expect(taau.upscale(in, linear[kTaau].data()), "taau upscale");
        expect(spatialUpscale(in, taau_kernel::SpatialFilter::Bilinear, linear[kBilinear].data()), "bilinear");
        for (u32 m = 0; m < kMethodCount; ++m) {
            out[m].finite = out[m].finite && !q::imageHasNonFinite(linear[m].data(), dn);
            toDisplay(linear[m], exposure, shown[m]);
        }
        if (f >= kWarmup) {
            const rs::GroundTruthFrame& g = gt.frames[f];
            const q::QualityImage ref{gt.displayed[f].data(), kDisplayW, kDisplayH};
            const q::QualityImage refPrev{gt.displayed[f - 1u].data(), kDisplayW, kDisplayH};
            const u32* history[kTrailFrames];
            for (u32 k = 0; k < kTrailFrames; ++k) {
                history[k] = gt.frames[f - 1u - k].object_id.data();
            }
            q::buildTrailMask(history, kTrailFrames, g.object_id.data(), rs::kObjectThin, kDisplayW, kDisplayH, trail.data());
            q::dilateMask(trail.data(), kDisplayW, kDisplayH, 1u, trailDilated.data());
            for (usize i = 0; i < dn; ++i) {
                trailDilated[i] = (trailDilated[i] != 0u && g.object_id[i] != rs::kObjectThin) ? 1u : 0u;
                // Sharpness region: static, visible-last-frame pixels away from the rod and its trail.
                steady[i] = (trailDilated[i] == 0u && g.disoccluded[i] == 0u && g.object_id[i] != rs::kObjectThin) ? 1u : 0u;
            }
            refEnergy += gradientEnergy(gt.displayed[f], kDisplayW, kDisplayH, steady);
            for (u32 m = 0; m < kMethodCount; ++m) {
                const q::QualityImage test{shown[m].data(), kDisplayW, kDisplayH};
                const q::QualityImage testPrev{previous[m].data(), kDisplayW, kDisplayH};
                Scores& s = out[m];
                q::PsnrResult pr{};
                eval.psnr(test, ref, pr);
                s.psnr += pr.psnr / evalFrames;
                s.ssim += eval.ssim(test, ref) / evalFrames;
                s.flip += eval.flip(test, ref) / evalFrames;
                q::TemporalResult tr{};
                eval.temporal(testPrev, test, refPrev, ref, false, tr);
                s.tpsnr += std::min(tr.tpsnr, 99.0) / evalFrames;
                q::MaskedErrorResult mr{};
                if (eval.maskedError(test, ref, trailDilated.data(), mr) && mr.pixels > 0u) {
                    s.ghost += mr.mean_abs_luma * static_cast<f64>(mr.pixels);
                    if (m == 0u) {
                        ghostPixels += mr.pixels;
                    }
                }
                energy[m] += gradientEnergy(shown[m], kDisplayW, kDisplayH, steady);
            }
        }
        for (u32 m = 0; m < kMethodCount; ++m) {
            previous[m] = shown[m];
        }
    }
    char line[512];
    std::snprintf(line, sizeof(line), "  \"%s\": {\"frames\": %u, \"evaluated\": %u, \"trail_pixels\": %llu, \"methods\": {",
                  rs::referenceSceneLabel(kind), kFrames, evalFrames, static_cast<unsigned long long>(ghostPixels));
    json += line;
    std::printf("  %-14s %-18s %7s %7s %7s %7s %8s %9s\n", rs::referenceSceneLabel(kind), "method", "PSNR", "SSIM", "FLIP",
                "tPSNR", "ghost", "sharpness");
    for (u32 m = 0; m < kMethodCount; ++m) {
        Scores& s = out[m];
        s.ghost = ghostPixels > 0u ? s.ghost / static_cast<f64>(ghostPixels) : 0.0;
        s.sharpness = refEnergy > 0.0 ? energy[m] / refEnergy : 0.0;
        std::printf("  %-14s %-18s %7.3f %7.4f %7.4f %7.3f %8.5f %9.4f%s\n", "", kMethodNames[m], s.psnr, s.ssim, s.flip, s.tpsnr,
                    s.ghost, s.sharpness, s.finite ? "" : "  NON-FINITE");
        std::snprintf(line, sizeof(line),
                      "%s\"%s\": {\"psnr\": %.4f, \"ssim\": %.5f, \"flip\": %.5f, \"tpsnr\": %.4f, \"ghost\": %.6f, "
                      "\"sharpness\": %.5f, \"finite\": %s}",
                      m == 0u ? "" : ", ", kMethodNames[m], s.psnr, s.ssim, s.flip, s.tpsnr, s.ghost, s.sharpness,
                      s.finite ? "true" : "false");
        json += line;
    }
    json += "}}";
    return true;
}

int runQuality(Context& ctx) {
    std::string json = "{\n  \"backend\": \"fsr3\", \"sdk\": \"FidelityFX SDK 1.1.4 (c6efa6bf)\", \"fsr3_upscaler\": \"3.1.4\",\n";
    char head[256];
    std::snprintf(head, sizeof(head), "  \"display\": [%u, %u], \"ratio\": %.2f, \"gt_spp\": %u,\n", kDisplayW, kDisplayH, kRatio,
                  kGtSamples * kGtSamples);
    json += head;
    const rs::ReferenceSceneKind kinds[2] = {rs::ReferenceSceneKind::ThinFastObject, rs::ReferenceSceneKind::Showcase};
    for (u32 k = 0; k < 2u; ++k) {
        Scores s[kMethodCount];
        if (!evaluateScene(ctx, kinds[k], s, json)) {
            std::fprintf(stderr, "FAIL: scene evaluation\n");
            return 1;
        }
        json += k == 0u ? ",\n" : "\n";
        const Scores& f = s[kFsr3];
        const Scores& r = s[kFsr3Rcas];
        const Scores& t = s[kTaau];
        const Scores& b = s[kBilinear];
        const Scores& x = s[kFsr3FlippedJitter];
        const bool thin = kinds[k] == rs::ReferenceSceneKind::ThinFastObject;
        const SceneGate& gate = thin ? kGateThin : kGateShowcase;
        expect(f.finite && r.finite && x.finite, "FSR 3.1 output finite");
        expect(f.psnr > b.psnr && f.flip < b.flip && r.psnr > b.psnr && r.flip < b.flip, "FSR 3.1 beats bilinear (PSNR, FLIP)");
        expect(f.psnr >= gate.minPsnr && f.flip <= gate.maxFlip, "FSR 3.1 PSNR / FLIP regression bounds");
        expect(f.sharpness >= gate.minSharpness && r.sharpness >= gate.minSharpnessRcas, "FSR 3.1 sharpness floors");
        expect(r.sharpness >= t.sharpness * kGateRcasSharpVsTaau, "FSR 3.1 + RCAS about as sharp as TAAU");
        expect(f.psnr >= t.psnr - kGatePsnrBandVsTaau, "FSR 3.1 PSNR within the documented TAAU band");
        if (thin) {
            expect(f.ghost <= gate.maxGhost && f.ghost <= t.ghost * kGateGhostVsTaau, "FSR 3.1 ghosting bounds (absolute, vs TAAU)");
        }
        expect(f.psnr >= x.psnr + kGateSignPsnr, "jitter sign: correct sign beats the flipped one (PSNR)");
        expect(f.sharpness > x.sharpness, "jitter sign: flipped sign is blurrier");
    }
    json += "}\n";
#if defined(FUSE_FSR3_REPORT_PATH)
    if (FILE* file = std::fopen(FUSE_FSR3_REPORT_PATH, "w")) {
        std::fputs(json.c_str(), file);
        std::fclose(file);
        std::printf("report: %s\n", FUSE_FSR3_REPORT_PATH);
    }
#endif
    return 0;
}

// --- determinism mode ----------------------------------------------------------------------------------
bool runSequence(Context& ctx, Fsr3KernelLanguage language, u32 frames, std::vector<std::vector<Vec3>>& outputs) {
    const rs::ReferenceScene scene(rs::ReferenceSceneKind::ThinFastObject);
    const UpscaleResolution res = resolution();
    const usize dn = static_cast<usize>(kDisplayW) * kDisplayH;
    Fsr3TemporalUpscaler fsr(ctx.binding());
    fsr.kernelLanguage() = language;
    fsr.sharpness() = 0.3f; // RCAS path too
    rs::ReferenceFrame frame{};
    outputs.assign(frames, std::vector<Vec3>(dn));
    for (u32 f = 0; f < frames; ++f) {
        if (!scene.renderFrame(f, res, frame)) {
            return false;
        }
        UpscaleInputs in = frame.inputs();
        in.reset_history = f == 9u;
        const up::TemporalUpscaleOutputs o{{outputs[f].data(), kDisplayW, kDisplayH}};
        if (fsr.evaluate(up::UpscaleDispatch{}, in, o) != up::UpscaleStatus::Ok) {
            return false;
        }
    }
    return language != Fsr3KernelLanguage::Glsl || std::strcmp(fsr.gpu().kernelLanguage(), "glsl") == 0;
}

int runDeterminism(Context& ctx) {
    constexpr u32 kSeq = 16;
    std::vector<std::vector<Vec3>> a, b, c;
    expect(runSequence(ctx, Fsr3KernelLanguage::Auto, kSeq, a), "sequence A");
    expect(runSequence(ctx, Fsr3KernelLanguage::Auto, kSeq, b), "sequence B");
    const bool glsl = fsr3_kernel_code(Fsr3Pass::Convert, Fsr3KernelLanguage::Glsl).words != nullptr;
    const bool slang = fsr3_kernel_code(Fsr3Pass::Convert, Fsr3KernelLanguage::Slang).words != nullptr;
    const bool twin = glsl && slang;
    if (twin) {
        expect(runSequence(ctx, Fsr3KernelLanguage::Glsl, kSeq, c), "sequence C (GLSL convert)");
    }
    u32 identical = 0, identicalTwin = 0;
    for (u32 f = 0; f < kSeq; ++f) {
        const bool same = a.size() == kSeq && b.size() == kSeq &&
                          std::memcmp(a[f].data(), b[f].data(), a[f].size() * sizeof(Vec3)) == 0;
        identical += same ? 1u : 0u;
        if (twin && c.size() == kSeq) {
            identicalTwin += std::memcmp(a[f].data(), c[f].data(), a[f].size() * sizeof(Vec3)) == 0 ? 1u : 0u;
        }
    }
    std::printf("determinism: %u / %u frames bit-identical across two fresh backends (RCAS on, reset at frame 9)\n", identical, kSeq);
    expect(identical == kSeq, "FSR 3.1 output is deterministic");
    if (twin) {
        std::printf("determinism: %u / %u frames bit-identical between the Slang and the GLSL convert kernel\n", identicalTwin, kSeq);
        expect(identicalTwin == kSeq, "Slang and GLSL convert twins agree bit for bit");
    }
    return 0;
}

// --- reset mode ----------------------------------------------------------------------------------------
int runReset(Context& ctx) {
    const rs::ReferenceScene before(rs::ReferenceSceneKind::Showcase);
    const rs::ReferenceScene after(rs::ReferenceSceneKind::MoireFlight);
    const UpscaleResolution res = resolution();
    const usize dn = static_cast<usize>(kDisplayW) * kDisplayH;
    constexpr u32 kBefore = 12;
    constexpr u32 kCutFrame = 20; // MoireFlight frame used after the cut
    rs::ReferenceFrame frame{};
    rs::GroundTruthFrame g{};
    expect(after.renderGroundTruth(kCutFrame, kDisplayW, kDisplayH, kGtSamples, g), "truth after the cut");
    Fsr3TemporalUpscaler withReset(ctx.binding());
    Fsr3TemporalUpscaler withoutReset(ctx.binding());
    Fsr3TemporalUpscaler invalidated(ctx.binding());
    std::vector<Vec3> out(dn), outNo(dn), outFresh(dn), outInv(dn);
    for (u32 f = 0; f < kBefore; ++f) {
        expect(before.renderFrame(f, res, frame), "frame before the cut");
        const UpscaleInputs in = frame.inputs();
        expect(withReset.evaluate({}, in, {{out.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok &&
                   withoutReset.evaluate({}, in, {{outNo.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok &&
                   invalidated.evaluate({}, in, {{outInv.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok,
               "pre-cut frames");
    }
    const u32 generation = withReset.history_generation();
    expect(withReset.accumulated_frames() == kBefore, "accumulated frames before the cut");
    expect(after.renderFrame(kCutFrame, res, frame), "frame after the cut");
    UpscaleInputs in = frame.inputs();
    in.reset_history = true;
    expect(withReset.evaluate({}, in, {{out.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok, "cut with reset");
    expect(withReset.gpu().stats().reset && withReset.gpu().frameSetup().constants.frameIndex == 0.f, "Fsr3Gpu reset the accumulation");
    expect(withReset.history_generation() == generation + 1u && withReset.accumulated_frames() == 1u &&
               withReset.last_reset_reason() == up::HistoryResetReason::CameraCut,
           "history generation / accumulated frames / reason");
    invalidated.invalidate_history(up::HistoryResetReason::Teleport);
    UpscaleInputs noReset = in;
    noReset.reset_history = false;
    expect(invalidated.evaluate({}, noReset, {{outInv.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok &&
               invalidated.gpu().stats().reset,
           "invalidate_history() resets the next frame");
    expect(withoutReset.evaluate({}, noReset, {{outNo.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok &&
               !withoutReset.gpu().stats().reset,
           "cut without reset");
    Fsr3TemporalUpscaler fresh(ctx.binding());
    expect(fresh.evaluate({}, noReset, {{outFresh.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok, "fresh backend");
    const f32 exposure = frame.exposure;
    std::vector<Vec3> ref, a, b, c, d;
    toDisplay(g.color, exposure, ref);
    toDisplay(out, exposure, a);
    toDisplay(outNo, exposure, b);
    toDisplay(outFresh, exposure, c);
    toDisplay(outInv, exposure, d);
    q::ImageQualityEvaluator eval{{q::QualityEncoding::Display, 1.f, kernel::Backend::CpuParallel}};
    const q::QualityImage r{ref.data(), kDisplayW, kDisplayH};
    q::PsnrResult pa{}, pb{}, pc{}, pd{};
    eval.psnr({a.data(), kDisplayW, kDisplayH}, r, pa);
    eval.psnr({b.data(), kDisplayW, kDisplayH}, r, pb);
    eval.psnr({c.data(), kDisplayW, kDisplayH}, r, pc);
    eval.psnr({d.data(), kDisplayW, kDisplayH}, r, pd);
    f64 maxDiff = 0.0;
    for (usize i = 0; i < dn; ++i) {
        maxDiff = std::max({maxDiff, static_cast<f64>(std::fabs(a[i].x - c[i].x)), static_cast<f64>(std::fabs(a[i].y - c[i].y)),
                            static_cast<f64>(std::fabs(a[i].z - c[i].z))});
    }
    std::printf("reset: first frame after the cut vs 16-spp truth: reset %.3f dB, invalidate_history %.3f dB, fresh backend %.3f dB, "
                "no reset %.3f dB (old-scene history); reset vs fresh max |diff| %.3g (display)\n",
                pa.psnr, pd.psnr, pc.psnr, pb.psnr, maxDiff);
    expect(std::fabs(pa.psnr - pc.psnr) <= 0.25 && std::fabs(pd.psnr - pc.psnr) <= 0.25, "reset frame == a fresh backend's first frame");
    // Measured: reset == invalidate == fresh bit for bit (24.73 dB), no reset 22.77 dB: FSR's own disocclusion /
    // shading-change detection already rejects most of the old scene, the rest is the ghost the reset removes.
    expect(maxDiff <= 1e-3, "reset frame == a fresh backend's first frame (display values)");
    expect(pa.psnr >= pb.psnr + 1.0, "reset removes the old scene's history (>= 1 dB better than no reset)");
    return 0;
}

// --- switch mode ---------------------------------------------------------------------------------------
int runSwitch(Context& ctx) {
#if !defined(FUSE_UPSCALER_FSR3)
    (void)ctx;
    std::printf("SKIP: switch: built with FUSE_UPSCALER_FSR3=OFF (register_fsr3_backend never registers \"fsr3\")\n");
    return kSkip;
#else
    up::UpscalerRegistry registry;
    registry.register_builtin_backends();
    expect(register_fsr3_backend(registry, ctx.binding()), "register fsr3");
    const up::UpscalerCaps* caps = registry.find(up::kFsr3Name);
    expect(caps != nullptr && caps->temporal && caps->supports_api(up::UpscalerApi::Vulkan) && std::strcmp(caps->license, "MIT") == 0,
           "fsr3 caps");
    up::UpscalerRequirements req{};
    req.api = up::UpscalerApi::Vulkan;
    const up::UpscalerCaps* chosen = registry.select(req);
    expect(chosen != nullptr && std::strcmp(chosen->name, up::kFsr3Name) == 0, "select(Vulkan) picks fsr3");
    const char* order[4] = {up::kNativeTaauName, up::kFsr1Name, up::kNisName, up::kFsr3Name};
    std::unique_ptr<up::IUpscaler> instances[4];
    for (u32 i = 0; i < 4u; ++i) {
        instances[i] = registry.create(order[i]);
        expect(instances[i] != nullptr, "create backend");
        if (instances[i] == nullptr) {
            return 1;
        }
    }
    const rs::ReferenceScene scene(rs::ReferenceSceneKind::Showcase);
    const UpscaleResolution res = resolution();
    const usize rn = static_cast<usize>(res.render_width) * res.render_height;
    const usize dn = static_cast<usize>(kDisplayW) * kDisplayH;
    rs::ReferenceFrame frame{};
    rs::GroundTruthFrame g{};
    std::vector<Vec3> linear(dn), shown, ref;
    std::vector<Vec4> spatialIn(rn), spatialOut(dn);
    q::ImageQualityEvaluator eval{{q::QualityEncoding::Display, 1.f, kernel::Backend::CpuParallel}};
    f64 flip[4] = {};
    u32 counts[4] = {};
    bool finite = true;
    constexpr u32 kSwitchFrames = 60;
    u32 active = 4;
    for (u32 f = 0; f < kSwitchFrames; ++f) {
        const u32 which = (f / 5u) % 4u;
        expect(scene.renderFrame(f, res, frame), "render frame");
        UpscaleInputs in = frame.inputs();
        up::IUpscaler* u = instances[which].get();
        if (which != active && u->caps().temporal) {
            // Re-entering a temporal backend: its history is stale.
            static_cast<up::ITemporalUpscaler*>(u)->invalidate_history(up::HistoryResetReason::Explicit);
        }
        active = which;
        if (u->caps().temporal) {
            auto* t = static_cast<up::ITemporalUpscaler*>(u);
            expect(t->evaluate({}, in, {{linear.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok, "temporal evaluate");
            finite = finite && !q::imageHasNonFinite(linear.data(), dn);
            toDisplay(linear, frame.exposure, shown);
        } else {
            for (usize i = 0; i < rn; ++i) {
                const Vec3 c = display(frame.color[i], frame.exposure);
                spatialIn[i] = Vec4(c.x, c.y, c.z, 1.f);
            }
            up::SpatialUpscaleInputs si{};
            si.color = {spatialIn.data(), res.render_width, res.render_height};
            si.sharpness = 0.2f;
            auto* s = static_cast<up::ISpatialUpscaler*>(u);
            expect(s->evaluate({}, si, {{spatialOut.data(), kDisplayW, kDisplayH}}) == up::UpscaleStatus::Ok, "spatial evaluate");
            shown.resize(dn);
            for (usize i = 0; i < dn; ++i) {
                shown[i] = Vec3(spatialOut[i].x, spatialOut[i].y, spatialOut[i].z);
                finite = finite && std::isfinite(shown[i].x) && std::isfinite(shown[i].y) && std::isfinite(shown[i].z);
            }
        }
        expect(scene.renderGroundTruth(f, kDisplayW, kDisplayH, kGtSamples, g), "truth");
        toDisplay(g.color, frame.exposure, ref);
        const f64 fl = eval.flip({shown.data(), kDisplayW, kDisplayH}, {ref.data(), kDisplayW, kDisplayH});
        finite = finite && std::isfinite(fl);
        flip[which] += fl;
        ++counts[which];
    }
    std::printf("switch: %u frames, native_taau -> fsr1 -> nis -> fsr3 every 5 frames; mean FLIP vs 16-spp truth:", kSwitchFrames);
    for (u32 i = 0; i < 4u; ++i) {
        flip[i] /= std::max(counts[i], 1u);
        std::printf(" %s %.4f", order[i], flip[i]);
    }
    std::printf("\n");
    expect(finite, "no NaN / Inf across the switches");
    for (u32 i = 0; i < 4u; ++i) {
        expect(counts[i] == 15u && flip[i] < 0.25, "every backend ran 15 frames with a bounded FLIP");
    }
    instances[3].reset();
    unregister_fsr3_backend(registry);
    expect(registry.find(up::kFsr3Name) == nullptr, "unregister fsr3");
    return 0;
#endif
}

// --- zero_alloc mode -----------------------------------------------------------------------------------
struct InputImage {
    Texture tex{};
    BindlessSlotHandle slot{};
    u32 handle = 0;
    u32 format = 0;
    u32 layout = 0;
    u8 queue = rg::kNoQueue;

    bool create(Context& ctx, u32 w, u32 h, u32 fmt, const char* name) {
        TextureDesc d{};
        d.width = w;
        d.height = h;
        d.format = static_cast<GpuFormat>(fmt);
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        d.name = name;
        if (!ctx.allocator->createImage(d, tex)) {
            return false;
        }
        slot = ctx.bindless.registerTextureSlot(tex, false);
        handle = slot.isValid() ? ctx.bindless.shaderHandle(slot) : 0u;
        format = fmt;
        return handle != 0u;
    }
    void destroy(Context& ctx) {
        if (slot.isValid()) {
            ctx.bindless.unregisterSlot(slot);
        }
        if (tex.image != nullptr) {
            ctx.allocator->destroyImage(tex);
        }
        *this = InputImage{};
    }
    rg::TextureRef import(rg::Graph& graph) {
        rg::ImportedImage i{};
        i.image = tex.image;
        i.view = tex.view;
        i.format = format;
        i.width = tex.desc.width;
        i.height = tex.desc.height;
        i.initialLayout = layout;
        i.initialQueue = queue;
        i.layoutTracker = &layout;
        i.queueTracker = &queue;
        i.name = "rp_fsr3.input";
        return graph.importImage(i);
    }
};

struct HostBuffer {
    Buffer buffer{};
    u8 queue = rg::kNoQueue;
    bool create(Context& ctx, u64 bytes, BufferUsage usage, MemoryUsage memory, const char* n) {
        BufferDesc d{};
        d.size = static_cast<usize>(bytes);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = n;
        return ctx.allocator->createBuffer(d, buffer) && buffer.mapped != nullptr;
    }
    void destroy(Context& ctx) {
        if (buffer.handle != nullptr) {
            ctx.allocator->destroyBuffer(buffer);
        }
        buffer = Buffer{};
    }
    rg::BufferRef import(rg::Graph& graph, const char* name) {
        return graph.importBuffer(rg::ImportedBuffer{buffer.handle, buffer.desc.size, queue, &queue, name});
    }
};

struct UploadRecord {
    rg::TextureRef image;
    rg::BufferRef src;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const UploadRecord& u = *static_cast<const UploadRecord*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = u.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {u.width, u.height, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(u.src)),
                           static_cast<VkImage>(pc.image(u.image)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "fsr3.", 5) == 0) {
        t_inPass = true;
        t_count = true;
    }
}

void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, bool countAllocations) {
    const UpscaleResolution res = resolution();
    const u32 rn = res.render_width * res.render_height;
    InputImage color, reactive, transparency;
    HostBuffer staging, depth, motion;
    // TransferSrc: taau.keep copies depth / motion into its previous-frame buffers.
    const BufferUsage storage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                         static_cast<u32>(BufferUsage::ShaderDeviceAddress) |
                                                         static_cast<u32>(BufferUsage::TransferSrc));
    if (!color.create(ctx, res.render_width, res.render_height, 109u, "rp_fsr3.color") ||
        !reactive.create(ctx, res.render_width, res.render_height, 100u, "rp_fsr3.reactive") ||
        !transparency.create(ctx, res.render_width, res.render_height, 100u, "rp_fsr3.transparency") ||
        !staging.create(ctx, rn * 24ull, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "rp_fsr3.staging") ||
        !depth.create(ctx, rn * 4ull, storage, MemoryUsage::CpuToGpu, "rp_fsr3.depth") ||
        !motion.create(ctx, rn * 8ull, storage, MemoryUsage::CpuToGpu, "rp_fsr3.motion")) {
        std::fprintf(stderr, "FAIL: inputs\n");
        return 1;
    }
    temporal::TaauGpu taau;
    temporal::TaauGpuDesc td{};
    td.device = ctx.device.get();
    td.allocator = ctx.allocator.get();
    td.bindless = &ctx.bindless;
    td.resolution = res;
    const bool haveTaau = taau.init(td);
    Fsr3Gpu fsr;
    Fsr3GpuDesc fd{};
    fd.device = ctx.device.get();
    fd.allocator = ctx.allocator.get();
    fd.maxResolution = res;
    if (!fsr.init(fd)) {
        std::fprintf(stderr, "FAIL: Fsr3Gpu init\n");
        return 1;
    }
    rg::Graph graph;
    constexpr u32 kWarmupFrames = 16;
    constexpr u32 kTotal = 80;
    unsigned long long begin = 0, build = 0, callbacks = 0;
    u32 conflicts = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    UploadRecord ups[3];
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmupFrames;
        const bool upload = frame < 2u || (frame % 7u) == 0u; // new content now and then; shape changes are warm-up
        const tm_test::TaauFrame tf = tm_test::makeTaauFrame(res, frame % 8u, true, true);
        if (upload) {
            f32* s = static_cast<f32*>(staging.buffer.mapped);
            for (u32 i = 0; i < rn; ++i) {
                s[i * 4u + 0u] = tf.color[i].x;
                s[i * 4u + 1u] = tf.color[i].y;
                s[i * 4u + 2u] = tf.color[i].z;
                s[i * 4u + 3u] = 1.f;
            }
            std::memcpy(s + rn * 4u, tf.reactive.data(), rn * 4u);
            std::memcpy(s + rn * 5u, tf.transparency.data(), rn * 4u);
            std::memcpy(depth.buffer.mapped, tf.depth.data(), rn * 4u);
            std::memcpy(motion.buffer.mapped, tf.motion.data(), rn * 8u);
        }
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        Fsr3GpuFrameDesc f{};
        f.resolution = res;
        f.jitter_px = tf.jitter;
        f.exposure = 0.7f;
        f.camera = tf.camera;
        f.camera.near_plane = 0.1f;
        f.camera.far_plane = 100.f;
        f.camera.vertical_fov_rad = 0.9f;
        f.reset_history = frame == 0u;
        f.sharpen = (frame & 1u) != 0u;
        f.sharpness = 0.5f;
        temporal::TaauGpuFrameDesc tfd{};
        tfd.resolution = res;
        tfd.jitter_px = tf.jitter;
        tfd.exposure = 0.7f;
        tfd.camera = tf.camera;
        tfd.previous_camera = tf.previousCamera;
        tfd.color = color.handle;
        tfd.reactive = reactive.handle;
        tfd.transparency = transparency.handle;
        tfd.depth = depth.buffer.deviceAddress;
        tfd.motion = motion.buffer.deviceAddress;
        t_allocations = 0;
        t_count = measure;
        const bool began = fsr.beginFrame(ctx.serial, f);
        t_count = false;
        begin += measure ? t_allocations : 0u;
        const bool taauBegan = !haveTaau || taau.beginFrame(ctx.serial, tfd);
        t_allocations = 0;
        t_count = measure && !upload;
        graph.reset();
        const rg::BufferRef stagingRef = staging.import(graph, "rp_fsr3.staging");
        InputImage* images[3] = {&color, &reactive, &transparency};
        const u64 offsets[3] = {0u, rn * 16ull, rn * 20ull};
        rg::TextureRef refs[3];
        for (u32 k = 0; k < 3u; ++k) {
            refs[k] = images[k]->import(graph);
            if (upload) {
                ups[k] = UploadRecord{refs[k], stagingRef, offsets[k], res.render_width, res.render_height};
                graph.addPass("upload.input", &recordUpload, &ups[k])
                    .use(refs[k], rg::Access::TransferDst)
                    .use(stagingRef, rg::Access::TransferSrc, rg::BufferRange{offsets[k], k == 0u ? rn * 16ull : rn * 4ull});
            }
        }
        const rg::BufferRef depthRef = depth.import(graph, "rp_fsr3.depth");
        const rg::BufferRef motionRef = motion.import(graph, "rp_fsr3.motion");
        if (haveTaau) {
            const temporal::TaauGraphRefs t = taau.importInto(graph);
            temporal::TaauGpuInputs ti{};
            ti.color = refs[0];
            ti.reactive = refs[1];
            ti.transparency = refs[2];
            ti.depth = depthRef;
            ti.motion = motionRef;
            taau.addResolve(graph, t, ti);
        }
        const Fsr3GraphRefs fr = fsr.importInto(graph);
        Fsr3GpuInputs fi{};
        fi.color = refs[0];
        fi.reactive = refs[1];
        fi.transparency = refs[2];
        fi.depth = depthRef;
        fi.motion = motionRef;
        fsr.addPasses(graph, fr, fi);
        t_count = false;
        build += (measure && !upload) ? t_allocations : 0u;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        callbacks += measure ? t_allocations : 0u;
        const bool waited = ctx.executor->waitIdle();
        conflicts += graph.stats().layoutConflicts;
        fsr.collectRetired(ctx.serial);
        if (haveTaau) {
            taau.collectRetired(ctx.serial);
        }
        ctx.bindless.collectRetired(ctx.serial);
        expect(began && taauBegan && result.ok && waited, "frame ok");
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(conflicts == 0u, "no layout conflicts in the FSR passes' declared accesses");
    expect(fsr.stats().passes == 9u || fsr.stats().passes == 8u, "8 compute passes (9 with RCAS) per frame");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (TAAU %s + FSR 3.1 %ux%u -> %ux%u, RCAS every other frame, %s bindless)\n"
                    "  Fsr3Gpu::beginFrame: %llu, graph build (imports + TAAU + FSR passes, frames without uploads): %llu, "
                    "fsr3.* callbacks: %llu operator-new calls\n",
                    kTotal - kWarmupFrames, haveTaau ? "GPU" : "absent", res.render_width, res.render_height, res.display_width,
                    res.display_height, bindlessBackendName(ctx.bindless.backend()), begin, build, callbacks);
        expect(begin == 0u && build == 0u && callbacks == 0u, "FSR 3.1 makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (TAAU %s, %s bindless), %u layout "
                    "conflicts\n",
                    kTotal, haveTaau ? "GPU" : "absent", bindlessBackendName(ctx.bindless.backend()), conflicts);
    }
    ctx.executor->waitIdle();
    taau.destroy();
    fsr.destroy();
    color.destroy(ctx);
    reactive.destroy(ctx);
    transparency.destroy(ctx);
    staging.destroy(ctx);
    depth.destroy(ctx);
    motion.destroy(ctx);
    ctx.bindless.collectRetired(~0ull);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "quality";
    std::string backend = "set";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            backend = argv[i + 1];
        }
    }
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, backend == "buffer", true);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "quality") {
            rc = runQuality(ctx);
        } else if (mode == "determinism") {
            rc = runDeterminism(ctx);
        } else if (mode == "reset") {
            rc = runReset(ctx);
        } else if (mode == "switch") {
            rc = runSwitch(ctx);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, false);
        } else {
            std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
            return 2;
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (mode == "zero_alloc" && rc == 0) {
        Context counted;
        const int setupRc = setup(counted, backend == "buffer", false);
        if (setupRc != 0) {
            return setupRc;
        }
        rc = runZeroAlloc(counted, true);
    }
    std::printf("validation messages: %u\n", g_messages);
    if (rc != 0 || g_failures != 0 || g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %d failure(s), %u validation message(s)\n", g_failures + rc, g_messages);
        return 1;
    }
    std::printf("PASS %s (%s)\n", mode.c_str(), backend.c_str());
    return 0;
}

#endif
