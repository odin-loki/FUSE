#pragma once
// Renderer hardware exit-criteria benches (docs/unification/RENDERER-HW-RUN.md): the shared pieces of the
// fuse_rp_hw_bench* executables.
//
//   HwContext         Vulkan instance (no validation layer: timings), the preferred device, allocator, bindless,
//                     upload queue, render-graph executor and a WP-0.6 GpuProfiler whose pass hooks put one
//                     timestamp zone around every executed render-graph pass. setup() returns 77 (skip) when
//                     the device is not a discrete GPU (the perf gates are meaningless on Lavapipe / iGPUs),
//                     unless the caller allows any device (smoke runs).
//   Stats             median / min / max / p10 / p90 / MAD of a sample vector.
//   Json*             a tiny flat-JSON reader / writer for the committed baselines ("key": number pairs, one
//                     object per device name).
//
// Noise strategy of every timed number (see RENDERER-HW-RUN.md, "Noise"): warm-up frames are discarded (clocks,
// pipeline caches, temporal histories, VSM page cache), then N measured frames; each pass's per-frame GPU duration
// comes from the timestamp pair around it, a stage is the sum of its passes in that frame, and the reported value
// is the median over the frames. Gates compare the median of K independent trials' medians, and re-measure once
// before failing (another process sharing the GPU shows up as one slow trial, not as a shift of every trial).
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/gpu_profiler.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace hw_bench {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

constexpr int kSkip = 77;

inline int g_failures = 0;

inline void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- statistics ------------------------------------------------------------------------------------------
struct Stats {
    f64 median = 0.0;
    f64 mean = 0.0;
    f64 min = 0.0;
    f64 max = 0.0;
    f64 p10 = 0.0;
    f64 p90 = 0.0;
    f64 mad = 0.0; ///< median absolute deviation
    usize count = 0;
};

inline f64 quantile(std::vector<f64> v, f64 q) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const f64 pos = q * static_cast<f64>(v.size() - 1u);
    const usize lo = static_cast<usize>(std::floor(pos));
    const usize hi = std::min(v.size() - 1u, lo + 1u);
    const f64 t = pos - static_cast<f64>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
}

inline Stats stats(const std::vector<f64>& v) {
    Stats s{};
    s.count = v.size();
    if (v.empty()) {
        return s;
    }
    s.median = quantile(v, 0.5);
    s.p10 = quantile(v, 0.1);
    s.p90 = quantile(v, 0.9);
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    f64 sum = 0.0;
    for (f64 x : v) {
        sum += x;
    }
    s.mean = sum / static_cast<f64>(v.size());
    std::vector<f64> dev(v.size());
    for (usize i = 0; i < v.size(); ++i) {
        dev[i] = std::fabs(v[i] - s.median);
    }
    s.mad = quantile(dev, 0.5);
    return s;
}

// --- flat JSON (baselines) -------------------------------------------------------------------------------
/// {"<section>": {"<key>": <number>, ...}, ...}: one level of objects of numbers (and "_comment" strings, ignored).
using JsonSection = std::map<std::string, f64>;
using JsonDoc = std::map<std::string, JsonSection>;

inline bool jsonRead(const std::string& path, JsonDoc& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    usize i = 0;
    auto skipWs = [&]() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t' || s[i] == ',' || s[i] == ':')) {
            ++i;
        }
    };
    auto readString = [&](std::string& str) {
        if (i >= s.size() || s[i] != '"') {
            return false;
        }
        const usize end = s.find('"', i + 1u);
        if (end == std::string::npos) {
            return false;
        }
        str = s.substr(i + 1u, end - i - 1u);
        i = end + 1u;
        return true;
    };
    skipWs();
    if (i >= s.size() || s[i] != '{') {
        return false;
    }
    ++i;
    for (;;) {
        skipWs();
        if (i < s.size() && s[i] == '}') {
            return true;
        }
        std::string section;
        if (!readString(section)) {
            return false;
        }
        skipWs();
        if (i < s.size() && s[i] == '"') { // top-level string value ("_comment")
            std::string ignored;
            readString(ignored);
            continue;
        }
        if (i >= s.size() || s[i] != '{') {
            return false;
        }
        ++i;
        JsonSection& sec = out[section];
        for (;;) {
            skipWs();
            if (i < s.size() && s[i] == '}') {
                ++i;
                break;
            }
            std::string key;
            if (!readString(key)) {
                return false;
            }
            skipWs();
            if (i < s.size() && s[i] == '"') {
                std::string ignored;
                readString(ignored);
                continue;
            }
            char* end = nullptr;
            const f64 v = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) {
                return false;
            }
            i = static_cast<usize>(end - s.c_str());
            sec[key] = v;
        }
    }
}

inline bool jsonWrite(const std::string& path, const JsonDoc& doc, const char* comment) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f << "{\n";
    f << "  \"_comment\": \"" << comment << "\"";
    for (const auto& [name, sec] : doc) {
        f << ",\n  \"" << name << "\": {";
        bool first = true;
        for (const auto& [k, v] : sec) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            f << (first ? "\n" : ",\n") << "    \"" << k << "\": " << buf;
            first = false;
        }
        f << "\n  }";
    }
    f << "\n}\n";
    return static_cast<bool>(f);
}

inline bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

#if defined(FUSE_VULKAN_BACKEND)

#if defined(_WIN32)
inline int setenvCompat(const char* name, const char* value) { return _putenv_s(name, value); }
#else
inline int setenvCompat(const char* name, const char* value) { return setenv(name, value, 1); }
#endif

// --- Vulkan context --------------------------------------------------------------------------------------
struct HwContext {
    std::unique_ptr<fuse::renderer::VulkanInstance> instance;
    std::unique_ptr<fuse::renderer::VulkanDevice> device;
    std::unique_ptr<fuse::renderer::GpuAllocator> allocator;
    std::unique_ptr<fuse::renderer::rg::Executor> executor;
    std::unique_ptr<fuse::renderer::GpuProfiler> profiler;
    fuse::renderer::BindlessDescriptors bindless;
    fuse::renderer::UploadQueue upload;
    fuse::renderer::Buffer staging{};
    VkDevice vkDevice = VK_NULL_HANDLE;
    u64 serial = 0;
    bool discrete = false;

    HwContext() = default;
    HwContext(const HwContext&) = delete;
    HwContext& operator=(const HwContext&) = delete;
    ~HwContext() { shutdown(); }

    void shutdown() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        profiler.reset();
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(staging);
        }
        if (device != nullptr) {
            bindless.collectRetired(~0ull);
            bindless.destroy(*device);
        }
        allocator.reset();
        device.reset();
        instance.reset();
        vkDevice = VK_NULL_HANDLE;
    }
};

struct HwSetupDesc {
    const char* appName = "fuse_rp_hw_bench";
    bool requireDiscrete = true;        ///< 77 on a non-discrete device (the perf gates)
    usize stagingBytes = 64u * 1024u * 1024u;
    u32 maxZonesPerFrame = 1024;
    bool descriptorSetBackend = true;   ///< BindlessBackendPreference::DescriptorSet (the frame gates' choice)
};

/// 0 ok, 77 skip, 1 failure.
inline int setup(HwContext& ctx, const HwSetupDesc& desc = {}) {
    using namespace fuse::renderer;
    setenvCompat("VK_INSTANCE_LAYERS", "");
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = desc.appName;
    instanceDesc.enableValidation = false;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    VulkanDeviceDesc dd{};
    dd.preferDiscreteGpu = true;
    ctx.device = VulkanDevice::create(*ctx.instance, dd);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    ctx.discrete = ctx.device->info().deviceType == static_cast<u32>(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    std::printf("device: %s (%s)\n", ctx.device->info().deviceName.c_str(), ctx.discrete ? "discrete" : "not discrete");
    if (desc.requireDiscrete && !ctx.discrete) {
        std::printf("SKIP: not a discrete GPU (hardware perf gate)\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    BindlessDesc bdesc{};
    if (desc.descriptorSetBackend) {
        bdesc.backend = BindlessBackendPreference::DescriptorSet;
    }
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = desc.stagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "hw_bench.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, desc.stagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    GpuProfilerDesc pd{};
    pd.maxZonesPerFrame = desc.maxZonesPerFrame;
    pd.framesInFlight = 4;
    pd.enableTracy = false;
    pd.name = "hw_bench.gpu";
    ctx.profiler = GpuProfiler::create(*ctx.device, pd);
    if (ctx.profiler == nullptr || !ctx.profiler->isValid()) {
        std::printf("SKIP: GpuProfiler unavailable (%s)\n", ctx.profiler != nullptr ? ctx.profiler->message().c_str() : "null");
        return kSkip;
    }
    ctx.executor->setPassHooks(ctx.profiler->passHooks());
    return 0;
}

/// Per-pass GPU durations (ms) of one resolved frame, by zone name (repeated names summed). The SVGF instances of
/// the frame composer share pass names ("denoise.*"); they are told apart by recording order: denoise passes after
/// "rt.reflections" are renamed "refl/denoise.*", after a "restir." pass "restir/denoise.*" (the RT-shadow
/// denoiser runs before both).
inline std::map<std::string, f64> zoneTimes(const fuse::renderer::GpuFrameTimings& t) {
    std::map<std::string, f64> out;
    const char* prefix = "";
    for (const fuse::renderer::GpuZoneTiming& z : t.zones) {
        const std::string n = z.name;
        if (n.rfind("rt.reflections", 0) == 0) {
            prefix = "refl/";
        } else if (n.rfind("restir.", 0) == 0) {
            prefix = "restir/";
        } else if (n.rfind("denoise.", 0) != 0 && n.rfind("frame.", 0) == 0) {
            prefix = "";
        }
        const std::string key = n.rfind("denoise.", 0) == 0 ? std::string(prefix) + n : n;
        out[key] += static_cast<f64>(z.durationNs()) * 1.0e-6;
    }
    return out;
}

/// GPU time of the whole frame (ms): first zone begin to last zone end (includes the gaps between passes).
inline f64 frameSpanMs(const fuse::renderer::GpuFrameTimings& t) {
    if (t.zones.empty()) {
        return 0.0;
    }
    u64 lo = ~0ull;
    u64 hi = 0;
    for (const fuse::renderer::GpuZoneTiming& z : t.zones) {
        lo = std::min(lo, z.beginNs);
        hi = std::max(hi, z.endNs);
    }
    return hi > lo ? static_cast<f64>(hi - lo) * 1.0e-6 : 0.0;
}

#endif // FUSE_VULKAN_BACKEND

} // namespace hw_bench
