// WP-0.6 test double for the RenderDoc in-app API: a shared library exporting RENDERDOC_GetAPI
// with a counting function table (renderdoc_app.h layout). Loaded by fuse_rp_renderdoc_capture
// through RenderDocCaptureDesc::libraryPath; never shipped.
#include <renderdoc_app.h>

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#define FUSE_FAKE_RDOC_EXPORT extern "C" __declspec(dllexport)
#else
#define FUSE_FAKE_RDOC_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct FuseFakeRenderDocCounters {
    std::uint32_t getApiCalls;
    std::uint32_t triggerCapture;
    std::uint32_t triggerMultiFrames;
    std::uint32_t startCapture;
    std::uint32_t endCapture;
    std::uint32_t discardCapture;
    std::uint32_t setTitle;
    std::uint32_t setPathTemplate;
    void* lastDevice;
    char lastTitle[128];
    char lastPathTemplate[256];
};

namespace {

FuseFakeRenderDocCounters g_counters{};
bool g_capturing = false;
std::uint32_t g_captures = 0;

void RENDERDOC_CC getApiVersion(int* major, int* minor, int* patch) {
    if (major != nullptr) {
        *major = 1;
    }
    if (minor != nullptr) {
        *minor = 6;
    }
    if (patch != nullptr) {
        *patch = 0;
    }
}

void RENDERDOC_CC triggerCapture() {
    ++g_counters.triggerCapture;
}

void RENDERDOC_CC triggerMultiFrameCapture(std::uint32_t frames) {
    g_counters.triggerMultiFrames += frames;
}

void RENDERDOC_CC startFrameCapture(RENDERDOC_DevicePointer device, RENDERDOC_WindowHandle) {
    ++g_counters.startCapture;
    g_counters.lastDevice = device;
    g_capturing = true;
}

std::uint32_t RENDERDOC_CC isFrameCapturing() {
    return g_capturing ? 1u : 0u;
}

std::uint32_t RENDERDOC_CC endFrameCapture(RENDERDOC_DevicePointer device, RENDERDOC_WindowHandle) {
    ++g_counters.endCapture;
    g_counters.lastDevice = device;
    const bool was = g_capturing;
    g_capturing = false;
    g_captures += was ? 1u : 0u;
    return was ? 1u : 0u;
}

std::uint32_t RENDERDOC_CC discardFrameCapture(RENDERDOC_DevicePointer, RENDERDOC_WindowHandle) {
    ++g_counters.discardCapture;
    const bool was = g_capturing;
    g_capturing = false;
    return was ? 1u : 0u;
}

void RENDERDOC_CC setCaptureTitle(const char* title) {
    ++g_counters.setTitle;
    std::strncpy(g_counters.lastTitle, title != nullptr ? title : "", sizeof(g_counters.lastTitle) - 1u);
}

void RENDERDOC_CC setCaptureFilePathTemplate(const char* path) {
    ++g_counters.setPathTemplate;
    std::strncpy(g_counters.lastPathTemplate, path != nullptr ? path : "", sizeof(g_counters.lastPathTemplate) - 1u);
}

std::uint32_t RENDERDOC_CC getNumCaptures() {
    return g_captures;
}

RENDERDOC_API_1_6_0 makeTable() {
    RENDERDOC_API_1_6_0 table;
    std::memset(&table, 0, sizeof(table));
    table.GetAPIVersion = &getApiVersion;
    table.TriggerCapture = &triggerCapture;
    table.TriggerMultiFrameCapture = &triggerMultiFrameCapture;
    table.StartFrameCapture = &startFrameCapture;
    table.IsFrameCapturing = &isFrameCapturing;
    table.EndFrameCapture = &endFrameCapture;
    table.DiscardFrameCapture = &discardFrameCapture;
    table.SetCaptureTitle = &setCaptureTitle;
    table.SetCaptureFilePathTemplate = &setCaptureFilePathTemplate;
    table.GetNumCaptures = &getNumCaptures;
    return table;
}

RENDERDOC_API_1_6_0 g_table = makeTable();

} // namespace

FUSE_FAKE_RDOC_EXPORT int RENDERDOC_CC RENDERDOC_GetAPI(RENDERDOC_Version version, void** outApi) {
    ++g_counters.getApiCalls;
    if (outApi == nullptr || version > eRENDERDOC_API_Version_1_6_0) {
        return 0;
    }
    *outApi = &g_table;
    return 1;
}

FUSE_FAKE_RDOC_EXPORT FuseFakeRenderDocCounters* fuse_fake_renderdoc_counters() {
    return &g_counters;
}
