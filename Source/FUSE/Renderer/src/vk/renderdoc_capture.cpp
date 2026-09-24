// WP-0.6 RenderDoc in-application capture: runtime load of the in-app API, capture triggers.
// See include/fuse/renderer/vk/renderdoc_capture.hpp.
#include <fuse/renderer/vk/renderdoc_capture.hpp>

#include <renderdoc_app.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
// RenderDoc does not support macOS: always unavailable.
#else
#include <dlfcn.h>
#endif

namespace fuse::renderer {

namespace {

RENDERDOC_API_1_6_0* api(void* pointer) {
    return static_cast<RENDERDOC_API_1_6_0*>(pointer);
}

bool atLeast(int major, int minor, int wantMajor, int wantMinor) {
    return major > wantMajor || (major == wantMajor && minor >= wantMinor);
}

/// Strict unsigned decimal parse ("12"); rejects signs, blanks, trailing junk and overflow.
bool parseUnsigned(const char* text, u64& out) {
    if (text == nullptr || *text < '0' || *text > '9') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<u64>(value);
    return true;
}

#if !defined(_WIN32) && !defined(__APPLE__)
constexpr const char* kInjectedNames[] = {"librenderdoc.so", "libVkLayer_GLES_RenderDoc.so"};
#endif

} // namespace

const char* renderDocStatusName(RenderDocStatus status) {
    switch (status) {
    case RenderDocStatus::Available:
        return "available";
    case RenderDocStatus::Disabled:
        return "disabled";
    case RenderDocStatus::NotInjected:
        return "unavailable (RenderDoc not loaded in this process)";
    case RenderDocStatus::LoadFailed:
        return "unavailable (library failed to load)";
    case RenderDocStatus::NoEntryPoint:
        return "unavailable (no RENDERDOC_GetAPI)";
    case RenderDocStatus::UnsupportedApi:
        return "unavailable (API version refused)";
    case RenderDocStatus::UnsupportedPlatform:
        return "unavailable (platform not supported by RenderDoc)";
    }
    return "unavailable";
}

std::unique_ptr<RenderDocCapture> RenderDocCapture::create(const RenderDocCaptureDesc& desc) {
    std::unique_ptr<RenderDocCapture> capture(new RenderDocCapture());
    capture->m_vkInstance = desc.vkInstance;
    capture->scheduleCapture(desc.captureFrame, desc.captureFrameCount);
    capture->load(desc);
    if (capture->available() && desc.captureFilePathTemplate != nullptr) {
        capture->setCaptureFilePathTemplate(desc.captureFilePathTemplate);
    }
    return capture;
}

RenderDocCapture::~RenderDocCapture() {
    if (m_hookCapturing) {
        endFrameCapture();
    }
    // An injected RenderDoc must stay loaded; only a library this object loaded is released, and
    // even then RenderDoc's hooks forbid unloading (RTLD_NODELETE semantics): keep it resident.
    m_api = nullptr;
}

void RenderDocCapture::load(const RenderDocCaptureDesc& desc) {
    if (!desc.enabled) {
        m_status = RenderDocStatus::Disabled;
        m_message = "RenderDoc capture disabled";
        return;
    }
    pRENDERDOC_GetAPI getApi = nullptr;
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr && desc.libraryPath != nullptr) {
        module = LoadLibraryA(desc.libraryPath);
        if (module == nullptr) {
            m_status = RenderDocStatus::LoadFailed;
            m_message = std::string("RenderDoc: LoadLibrary failed for ") + desc.libraryPath;
            return;
        }
        m_ownsLibrary = true;
    }
    if (module == nullptr) {
        m_status = RenderDocStatus::NotInjected;
        m_message = "RenderDoc: renderdoc.dll not loaded in this process";
        return;
    }
    m_library = module;
    getApi = reinterpret_cast<pRENDERDOC_GetAPI>(reinterpret_cast<void*>(GetProcAddress(module, "RENDERDOC_GetAPI")));
#elif defined(__APPLE__)
    m_status = RenderDocStatus::UnsupportedPlatform;
    m_message = "RenderDoc: not supported on Apple platforms";
    return;
#else
    void* library = nullptr;
    for (const char* name : kInjectedNames) {
        library = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
        if (library != nullptr) {
            break;
        }
    }
    if (library == nullptr && desc.libraryPath != nullptr) {
        library = dlopen(desc.libraryPath, RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            const char* error = dlerror();
            m_status = RenderDocStatus::LoadFailed;
            m_message = std::string("RenderDoc: dlopen failed: ") + (error != nullptr ? error : desc.libraryPath);
            return;
        }
        m_ownsLibrary = true;
    }
    if (library == nullptr) {
        m_status = RenderDocStatus::NotInjected;
        m_message = "RenderDoc: librenderdoc.so not loaded in this process";
        return;
    }
    m_library = library;
    getApi = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(library, "RENDERDOC_GetAPI"));
#endif
    if (getApi == nullptr) {
        m_status = RenderDocStatus::NoEntryPoint;
        m_message = "RenderDoc: library has no RENDERDOC_GetAPI";
        return;
    }
    // Newest first; the struct is append-only so an older table is a prefix of the 1.6.0 layout.
    const RENDERDOC_Version versions[] = {eRENDERDOC_API_Version_1_6_0, eRENDERDOC_API_Version_1_4_0,
                                          eRENDERDOC_API_Version_1_1_2};
    for (RENDERDOC_Version version : versions) {
        void* table = nullptr;
        if (getApi(version, &table) == 1 && table != nullptr) {
            m_api = table;
            m_major = static_cast<int>(version) / 10000;
            m_minor = (static_cast<int>(version) / 100) % 100;
            m_patch = static_cast<int>(version) % 100;
            break;
        }
    }
    if (m_api == nullptr) {
        m_status = RenderDocStatus::UnsupportedApi;
        m_message = "RenderDoc: RENDERDOC_GetAPI refused API 1.6.0, 1.4.0 and 1.1.2";
        return;
    }
    if (api(m_api)->GetAPIVersion != nullptr) {
        api(m_api)->GetAPIVersion(&m_major, &m_minor, &m_patch);
    }
    m_status = RenderDocStatus::Available;
    m_message = "RenderDoc: in-app API " + std::to_string(m_major) + "." + std::to_string(m_minor) + "." +
                std::to_string(m_patch);
}

RenderDocCaptureDesc RenderDocCapture::descFromEnvironment(const RenderDocCaptureDesc& base) {
    RenderDocCaptureDesc desc = base;
    const char* enabled = std::getenv("FUSE_RENDERDOC");
    if (enabled != nullptr && (std::strcmp(enabled, "0") == 0 || std::strcmp(enabled, "off") == 0 ||
                               std::strcmp(enabled, "false") == 0)) {
        desc.enabled = false;
    }
    const char* library = std::getenv("FUSE_RENDERDOC_LIB");
    if (library != nullptr && library[0] != '\0') {
        desc.libraryPath = library;
    }
    u64 value = 0;
    if (parseUnsigned(std::getenv("FUSE_RENDERDOC_CAPTURE_FRAME"), value) && value <= static_cast<u64>(INT64_MAX)) {
        desc.captureFrame = static_cast<s64>(value);
    }
    if (parseUnsigned(std::getenv("FUSE_RENDERDOC_CAPTURE_COUNT"), value) && value > 0u && value <= UINT32_MAX) {
        desc.captureFrameCount = static_cast<u32>(value);
    }
    const char* path = std::getenv("FUSE_RENDERDOC_CAPTURE_PATH");
    if (path != nullptr && path[0] != '\0') {
        desc.captureFilePathTemplate = path;
    }
    return desc;
}

bool RenderDocCapture::parseCommandLine(int argc, const char* const* argv, RenderDocCaptureDesc& desc) {
    bool found = false;
    for (int i = 1; i < argc && argv != nullptr; ++i) {
        const char* arg = argv[i];
        if (arg == nullptr) {
            continue;
        }
        for (const char* option : {"--capture-frame", "--capture-count"}) {
            const size_t length = std::strlen(option);
            if (std::strncmp(arg, option, length) != 0) {
                continue;
            }
            const char* value = nullptr;
            if (arg[length] == '=') {
                value = arg + length + 1;
            } else if (arg[length] == '\0' && i + 1 < argc) {
                value = argv[++i];
            } else {
                continue;
            }
            u64 parsed = 0;
            if (!parseUnsigned(value, parsed)) {
                return false;
            }
            if (std::strcmp(option, "--capture-frame") == 0) {
                if (parsed > static_cast<u64>(INT64_MAX)) {
                    return false;
                }
                desc.captureFrame = static_cast<s64>(parsed);
                found = true;
            } else {
                if (parsed == 0u || parsed > UINT32_MAX) {
                    return false;
                }
                desc.captureFrameCount = static_cast<u32>(parsed);
            }
        }
    }
    return found;
}

void RenderDocCapture::apiVersion(int& major, int& minor, int& patch) const {
    major = m_major;
    minor = m_minor;
    patch = m_patch;
}

void RenderDocCapture::setVkInstance(void* vkInstance) {
    m_vkInstance = vkInstance;
}

void* RenderDocCapture::devicePointer() const {
    return m_vkInstance != nullptr ? RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(m_vkInstance) : nullptr;
}

void RenderDocCapture::setCaptureFilePathTemplate(const char* pathTemplate) {
    if (available() && pathTemplate != nullptr && api(m_api)->SetCaptureFilePathTemplate != nullptr) {
        api(m_api)->SetCaptureFilePathTemplate(pathTemplate);
    }
}

void RenderDocCapture::scheduleCapture(s64 frameIndex, u32 frameCount) {
    m_captureFrame = frameIndex;
    m_captureFrameCount = frameCount > 0u ? frameCount : 1u;
}

bool RenderDocCapture::triggerCapture(u32 frameCount) {
    if (!available() || frameCount == 0u) {
        return false;
    }
    if (frameCount == 1u) {
        api(m_api)->TriggerCapture();
    } else {
        api(m_api)->TriggerMultiFrameCapture(frameCount);
    }
    return true;
}

bool RenderDocCapture::startFrameCapture() {
    if (!available()) {
        return false;
    }
    api(m_api)->StartFrameCapture(devicePointer(), nullptr);
    return true;
}

bool RenderDocCapture::endFrameCapture() {
    return available() && api(m_api)->EndFrameCapture(devicePointer(), nullptr) == 1u;
}

bool RenderDocCapture::discardFrameCapture() {
    return available() && atLeast(m_major, m_minor, 1, 4) &&
           api(m_api)->DiscardFrameCapture(devicePointer(), nullptr) == 1u;
}

bool RenderDocCapture::isFrameCapturing() const {
    return available() && api(m_api)->IsFrameCapturing() == 1u;
}

bool RenderDocCapture::setCaptureTitle(const char* title) {
    if (!available() || title == nullptr || !atLeast(m_major, m_minor, 1, 6)) {
        return false;
    }
    api(m_api)->SetCaptureTitle(title);
    return true;
}

u32 RenderDocCapture::numCaptures() const {
    return available() ? api(m_api)->GetNumCaptures() : 0u;
}

void RenderDocCapture::onFrameBegin(u64 frameIndex) {
    if (!available() || m_hookCapturing || m_captureFrame < 0 || frameIndex != static_cast<u64>(m_captureFrame)) {
        return;
    }
    m_hookLastFrame = frameIndex + m_captureFrameCount - 1u;
    m_hookCapturing = startFrameCapture();
    if (m_hookCapturing) {
        const std::string title = "FUSE frame " + std::to_string(frameIndex) +
                                  (m_captureFrameCount > 1u ? "+" + std::to_string(m_captureFrameCount - 1u) : "");
        setCaptureTitle(title.c_str());
    }
}

void RenderDocCapture::onFrameEnd(u64 frameIndex) {
    if (!m_hookCapturing || frameIndex < m_hookLastFrame) {
        return;
    }
    m_hookCapturing = false;
    if (endFrameCapture()) {
        ++m_hookCaptures;
    }
}

} // namespace fuse::renderer
