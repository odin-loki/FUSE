/* FUSE Relight RL-0.3 environment probe (C99, Windows PE, x64 and i686).
 *
 * Loads vulkan-1.dll at run time (no Vulkan SDK headers or import library: the handful of types
 * used are declared below from the Vulkan specification), creates an instance, and prints every
 * physical device with its name, API version and the ray-tracing extensions Relight's path
 * tracer needs. Under Wine + Xvfb the device is Mesa's Lavapipe ("llvmpipe ...") reached through
 * winevulkan, which is what the Relight Wine CI gates run on (plan §6.1).
 *
 * Usage: probe_vulkan.exe [--expect-device <substring>] [--no-rt]
 *   exit 0   a device was found (matching --expect-device, if given) that exposes all required
 *            RT extensions (unless --no-rt);
 *   exit 1   the requirement was not met (message on stderr);
 *   exit 2   vulkan-1.dll or vkCreateInstance failed (the environment is broken, not missing:
 *            the runner already exits 77 when Wine or Xvfb is absent).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_VKAPI __stdcall /* VKAPI_CALL on Windows; matters on i686 */

typedef int32_t VkResult;
typedef uint32_t VkBool32;
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef void (PROBE_VKAPI* PFN_vkVoidFunction)(void);

enum {
    PROBE_VK_SUCCESS = 0,
    PROBE_VK_INCOMPLETE = 5,
    PROBE_VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    PROBE_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    PROBE_VK_MAX_EXTENSION_NAME_SIZE = 256,
    PROBE_VK_MAX_PHYSICAL_DEVICE_NAME_SIZE = 256
};

typedef struct {
    int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} ProbeVkApplicationInfo;

typedef struct {
    int32_t sType;
    const void* pNext;
    uint32_t flags;
    const ProbeVkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} ProbeVkInstanceCreateInfo;

typedef struct {
    char extensionName[PROBE_VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} ProbeVkExtensionProperties;

/* Leading members of VkPhysicalDeviceProperties; the rest (pipelineCacheUUID, limits, sparse
 * properties: < 1 KiB) lands in the padding, which is sized well above the real struct. */
typedef struct {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    int32_t deviceType;
    char deviceName[PROBE_VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t rest[4096];
} ProbeVkPhysicalDeviceProperties;

typedef PFN_vkVoidFunction(PROBE_VKAPI* PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult(PROBE_VKAPI* PFN_vkCreateInstance)(const ProbeVkInstanceCreateInfo*, const void*, VkInstance*);
typedef void(PROBE_VKAPI* PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult(PROBE_VKAPI* PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void(PROBE_VKAPI* PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, ProbeVkPhysicalDeviceProperties*);
typedef VkResult(PROBE_VKAPI* PFN_vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*,
                                                                         ProbeVkExtensionProperties*);

static const char* const kRtExtensions[] = {
    "VK_KHR_acceleration_structure",
    "VK_KHR_ray_query",
    "VK_KHR_ray_tracing_pipeline",
    "VK_KHR_deferred_host_operations",
    "VK_KHR_buffer_device_address",
};
/* Reported when present, not required (Lavapipe has these today; real GPUs may differ). */
static const char* const kInfoExtensions[] = {
    "VK_KHR_ray_tracing_position_fetch",
    "VK_EXT_mesh_shader",
    "VK_KHR_swapchain",
    "VK_EXT_opacity_micromap",
};

#define PROBE_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

static int hasExtension(const ProbeVkExtensionProperties* exts, uint32_t count, const char* name)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        if (strncmp(exts[i].extensionName, name, PROBE_VK_MAX_EXTENSION_NAME_SIZE) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char* deviceTypeName(int32_t t)
{
    switch (t) {
    case 1: return "integrated";
    case 2: return "discrete";
    case 3: return "virtual";
    case 4: return "cpu";
    default: return "other";
    }
}

int main(int argc, char** argv)
{
    const char* expectDevice = NULL;
    int requireRt = 1;
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--expect-device") == 0 && i + 1 < argc) {
            expectDevice = argv[++i];
        } else if (strcmp(argv[i], "--no-rt") == 0) {
            requireRt = 0;
        } else {
            fprintf(stderr, "usage: %s [--expect-device <substring>] [--no-rt]\n", argv[0]);
            return 2;
        }
    }

    printf("FUSE Relight env probe (%s PE)\n", sizeof(void*) == 8 ? "x64" : "x86");

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) {
        fprintf(stderr, "FAIL: LoadLibrary(vulkan-1.dll) error %lu\n", (unsigned long)GetLastError());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) {
        fprintf(stderr, "FAIL: vulkan-1.dll has no vkGetInstanceProcAddr\n");
        return 2;
    }
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)(void*)gipa(NULL, "vkCreateInstance");
    if (!createInstance) {
        fprintf(stderr, "FAIL: no vkCreateInstance\n");
        return 2;
    }

    ProbeVkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = PROBE_VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "fuse_relight_env_probe";
    app.pEngineName = "FUSE";
    app.apiVersion = (1u << 22) | (3u << 12); /* VK_API_VERSION_1_3 */
    ProbeVkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = PROBE_VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = NULL;
    VkResult r = createInstance(&ici, NULL, &instance);
    printf("vkCreateInstance = %d\n", (int)r);
    if (r != PROBE_VK_SUCCESS || !instance) {
        fprintf(stderr, "FAIL: vkCreateInstance returned %d (under Wine: is DISPLAY an X server and "
                        "VK_ICD_FILENAMES a Lavapipe ICD?)\n", (int)r);
        return 2;
    }

    PFN_vkDestroyInstance destroyInstance = (PFN_vkDestroyInstance)(void*)gipa(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumDevices =
        (PFN_vkEnumeratePhysicalDevices)(void*)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties getProps =
        (PFN_vkGetPhysicalDeviceProperties)(void*)gipa(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties enumExts =
        (PFN_vkEnumerateDeviceExtensionProperties)(void*)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    if (!destroyInstance || !enumDevices || !getProps || !enumExts) {
        fprintf(stderr, "FAIL: missing core instance entry points\n");
        return 2;
    }

    uint32_t deviceCount = 0;
    r = enumDevices(instance, &deviceCount, NULL);
    printf("physical devices: %u\n", (unsigned)deviceCount);
    if (r != PROBE_VK_SUCCESS || deviceCount == 0) {
        fprintf(stderr, "FAIL: no Vulkan physical devices (result %d)\n", (int)r);
        destroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(deviceCount, sizeof *devices);
    if (!devices) {
        destroyInstance(instance, NULL);
        return 2;
    }
    r = enumDevices(instance, &deviceCount, devices);
    if (r != PROBE_VK_SUCCESS && r != PROBE_VK_INCOMPLETE) {
        fprintf(stderr, "FAIL: vkEnumeratePhysicalDevices returned %d\n", (int)r);
        free(devices);
        destroyInstance(instance, NULL);
        return 2;
    }

    int satisfied = 0;
    uint32_t d;
    for (d = 0; d < deviceCount; ++d) {
        static ProbeVkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof props);
        getProps(devices[d], &props);
        props.deviceName[PROBE_VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';

        uint32_t extCount = 0;
        ProbeVkExtensionProperties* exts = NULL;
        if (enumExts(devices[d], NULL, &extCount, NULL) == PROBE_VK_SUCCESS && extCount > 0) {
            exts = (ProbeVkExtensionProperties*)calloc(extCount, sizeof *exts);
            if (exts && enumExts(devices[d], NULL, &extCount, exts) < 0) {
                extCount = 0;
            }
        }
        if (!exts) {
            extCount = 0;
        }

        printf("device %u: \"%s\" type=%s api=%u.%u.%u vendor=0x%04x extensions=%u\n", (unsigned)d,
               props.deviceName, deviceTypeName(props.deviceType), (unsigned)(props.apiVersion >> 22),
               (unsigned)((props.apiVersion >> 12) & 0x3ffu), (unsigned)(props.apiVersion & 0xfffu),
               (unsigned)props.vendorID, (unsigned)extCount);
        int rtOk = 1;
        size_t e;
        for (e = 0; e < PROBE_COUNTOF(kRtExtensions); ++e) {
            int has = hasExtension(exts, extCount, kRtExtensions[e]);
            printf("  %-36s %s\n", kRtExtensions[e], has ? "yes" : "MISSING");
            rtOk &= has;
        }
        for (e = 0; e < PROBE_COUNTOF(kInfoExtensions); ++e) {
            printf("  %-36s %s (info)\n", kInfoExtensions[e],
                   hasExtension(exts, extCount, kInfoExtensions[e]) ? "yes" : "no");
        }
        free(exts);

        int nameOk = !expectDevice || strstr(props.deviceName, expectDevice) != NULL;
        if (nameOk && (rtOk || !requireRt)) {
            satisfied = 1;
        }
    }
    free(devices);
    destroyInstance(instance, NULL);

    if (!satisfied) {
        fprintf(stderr, "FAIL: no device%s%s%s%s\n", expectDevice ? " matching \"" : "",
                expectDevice ? expectDevice : "", expectDevice ? "\"" : "",
                requireRt ? " with all required ray-tracing extensions" : "");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
