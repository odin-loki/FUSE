// Gate: "Physical device selection picks the RTX 3090 correctly over any integrated GPU" — the
// selection policy half (the RTX 3090 half needs that hardware and stays a manual check).
//
// CI has one Lavapipe device, so the test builds a multi-GPU machine out of it:
//  1. The Lavapipe ICD is registered twice (two manifests naming the same library through two
//     distinct path strings; the loader dedups ICDs by path only), giving two real, independent
//     VkPhysicalDevices.
//  2. The test-only layer VK_LAYER_FUSE_spoof_multi_gpu (tests/vk_layer_spoof_multi_gpu.cpp), loaded
//     below VK_LAYER_KHRONOS_validation, rewrites vkEnumeratePhysicalDevices (order, which devices
//     are visible) and each device's type, name, device-local heap size, maxImageDimension2D,
//     apiVersion, timelineSemaphore and graphics queue support per FUSE_MULTI_GPU_SPEC.
// Each scenario creates a fresh instance under a spec and checks `VulkanDevice::create` picked the
// expected device (index, VkPhysicalDevice handle, spoofed type/name): discrete over integrated in
// both enumeration orders, fallback to integrated / first suitable, rejection of devices missing a
// hard requirement (timeline semaphores, Vulkan 1.2, a graphics queue), the tie-breaks (device-local
// memory, then maxImageDimension2D, then enumeration order), `preferDiscreteGpu = false`, and the
// no-suitable-device failure. The picked device must then work: a fill -> copy -> host readback on
// its graphics queue, signalled through a timeline semaphore, verified byte-exact. With the Khronos
// validation layer installed any validation warning or error fails the test.
//
// Exit 77 (skip) in the stub build, off Linux, or without the Lavapipe ICD.
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#include <climits>
#include <fstream>
#include <sstream>
#endif

namespace {

constexpr int kSkip = 77;

#if defined(FUSE_VULKAN_BACKEND) && defined(__linux__)

using fuse::u32;
using fuse::u64;

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSpoofLayer = "VK_LAYER_FUSE_spoof_multi_gpu";
constexpr u64 kTimeoutNs = 5000000000ull;

int g_failures = 0;
u32 g_messages = 0;

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                 data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                 data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
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

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Absolute path of the ICD library named by the manifest `manifestPath` (loaded with the dynamic
/// linker's own search rules, as the loader does). Empty when it cannot be resolved.
std::string resolveIcdLibrary(const std::string& manifestPath) {
    const std::string json = readFile(manifestPath);
    const size_t key = json.find("\"library_path\"");
    if (key == std::string::npos) {
        return {};
    }
    const size_t open = json.find('"', json.find(':', key) + 1);
    const size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    std::string library = json.substr(open + 1, close - open - 1);
    if (library.find('/') != std::string::npos && library[0] != '/') {
        const size_t slash = manifestPath.rfind('/');
        library = (slash == std::string::npos ? std::string(".") : manifestPath.substr(0, slash)) + "/" + library;
    }
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return {};
    }
    std::string resolved;
    link_map* map = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr && map->l_name != nullptr) {
        char real[PATH_MAX];
        if (realpath(map->l_name, real) != nullptr) {
            resolved = real;
        }
    }
    dlclose(handle);
    return resolved;
}

/// Writes two ICD manifests for the same library under `dir` and points the loader at them.
/// Returns 0, kSkip (library not resolvable) or 1 (write failure).
int registerIcdTwice(const std::string& dir) {
    const char* icds = std::getenv("VK_ICD_FILENAMES");
    if (icds == nullptr || icds[0] == '\0') {
        icds = std::getenv("VK_DRIVER_FILES");
    }
    std::string manifest = icds != nullptr && icds[0] != '\0' ? icds : "/usr/share/vulkan/icd.d/lvp_icd.json";
    manifest = manifest.substr(0, manifest.find(':'));
    const std::string library = resolveIcdLibrary(manifest);
    if (library.empty()) {
        std::printf("SKIP: cannot resolve the ICD library of %s\n", manifest.c_str());
        return kSkip;
    }
    const size_t slash = library.rfind('/');
    // Same file, distinct path string: the loader registers a second ICD for it.
    const std::string alias = library.substr(0, slash) + "/." + library.substr(slash);
    const std::string paths[2] = {library, alias};
    std::string list;
    for (int i = 0; i < 2; ++i) {
        const std::string path = dir + "/fuse_spoof_icd_" + std::to_string(i) + ".json";
        std::ofstream out(path);
        out << "{\n    \"file_format_version\": \"1.0.1\",\n    \"ICD\": {\n        \"library_path\": \"" << paths[i]
            << "\",\n        \"api_version\": \"1.3.0\"\n    }\n}\n";
        if (!out) {
            std::fprintf(stderr, "FAIL: cannot write %s\n", path.c_str());
            return 1;
        }
        list += (i == 0 ? "" : ":") + path;
    }
    setenv("VK_ICD_FILENAMES", list.c_str(), 1);
    unsetenv("VK_DRIVER_FILES");
    std::printf("ICD %s registered twice (%s)\n", library.c_str(), list.c_str());
    return 0;
}

struct Scenario {
    const char* spec;
    bool preferDiscrete;
    int expected; ///< presented index, or -1: no suitable device
    VkPhysicalDeviceType expectedType;
    const char* what;
};

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

bool createHostBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage,
                      HostBuffer& out) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0 && (mem.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
        }
    }
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    return vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
}

void destroyHostBuffer(VkDevice device, HostBuffer& b) {
    if (b.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, b.buffer, nullptr);
    }
    if (b.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, b.memory, nullptr);
    }
    b = {};
}

/// Fill -> copy -> host readback on the graphics queue, completion observed on a timeline semaphore.
bool exerciseDevice(const fuse::renderer::VulkanDevice& device, u32 pattern) {
    const auto physical = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    const auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    const auto queue = static_cast<VkQueue>(device.queues().graphics);
    constexpr VkDeviceSize kBytes = 64 * 1024;
    HostBuffer source;
    HostBuffer readback;
    bool ok = createHostBuffer(physical, vkDevice, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               source) &&
              createHostBuffer(physical, vkDevice, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    if (ok) {
        std::memset(readback.mapped, 0, kBytes);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
        ok = vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(vkDevice, &alloc, &cmd) == VK_SUCCESS;
    }
    if (ok) {
        VkSemaphoreTypeCreateInfo typeInfo{};
        typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = 0;
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semInfo.pNext = &typeInfo;
        ok = vkCreateSemaphore(vkDevice, &semInfo, nullptr, &timeline) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        vkCmdFillBuffer(cmd, source.buffer, 0, kBytes, pattern);
        VkMemoryBarrier fillToCopy{};
        fillToCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fillToCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fillToCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &fillToCopy, 0,
                             nullptr, 0, nullptr);
        const VkBufferCopy region{0, 0, kBytes};
        vkCmdCopyBuffer(cmd, source.buffer, readback.buffer, 1, &region);
        VkMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr,
                             0, nullptr);
        ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }
    if (ok) {
        const u64 signalValue = 1;
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &signalValue;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &timelineInfo;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &timeline;
        ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &timeline;
        wait.pValues = &signalValue;
        ok = ok && vkWaitSemaphores(vkDevice, &wait, kTimeoutNs) == VK_SUCCESS;
    }
    if (ok) {
        const auto* words = static_cast<const u32*>(readback.mapped);
        for (VkDeviceSize i = 0; i < kBytes / sizeof(u32); ++i) {
            ok = ok && words[i] == pattern;
        }
    }
    vkDeviceWaitIdle(vkDevice);
    if (timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(vkDevice, timeline, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
    }
    destroyHostBuffer(vkDevice, source);
    destroyHostBuffer(vkDevice, readback);
    return ok;
}

/// One fresh instance under `scenario.spec`. Returns false when the instance could not be created.
bool runScenario(const Scenario& scenario, bool validation, u32 index) {
    setenv("FUSE_MULTI_GPU_SPEC", scenario.spec, 1);
    const u32 messagesBefore = g_messages;
    std::printf("[%s] %s%s\n", scenario.spec, scenario.what, scenario.preferDiscrete ? "" : " (preferDiscreteGpu = false)");

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_b2_physical_device_selection";
    instanceDesc.enableValidation = validation;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        return false;
    }
    const auto vkInstance = static_cast<VkInstance>(instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (validation && createMessenger != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &info, nullptr, &messenger);
    }

    // What the application sees, independently of the RHI.
    u32 count = 0;
    vkEnumeratePhysicalDevices(vkInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> presented(count);
    vkEnumeratePhysicalDevices(vkInstance, &count, presented.data());
    u32 specEntries = 1;
    for (const char* c = scenario.spec; *c != '\0'; ++c) {
        specEntries += *c == ';' ? 1u : 0u;
    }
    expectTrue(count == specEntries, std::string(scenario.spec) + ": layer presents one device per spec entry");
    expectTrue(count < 2 || presented[0] != presented[1], std::string(scenario.spec) + ": presented devices are distinct");

    fuse::renderer::VulkanDeviceDesc deviceDesc{};
    deviceDesc.preferDiscreteGpu = scenario.preferDiscrete;
    auto device = fuse::renderer::VulkanDevice::create(*instance, deviceDesc);
    const fuse::renderer::VulkanDeviceInfo& info = device->info();
    std::printf("  selection: %s\n  -> %s\n", info.selection.c_str(), info.message.c_str());
    const std::string tag = std::string(scenario.spec) + ": ";
    expectTrue(info.physicalDeviceCount == count, tag + "RHI enumerated the presented devices");
    if (scenario.expected < 0) {
        expectTrue(!device->isValid(), tag + "no device created when none is suitable");
        expectTrue(info.physicalDeviceIndex == UINT32_MAX, tag + "no physical device index");
        expectTrue(info.message.find("No suitable Vulkan physical device") != std::string::npos,
                   tag + "failure names the missing suitability");
    } else {
        const auto expected = static_cast<u32>(scenario.expected);
        expectTrue(device->isValid(), tag + "device created");
        expectTrue(info.physicalDeviceIndex == expected,
                   tag + "picked presented device #" + std::to_string(expected) + " (got #" +
                       std::to_string(static_cast<int>(info.physicalDeviceIndex)) + ")");
        expectTrue(expected < count && device->nativePhysicalDevice() == static_cast<void*>(presented[expected]),
                   tag + "VkPhysicalDevice handle is the expected one");
        expectTrue(info.deviceType == static_cast<u32>(scenario.expectedType), tag + "picked device has the expected type");
        VkPhysicalDeviceProperties props{};
        if (expected < count) {
            vkGetPhysicalDeviceProperties(presented[expected], &props);
        }
        expectTrue(info.deviceName == props.deviceName, tag + "device name matches the picked device");
        expectTrue(info.timelineSemaphore, tag + "timeline semaphores enabled on the picked device");
        if (device->isValid()) {
            expectTrue(exerciseDevice(*device, 0xC0DE0000u + index), tag + "picked device runs work (fill/copy/readback)");
        }
    }
    device.reset();
    if (messenger != VK_NULL_HANDLE) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }
    instance.reset();
    expectTrue(g_messages == messagesBefore, tag + "zero validation messages");
    return true;
}

int run(const char* layerDir) {
    if (layerDir == nullptr) {
        std::fprintf(stderr, "FAIL: --layer-dir required\n");
        return 1;
    }
    if (const int status = registerIcdTwice(layerDir); status != 0) {
        return status;
    }
    std::string path = layerDir;
    const char* existing = std::getenv("VK_ADD_LAYER_PATH");
    if (existing != nullptr && existing[0] != '\0') {
        path += ":";
        path += existing;
    }
    setenv("VK_ADD_LAYER_PATH", path.c_str(), 1);
    // Mesa's implicit VK_LAYER_MESA_device_select sits above the spoof layer and re-sorts the
    // devices it sees (CPU devices last, its own "default" first), which would undo the spec's
    // enumeration order: the gate is about the RHI's policy, so switch it off.
    setenv("NODEVICE_SELECT", "1", 1);
    const bool validation = layerAvailable(kValidationLayer);
    std::string layers = validation ? std::string(kValidationLayer) + ":" : std::string();
    layers += kSpoofLayer; // application -> driver order: validation sees the spoofed devices
    setenv("VK_INSTANCE_LAYERS", layers.c_str(), 1);
    if (validation) {
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        std::printf("note: %s not installed — running without validation\n", kValidationLayer);
    }
    if (!layerAvailable(kSpoofLayer)) {
        std::fprintf(stderr, "FAIL: %s not found under %s\n", kSpoofLayer, layerDir);
        return 1;
    }

    constexpr VkPhysicalDeviceType D = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    constexpr VkPhysicalDeviceType I = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    constexpr VkPhysicalDeviceType C = VK_PHYSICAL_DEVICE_TYPE_CPU;
    const Scenario scenarios[] = {
        // Discrete over integrated, in both enumeration orders (and with the real devices swapped).
        {"integrated;discrete", true, 1, D, "discrete enumerated second"},
        {"discrete;integrated", true, 0, D, "discrete enumerated first"},
        {"integrated,src=1;discrete,src=0", true, 1, D, "discrete second, real devices swapped"},
        {"discrete,src=1;integrated,src=0", true, 0, D, "discrete first, real devices swapped"},
        {"integrated,vram=8192;discrete,vram=1024", true, 1, D, "type outranks device-local memory"},
        // Fallbacks when no discrete device is usable.
        {"cpu;integrated", true, 1, I, "no discrete: integrated over CPU"},
        {"integrated", true, 0, I, "integrated only"},
        {"cpu", true, 0, C, "CPU only"},
        // Required-feature rejection: a better-ranked device missing a hard requirement loses.
        {"discrete,notimeline;integrated", true, 1, I, "discrete without timeline semaphores rejected"},
        {"integrated;discrete,api=1.1", true, 0, I, "discrete on Vulkan 1.1 rejected"},
        {"discrete,nographics;integrated", true, 1, I, "discrete without a graphics queue rejected"},
        {"discrete,notimeline;integrated,api=1.1", true, -1, D, "nothing suitable: creation fails"},
        {"discrete,nographics;integrated,notimeline", true, -1, D, "nothing suitable (no graphics / no timeline)"},
        // Tie-breaks between equal types: device-local memory, then maxImageDimension2D, then order.
        {"discrete,vram=2048;discrete,vram=4096", true, 1, D, "more device-local memory wins (second)"},
        {"discrete,vram=4096;discrete,vram=2048", true, 0, D, "more device-local memory wins (first)"},
        {"discrete,vram=4096,dim2d=8192;discrete,vram=4096", true, 1, D, "equal memory: larger 2D limit wins"},
        {"discrete,vram=4096;discrete,vram=4096,dim2d=8192", true, 0, D, "equal memory: larger 2D limit wins (first)"},
        {"discrete,vram=4096;discrete,vram=4096", true, 0, D, "full tie: enumeration order"},
        {"integrated,vram=2048;integrated,vram=4096", true, 1, I, "tie-break among integrated devices"},
        // preferDiscreteGpu = false: first suitable device in enumeration order.
        {"integrated;discrete", false, 0, I, "no preference: first device"},
        {"integrated,notimeline;discrete", false, 1, D, "no preference: first suitable device"},
    };
    u32 index = 0;
    for (const Scenario& s : scenarios) {
        if (!runScenario(s, validation, index++)) {
            if (index == 1) {
                std::printf("SKIP: no Vulkan instance\n");
                return kSkip;
            }
            expectTrue(false, std::string(s.spec) + ": instance creation");
        }
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_b2_physical_device_selection: OK (%u scenarios, %s validation)\n", index,
                validation ? "with" : "without");
    return 0;
}

#endif // FUSE_VULKAN_BACKEND && __linux__

} // namespace

int main(int argc, char** argv) {
#if defined(FUSE_VULKAN_BACKEND) && defined(__linux__)
    const char* layerDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layer-dir") == 0 && i + 1 < argc) {
            layerDir = argv[++i];
        }
    }
    return run(layerDir);
#else
    (void)argc;
    (void)argv;
    std::printf("SKIP: needs the Vulkan backend on Linux (ICD registered twice + spoof layer)\n");
    return kSkip;
#endif
}
