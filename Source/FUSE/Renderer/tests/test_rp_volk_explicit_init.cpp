// Gates for the volk loader initialisation policy (vk/loader.hpp "Loader initialisation"; RL-4.1
// blocker 1: fuse_rhi must not load vulkan-1.dll from d3d9.dll's DllMain).
//
// Built twice from this file:
//   fuse_rp_volk_explicit_init  target property FUSE_RHI_VOLK_NO_AUTO_INIT=ON (links all of fuse_rhi,
//                               not only the loader TU). On entry to main: the static auto-init did
//                               not run, volk has no loader, every volk global is null, the dispatch
//                               mode is Unavailable, and the loader library is not resident in the
//                               process (dlopen RTLD_NOLOAD / GetModuleHandle). Then, per --mode:
//       explicit   vkloader::initialize() loads it; global entry points appear.
//       lazy       no explicit call: the first VulkanInstance::create loads it.
//       proc-addr  the test opens the loader itself (standing in for DXVK) and hands its
//                  vkGetInstanceProcAddr to vkloader::initializeWithProcAddr (volkInitializeCustom).
//     Each mode then creates an instance and a device and destroys them, with zero validation
//     messages.
//   fuse_rp_volk_auto_init      default link (FUSE_RP_EXPECT_AUTO_INIT): the static auto-init ran
//                               before main and global entry points are loaded (WP-0.2 behaviour).
// Exit 77 = skip (stub build, no loader / ICD).
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/loader.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#define FUSE_RP_HAVE_DLOPEN 1
#endif
#endif

namespace {

using namespace fuse::renderer;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

[[maybe_unused]] int finish(const char* name) {
    if (g_failures != 0) {
        std::printf("%s: %d failure(s)\n", name, g_failures);
        return 1;
    }
    std::printf("%s: OK\n", name);
    return 0;
}

#if defined(FUSE_VULKAN_BACKEND)

/// Whether the platform's Vulkan loader library is mapped into the process. `known` is false where
/// the check is not implemented.
bool loaderResident(bool& known) {
#if defined(_WIN32)
    known = true;
    return GetModuleHandleW(L"vulkan-1.dll") != nullptr;
#elif defined(FUSE_RP_HAVE_DLOPEN)
    known = true;
    void* handle = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (handle == nullptr) {
        handle = dlopen("libvulkan.so", RTLD_LAZY | RTLD_NOLOAD);
    }
    if (handle != nullptr) {
        dlclose(handle);
        return true;
    }
    return false;
#else
    known = false;
    return false;
#endif
}

/// Opens the loader outside volk and returns its vkGetInstanceProcAddr (null when unavailable).
[[maybe_unused]] void* hostGetInstanceProcAddr() {
#if defined(_WIN32)
    HMODULE module = LoadLibraryW(L"vulkan-1.dll");
    return module != nullptr ? reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")) : nullptr;
#elif defined(FUSE_RP_HAVE_DLOPEN)
    void* handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    return handle != nullptr ? dlsym(handle, "vkGetInstanceProcAddr") : nullptr;
#else
    return nullptr;
#endif
}

bool globalsLoaded() {
    return vkGetInstanceProcAddr != nullptr && vkCreateInstance != nullptr &&
           vkEnumerateInstanceLayerProperties != nullptr && vkEnumerateInstanceVersion != nullptr;
}

[[maybe_unused]] bool globalsNull() {
    return vkGetInstanceProcAddr == nullptr && vkCreateInstance == nullptr &&
           vkEnumerateInstanceLayerProperties == nullptr && vkEnumerateInstanceExtensionProperties == nullptr &&
           vkEnumerateInstanceVersion == nullptr && vkCreateDevice == nullptr && vkQueueSubmit == nullptr;
}

/// Instance + device + teardown after the loader is up, zero validation messages.
int useLoader(const char* name) {
    resetVulkanValidationCounters();
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = name;
    instanceDesc.enableValidation = true;
    auto instance = VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: %s: no Vulkan instance\n", name);
        return kSkip;
    }
    expect(vkloader::loaderLoaded() && globalsLoaded(), "loader loaded once an instance exists");
    expect(vkloader::dispatchMode() == vkloader::DispatchMode::Instance, "instance table loaded");
    auto device = VulkanDevice::create(*instance);
    if (device == nullptr || !device->isValid()) {
        std::printf("SKIP: %s: no Vulkan device\n", name);
        return kSkip;
    }
    std::printf("device: %s | %s\n", device->info().deviceName.c_str(), device->info().caps.summary().c_str());
    expect(vkloader::dispatchMode() == vkloader::DispatchMode::Device &&
               vkloader::dispatchDevice() == device->nativeHandle(),
           "device-level dispatch");
    device->waitIdle();
    device.reset();
    instance.reset();
    expect(vkloader::dispatchMode() == vkloader::DispatchMode::Global, "back to global entry points");
    const VulkanValidationCounters counters = vulkanValidationCounters();
    expect(counters.errors == 0 && counters.warnings == 0,
           "zero validation messages (errors " + std::to_string(counters.errors) + ", warnings " +
               std::to_string(counters.warnings) + ": " + counters.lastError + ")");
    return 0;
}

#endif

} // namespace

int main(int argc, char** argv) {
    std::string mode = "explicit";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        }
    }
#if !defined(FUSE_VULKAN_BACKEND)
    (void)mode;
    expect(!vkloader::autoInitialized() && !vkloader::loaderLoaded(), "stub: nothing loaded");
    expect(!vkloader::initialize() && !vkloader::initializeWithProcAddr(nullptr) && !vkloader::loaderLoaded(),
           "stub: initialisation reports no loader");
    if (g_failures != 0) {
        return 1;
    }
    std::printf("SKIP: Vulkan backend disabled (stub loader API checked)\n");
    return kSkip;
#elif defined(FUSE_RP_EXPECT_AUTO_INIT)
    // Default link: WP-0.2 behaviour, the loader is up before main.
    (void)mode;
    const char* name = "fuse_rp_volk_auto_init";
    expect(vkloader::autoInitialized(), "static auto-init ran before main");
    if (!vkloader::loaderLoaded()) {
        std::printf("SKIP: %s: no Vulkan loader library\n", name);
        return g_failures != 0 ? 1 : kSkip;
    }
    expect(globalsLoaded(), "global entry points loaded before main");
    expect(vkloader::dispatchMode() == vkloader::DispatchMode::Global, "global dispatch before any instance");
    bool known = false;
    expect(loaderResident(known) || !known, "loader library resident");
    const int rc = useLoader(name);
    return rc == kSkip ? kSkip : finish(name);
#else
    const char* name = "fuse_rp_volk_explicit_init";
    // Nothing may have touched the loader before main: fuse_rhi is linked in full, only the
    // auto-init member is left out (FUSE_RHI_VOLK_NO_AUTO_INIT on this target).
    expect(!vkloader::autoInitialized(), "no static auto-init in this image");
    expect(!vkloader::loaderLoaded(), "volk has no loader before the first explicit call");
    expect(vkloader::dispatchMode() == vkloader::DispatchMode::Unavailable, "dispatch mode Unavailable");
    expect(globalsNull(), "every volk global is still null");
    bool known = false;
    const bool residentBefore = loaderResident(known);
#if FUSE_RP_LOADER_LINKED_BY_WSI
    // fuse_core's WSI imports the loader: mapped at startup, independent of volk.
    const char* residency = "not checked (the platform WSI links the loader)";
#else
    expect(!residentBefore, "loader library not resident before the first explicit call");
    const char* residency = "checked";
#endif
    std::printf("before first call: volk loaded %d, loader library resident %s (%s)\n",
                vkloader::loaderLoaded() ? 1 : 0, known ? (residentBefore ? "yes" : "no") : "unknown", residency);

    if (mode == "explicit") {
        if (!vkloader::initialize()) {
            std::printf("SKIP: %s: no Vulkan loader library\n", name);
            return g_failures != 0 ? 1 : kSkip;
        }
        expect(vkloader::loaderLoaded() && !vkloader::loadedThroughProcAddr(), "initialize() loaded the library");
        expect(globalsLoaded(), "global entry points after initialize()");
        expect(vkloader::dispatchMode() == vkloader::DispatchMode::Global, "global dispatch");
        expect(loaderResident(known) || !known, "loader library resident after initialize()");
    } else if (mode == "lazy") {
        // The first VulkanInstance::create is the first call (useLoader checks the result).
    } else if (mode == "proc-addr") {
        void* gipa = hostGetInstanceProcAddr();
        if (gipa == nullptr) {
            std::printf("SKIP: %s: cannot open the loader outside volk here\n", name);
            return g_failures != 0 ? 1 : kSkip;
        }
        expect(!vkloader::initializeWithProcAddr(nullptr) && !vkloader::loaderLoaded(),
               "a null vkGetInstanceProcAddr loads nothing");
        expect(vkloader::initializeWithProcAddr(gipa), "initializeWithProcAddr");
        expect(vkloader::loaderLoaded() && vkloader::loadedThroughProcAddr(), "loaded through the host's entry point");
        expect(reinterpret_cast<void*>(vkGetInstanceProcAddr) == gipa && globalsLoaded(),
               "volk resolves globals through the host's vkGetInstanceProcAddr");
        expect(vkloader::initialize() && vkloader::loadedThroughProcAddr(), "later initialize() keeps the host loader");
    } else {
        std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
        return 2;
    }
    expect(!vkloader::autoInitialized(), "still no auto-init");
    const int rc = useLoader(name);
    if (rc == kSkip) {
        return g_failures != 0 ? 1 : kSkip;
    }
    return finish(name);
#endif
}
