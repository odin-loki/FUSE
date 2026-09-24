// Vulkan loading through volk (WP-0.2). See include/fuse/renderer/vk/loader.hpp for the policy.
#include <fuse/renderer/vk/loader.hpp>

#if defined(FUSE_VULKAN_BACKEND)
// This TU holds volk's implementation (volk.c, via VOLK_IMPLEMENTATION). No platform surface
// entry points: volk would define them as function-pointer *data* symbols with the same names as
// the loader's exports, and fuse_core's WSI code (window_wsi.cpp, x11_window.cpp) still calls
// vkCreateWin32SurfaceKHR / vkCreateXlibSurfaceKHR through the linked loader. A call bound to a
// data symbol would jump into the pointer's bytes, so keep those names out of volk's definitions.
#undef VK_USE_PLATFORM_WIN32_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT
#undef VK_USE_PLATFORM_XCB_KHR
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_ANDROID_KHR
#undef VK_USE_PLATFORM_METAL_EXT
#undef VK_USE_PLATFORM_MACOS_MVK
#undef VK_USE_PLATFORM_IOS_MVK
#undef VK_USE_PLATFORM_DIRECTFB_EXT
#undef VK_USE_PLATFORM_SCREEN_QNX
#undef VK_USE_PLATFORM_GGP
#undef VK_USE_PLATFORM_FUCHSIA
#undef VK_USE_PLATFORM_VI_NN
// Declarations first (the shim: real vulkan.h without prototypes, then volk.h). volk.h's
// implementation section sits outside its include guard, so it must be requested only after that.
#include <vulkan/vulkan.h>
#define VOLK_IMPLEMENTATION
#include <volk.h>

#include <fuse_volk_pin.h>

static_assert(VOLK_HEADER_VERSION == FUSE_VOLK_VERSION_PATCH,
              "Engine/lib/volk/volk.h does not match the version pinned in Engine/lib/volk/VERSION");

#include <algorithm>
#include <mutex>
#include <vector>
#endif

namespace fuse::renderer::vkloader {

#if defined(FUSE_VULKAN_BACKEND)
namespace {

struct DeviceEntry {
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
};

struct LoaderState {
    std::mutex mutex;
    bool initTried = false;
    bool initOk = false;
    std::vector<VkInstance> instances; // registration order; the newest is the default table source
    std::vector<DeviceEntry> devices;
    DispatchMode mode = DispatchMode::Unavailable;
    VkDevice loadedDevice = VK_NULL_HANDLE;
    u32 reloads = 0;
    ReloadHook hook = nullptr;
    void* hookUser = nullptr;
};

LoaderState& state() {
    static LoaderState s;
    return s;
}

bool initializeLocked(LoaderState& s) {
    if (!s.initTried) {
        s.initTried = true;
        s.initOk = volkInitialize() == VK_SUCCESS;
        s.mode = s.initOk ? DispatchMode::Global : DispatchMode::Unavailable;
    }
    return s.initOk;
}

/// Reloads volk's tables for the current registrations (policy in loader.hpp) and re-applies the
/// reload hook. Caller holds `s.mutex`.
void reloadLocked(LoaderState& s) {
    if (!s.initOk) {
        return;
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    if (s.devices.size() == 1u) {
        device = s.devices.front().device;
        instance = s.devices.front().instance;
    }
    if (instance == VK_NULL_HANDLE && !s.instances.empty()) {
        instance = s.instances.back();
    }
    if (instance == VK_NULL_HANDLE) {
        // Nothing to load from: the instance/device pointers keep their last values, and no code
        // may call them until another instance registers (which reloads everything).
        s.mode = DispatchMode::Global;
        s.loadedDevice = VK_NULL_HANDLE;
        return;
    }
    // Instance-level entry points plus every device entry point as a loader trampoline.
    volkLoadInstance(instance);
    if (device != VK_NULL_HANDLE) {
        volkLoadDevice(device);
    }
    s.loadedDevice = device;
    s.mode = device != VK_NULL_HANDLE ? DispatchMode::Device : DispatchMode::Instance;
    ++s.reloads;
    if (s.hook != nullptr) {
        s.hook(s.hookUser);
    }
}

// Global entry points (vkCreateInstance, vkEnumerateInstance*) must be callable before any
// fuse_rhi object exists: tests probe layers first. This object file defines every volk pointer,
// so it is always linked wherever a vk* name is used, and this runs during its static init.
[[maybe_unused]] const bool g_loaderAutoInit = initialize();

} // namespace
#endif

bool initialize() {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return initializeLocked(s);
#else
    return false;
#endif
}

u32 loaderInstanceVersion() {
#if defined(FUSE_VULKAN_BACKEND)
    return initialize() ? volkGetInstanceVersion() : 0u;
#else
    return 0u;
#endif
}

void registerInstance(void* vkInstance) {
#if defined(FUSE_VULKAN_BACKEND)
    if (vkInstance == nullptr) {
        return;
    }
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!initializeLocked(s)) {
        return;
    }
    const auto instance = static_cast<VkInstance>(vkInstance);
    if (std::find(s.instances.begin(), s.instances.end(), instance) == s.instances.end()) {
        s.instances.push_back(instance);
    }
    reloadLocked(s);
#else
    (void)vkInstance;
#endif
}

void unregisterInstance(void* vkInstance) {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto instance = static_cast<VkInstance>(vkInstance);
    s.instances.erase(std::remove(s.instances.begin(), s.instances.end(), instance), s.instances.end());
    // Devices must be destroyed before their instance; drop any that were not, so no table is
    // ever loaded from a dead instance.
    s.devices.erase(std::remove_if(s.devices.begin(), s.devices.end(),
                                   [instance](const DeviceEntry& e) { return e.instance == instance; }),
                    s.devices.end());
    reloadLocked(s);
#else
    (void)vkInstance;
#endif
}

void registerDevice(void* vkDevice, void* vkInstance) {
#if defined(FUSE_VULKAN_BACKEND)
    if (vkDevice == nullptr) {
        return;
    }
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!initializeLocked(s)) {
        return;
    }
    const auto device = static_cast<VkDevice>(vkDevice);
    const auto instance = static_cast<VkInstance>(vkInstance);
    if (instance != VK_NULL_HANDLE &&
        std::find(s.instances.begin(), s.instances.end(), instance) == s.instances.end()) {
        s.instances.push_back(instance);
    }
    const bool known = std::any_of(s.devices.begin(), s.devices.end(),
                                   [device](const DeviceEntry& e) { return e.device == device; });
    if (!known) {
        s.devices.push_back(DeviceEntry{device, instance != VK_NULL_HANDLE ? instance : volkGetLoadedInstance()});
    }
    reloadLocked(s);
#else
    (void)vkDevice;
    (void)vkInstance;
#endif
}

void unregisterDevice(void* vkDevice) {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto device = static_cast<VkDevice>(vkDevice);
    s.devices.erase(std::remove_if(s.devices.begin(), s.devices.end(),
                                   [device](const DeviceEntry& e) { return e.device == device; }),
                    s.devices.end());
    reloadLocked(s);
#else
    (void)vkDevice;
#endif
}

DispatchMode dispatchMode() {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.mode;
#else
    return DispatchMode::Unavailable;
#endif
}

void* dispatchDevice() {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.mode == DispatchMode::Device ? static_cast<void*>(s.loadedDevice) : nullptr;
#else
    return nullptr;
#endif
}

void* dispatchInstance() {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.mode == DispatchMode::Instance || s.mode == DispatchMode::Device ? static_cast<void*>(volkGetLoadedInstance())
                                                                              : nullptr;
#else
    return nullptr;
#endif
}

u32 reloadCount() {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.reloads;
#else
    return 0u;
#endif
}

void setReloadHook(ReloadHook hook, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    LoaderState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const bool hadHook = s.hook != nullptr;
    s.hook = hook;
    s.hookUser = user;
    if (s.mode == DispatchMode::Instance || s.mode == DispatchMode::Device) {
        if (hadHook) {
            reloadLocked(s); // fresh tables (drops the previous wrappers), then the new hook
        } else if (hook != nullptr) {
            hook(user);
        }
    }
#else
    (void)hook;
    (void)user;
#endif
}

} // namespace fuse::renderer::vkloader
