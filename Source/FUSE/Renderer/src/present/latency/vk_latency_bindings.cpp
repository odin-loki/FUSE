// WP-4.4: VK_NV_low_latency2 and VK_AMD_anti_lag entry points for the latency providers (latency_provider.hpp).
// Resolved at run time with vkGetDeviceProcAddr, only when the device enabled the extension (the swapchain /
// device owner enables them; VulkanDevice::info().enabledExtensions tells). Neither extension exists on Lavapipe:
// there the bindings report "not enabled" and the gates drive the providers through mocks of the dispatch tables.
#include <fuse/renderer/present/latency/latency_provider.hpp>

#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::present {

namespace {
[[maybe_unused]] bool extensionEnabled(const VulkanDevice& device, const char* name) {
    for (const char* e : device.info().enabledExtensions) {
        if (e != nullptr && std::strcmp(e, name) == 0) {
            return true;
        }
    }
    return false;
}

#if defined(FUSE_VULKAN_BACKEND)
// VK_AMD_anti_lag (extension 477, revision 1) is newer than the Vulkan headers FUSE builds against (1.3.275): the
// registry definitions, mirrored here (numeric values from vk.xml).
constexpr VkStructureType kStructureTypeAntiLagDataAmd = static_cast<VkStructureType>(1000476001);
constexpr VkStructureType kStructureTypeAntiLagPresentationInfoAmd = static_cast<VkStructureType>(1000476002);
struct AntiLagPresentationInfoAmd {
    VkStructureType sType;
    void* pNext;
    u32 stage; // VkAntiLagStageAMD
    u64 frameIndex;
};
struct AntiLagDataAmd {
    VkStructureType sType;
    const void* pNext;
    u32 mode; // VkAntiLagModeAMD
    u32 maxFPS;
    const AntiLagPresentationInfoAmd* pPresentationInfo;
};
using PfnAntiLagUpdateAmd = void(VKAPI_PTR*)(VkDevice device, const AntiLagDataAmd* pData);

enum : u32 { kFnSleepMode = 0, kFnSleep, kFnMarker, kFnTimings, kFnWait };

#endif
} // namespace

VkLowLatency2Binding::~VkLowLatency2Binding() { destroy(); }

bool VkLowLatency2Binding::init(const VulkanDevice& device, void* swapchain, std::string& reason) {
    destroy();
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        reason = "no device";
        return false;
    }
    if (!extensionEnabled(device, VK_NV_LOW_LATENCY_2_EXTENSION_NAME)) {
        reason = "VK_NV_low_latency2 not enabled on the device";
        return false;
    }
    if (swapchain == nullptr) {
        reason = "VK_NV_low_latency2 needs the swapchain (latency state is per VkSwapchainKHR)";
        return false;
    }
    const VkDevice vk = static_cast<VkDevice>(device.nativeHandle());
    const char* names[] = {"vkSetLatencySleepModeNV", "vkLatencySleepNV", "vkSetLatencyMarkerNV", "vkGetLatencyTimingsNV",
                           "vkWaitSemaphores"};
    for (u32 i = 0; i < 5u; ++i) {
        m_fn[i] = reinterpret_cast<void*>(vkGetDeviceProcAddr(vk, names[i]));
        if (m_fn[i] == nullptr) {
            reason = std::string(names[i]) + " did not resolve";
            destroy();
            return false;
        }
    }
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    ci.pNext = &type;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(vk, &ci, nullptr, &semaphore) != VK_SUCCESS) {
        reason = "timeline semaphore creation failed";
        destroy();
        return false;
    }
    m_device = vk;
    m_swapchain = swapchain;
    m_semaphore = semaphore;
    reason = "ok";
    return true;
#else
    (void)device;
    (void)swapchain;
    reason = "stub backend (no Vulkan)";
    return false;
#endif
}

void VkLowLatency2Binding::destroy() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_semaphore != nullptr && m_device != nullptr) {
        vkDestroySemaphore(static_cast<VkDevice>(m_device), static_cast<VkSemaphore>(m_semaphore), nullptr);
    }
#endif
    m_semaphore = nullptr;
    m_device = nullptr;
    m_swapchain = nullptr;
    for (void*& f : m_fn) {
        f = nullptr;
    }
}

LowLatency2Dispatch VkLowLatency2Binding::dispatch() {
    LowLatency2Dispatch d{};
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr) {
        return d;
    }
    d.user = this;
    d.set_sleep_mode = [](void* user, bool lowLatency, bool boost, u32 minimumIntervalUs) -> i32 {
        const VkLowLatency2Binding& b = *static_cast<const VkLowLatency2Binding*>(user);
        VkLatencySleepModeInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
        info.lowLatencyMode = lowLatency ? VK_TRUE : VK_FALSE;
        info.lowLatencyBoost = boost ? VK_TRUE : VK_FALSE;
        info.minimumIntervalUs = minimumIntervalUs;
        const auto fn = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(b.m_fn[kFnSleepMode]);
        return static_cast<i32>(fn(static_cast<VkDevice>(b.m_device), static_cast<VkSwapchainKHR>(b.m_swapchain), &info));
    };
    d.sleep = [](void* user, u64 value) -> i32 {
        const VkLowLatency2Binding& b = *static_cast<const VkLowLatency2Binding*>(user);
        VkLatencySleepInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
        info.signalSemaphore = static_cast<VkSemaphore>(b.m_semaphore);
        info.value = value;
        const auto fn = reinterpret_cast<PFN_vkLatencySleepNV>(b.m_fn[kFnSleep]);
        const VkResult r = fn(static_cast<VkDevice>(b.m_device), static_cast<VkSwapchainKHR>(b.m_swapchain), &info);
        if (r != VK_SUCCESS) {
            return static_cast<i32>(r);
        }
        VkSemaphore semaphore = static_cast<VkSemaphore>(b.m_semaphore);
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &semaphore;
        wait.pValues = &value;
        const auto waitFn = reinterpret_cast<PFN_vkWaitSemaphores>(b.m_fn[kFnWait]);
        return static_cast<i32>(waitFn(static_cast<VkDevice>(b.m_device), &wait, UINT64_MAX));
    };
    d.set_marker = [](void* user, u64 presentId, u32 marker) {
        const VkLowLatency2Binding& b = *static_cast<const VkLowLatency2Binding*>(user);
        VkSetLatencyMarkerInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
        info.presentID = presentId;
        info.marker = static_cast<VkLatencyMarkerNV>(marker);
        const auto fn = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(b.m_fn[kFnMarker]);
        fn(static_cast<VkDevice>(b.m_device), static_cast<VkSwapchainKHR>(b.m_swapchain), &info);
    };
    d.get_timings = [](void* user, LatencyFrameReport* out, u32 capacity) -> u32 {
        const VkLowLatency2Binding& b = *static_cast<const VkLowLatency2Binding*>(user);
        constexpr u32 kMax = 64u;
        VkLatencyTimingsFrameReportNV reports[kMax];
        const u32 n = std::min(capacity, kMax);
        for (u32 i = 0; i < n; ++i) {
            reports[i] = VkLatencyTimingsFrameReportNV{};
            reports[i].sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
        }
        VkGetLatencyMarkerInfoNV info{};
        info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
        info.timingCount = n;
        info.pTimings = reports;
        const auto fn = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(b.m_fn[kFnTimings]);
        fn(static_cast<VkDevice>(b.m_device), static_cast<VkSwapchainKHR>(b.m_swapchain), &info);
        const u32 count = std::min(info.timingCount, n);
        for (u32 i = 0; i < count; ++i) {
            const VkLatencyTimingsFrameReportNV& r = reports[i];
            LatencyFrameReport& o = out[i];
            o = LatencyFrameReport{};
            o.frame_id = r.presentID;
            o.input_sample_us = r.inputSampleTimeUs;
            o.sim_start_us = r.simStartTimeUs;
            o.sim_end_us = r.simEndTimeUs;
            o.render_submit_start_us = r.renderSubmitStartTimeUs;
            o.render_submit_end_us = r.renderSubmitEndTimeUs;
            o.present_start_us = r.presentStartTimeUs;
            o.present_end_us = r.presentEndTimeUs;
            o.driver_start_us = r.driverStartTimeUs;
            o.driver_end_us = r.driverEndTimeUs;
            o.os_render_queue_start_us = r.osRenderQueueStartTimeUs;
            o.os_render_queue_end_us = r.osRenderQueueEndTimeUs;
            o.gpu_render_start_us = r.gpuRenderStartTimeUs;
            o.gpu_render_end_us = r.gpuRenderEndTimeUs;
        }
        return count;
    };
#endif
    return d;
}

bool VkAntiLagBinding::init(const VulkanDevice& device, std::string& reason) {
    m_device = nullptr;
    m_update = nullptr;
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        reason = "no device";
        return false;
    }
    if (!extensionEnabled(device, "VK_AMD_anti_lag")) {
        reason = "VK_AMD_anti_lag not enabled on the device";
        return false;
    }
    const VkDevice vk = static_cast<VkDevice>(device.nativeHandle());
    void* fn = reinterpret_cast<void*>(vkGetDeviceProcAddr(vk, "vkAntiLagUpdateAMD"));
    if (fn == nullptr) {
        reason = "vkAntiLagUpdateAMD did not resolve";
        return false;
    }
    m_device = vk;
    m_update = fn;
    reason = "ok";
    return true;
#else
    (void)device;
    reason = "stub backend (no Vulkan)";
    return false;
#endif
}

AntiLagDispatch VkAntiLagBinding::dispatch() {
    AntiLagDispatch d{};
#if defined(FUSE_VULKAN_BACKEND)
    if (m_update == nullptr) {
        return d;
    }
    d.user = this;
    d.update = [](void* user, const AntiLagUpdate& u) {
        const VkAntiLagBinding& b = *static_cast<const VkAntiLagBinding*>(user);
        AntiLagPresentationInfoAmd presentation{};
        presentation.sType = kStructureTypeAntiLagPresentationInfoAmd;
        presentation.stage = u.stage;
        presentation.frameIndex = u.frame_index;
        AntiLagDataAmd data{};
        data.sType = kStructureTypeAntiLagDataAmd;
        data.mode = u.mode;
        data.maxFPS = u.max_fps;
        data.pPresentationInfo = u.has_presentation ? &presentation : nullptr;
        reinterpret_cast<PfnAntiLagUpdateAmd>(b.m_update)(static_cast<VkDevice>(b.m_device), &data);
    };
#endif
    return d;
}

} // namespace fuse::renderer::present
