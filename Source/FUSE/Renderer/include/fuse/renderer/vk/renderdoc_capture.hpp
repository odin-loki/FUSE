#pragma once

// WP-0.6 RenderDoc in-application capture (docs/unification/RENDERER-EXECUTION.md).
//
// FUSE never links RenderDoc. create() attaches to a RenderDoc that already injected itself into
// the process (launched from the RenderDoc UI / `renderdoccmd capture`), or loads an explicit
// library path (desc.libraryPath / FUSE_RENDERDOC_LIB, before the VkInstance is created), and asks
// for the in-app API (vendored Engine/lib/renderdoc/include/renderdoc_app.h, MIT). Without
// RenderDoc every call is a cheap no-op and available() is false: status() says why.
//
// Triggers
//   API          triggerCapture(n)       capture the next n presented frames (RenderDoc's own
//                                        frame delimiter: vkQueuePresentKHR)
//                startFrameCapture() / endFrameCapture()   explicit range (headless too)
//   frame hook   onFrameBegin(i) / onFrameEnd(i) around frame i: starts/ends the scheduled
//                capture when i == captureFrame (command line --capture-frame N, env
//                FUSE_RENDERDOC_CAPTURE_FRAME=N), captureFrameCount frames long
//   env          FUSE_RENDERDOC=0 disables the hook; FUSE_RENDERDOC_CAPTURE_PATH sets the capture
//                file path template; FUSE_RENDERDOC_LIB loads that library
// Object names: RenderDoc records VK_EXT_debug_utils names, so objects named through
// fuse::renderer::setDebugObjectName / nameVkObject (vk/debug_utils.hpp) appear by name in captures.

#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct RenderDocCaptureDesc {
    /// Explicit library to load (librenderdoc.so / renderdoc.dll). Null: only attach when injected.
    const char* libraryPath = nullptr;
    /// VkInstance whose captures to control (RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE); null = any.
    void* vkInstance = nullptr;
    /// First frame index to capture in onFrameBegin (-1 = none) and how many frames.
    s64 captureFrame = -1;
    u32 captureFrameCount = 1;
    /// RenderDoc capture path template (e.g. "captures/fuse"); null keeps RenderDoc's default.
    const char* captureFilePathTemplate = nullptr;
    /// false: do not even look for RenderDoc (FUSE_RENDERDOC=0).
    bool enabled = true;
};

enum class RenderDocStatus : u8 {
    Available,
    Disabled,         ///< desc.enabled == false
    NotInjected,      ///< RenderDoc is not loaded in this process (and no libraryPath given)
    LoadFailed,       ///< libraryPath could not be loaded
    NoEntryPoint,     ///< the library has no RENDERDOC_GetAPI
    UnsupportedApi,   ///< RENDERDOC_GetAPI refused every API version FUSE asks for
    UnsupportedPlatform,
};

const char* renderDocStatusName(RenderDocStatus status);

class RenderDocCapture {
public:
    static std::unique_ptr<RenderDocCapture> create(const RenderDocCaptureDesc& desc = {});
    ~RenderDocCapture();

    RenderDocCapture(const RenderDocCapture&) = delete;
    RenderDocCapture& operator=(const RenderDocCapture&) = delete;

    /// Desc from the environment (FUSE_RENDERDOC, FUSE_RENDERDOC_LIB, FUSE_RENDERDOC_CAPTURE_FRAME,
    /// FUSE_RENDERDOC_CAPTURE_COUNT, FUSE_RENDERDOC_CAPTURE_PATH) layered over `base`. The returned
    /// desc points into the process environment (valid until it changes).
    static RenderDocCaptureDesc descFromEnvironment(const RenderDocCaptureDesc& base = {});
    /// `--capture-frame N` / `--capture-frame=N` (and `--capture-count N`) from argv. Returns true
    /// when a frame was found; malformed values leave `desc` unchanged and return false.
    static bool parseCommandLine(int argc, const char* const* argv, RenderDocCaptureDesc& desc);

    bool available() const { return m_status == RenderDocStatus::Available; }
    RenderDocStatus status() const { return m_status; }
    const std::string& message() const { return m_message; }
    /// RenderDoc API version the library returned (0.0.0 when unavailable).
    void apiVersion(int& major, int& minor, int& patch) const;

    void setVkInstance(void* vkInstance);
    void setCaptureFilePathTemplate(const char* pathTemplate);
    /// Scheduled capture for the frame hook (-1 clears it).
    void scheduleCapture(s64 frameIndex, u32 frameCount = 1);

    bool triggerCapture(u32 frameCount = 1);
    bool startFrameCapture();
    /// Returns true when RenderDoc wrote the capture.
    bool endFrameCapture();
    bool discardFrameCapture();
    bool isFrameCapturing() const;
    /// Title of the capture in progress (RenderDoc API 1.6+; false otherwise).
    bool setCaptureTitle(const char* title);
    u32 numCaptures() const;

    /// Frame-loop hook: call around every frame with a monotonically increasing index.
    void onFrameBegin(u64 frameIndex);
    void onFrameEnd(u64 frameIndex);
    /// Captures this hook started and completed (onFrameBegin/onFrameEnd).
    u32 hookCaptures() const { return m_hookCaptures; }

private:
    RenderDocCapture() = default;
    void load(const RenderDocCaptureDesc& desc);
    void* devicePointer() const;

    void* m_library = nullptr;
    bool m_ownsLibrary = false;
    void* m_api = nullptr; ///< RENDERDOC_API_1_1_2* (1.6.0 layout when m_minor >= 6)
    int m_major = 0;
    int m_minor = 0;
    int m_patch = 0;
    void* m_vkInstance = nullptr;
    s64 m_captureFrame = -1;
    u32 m_captureFrameCount = 1;
    bool m_hookCapturing = false;
    u64 m_hookLastFrame = 0;
    u32 m_hookCaptures = 0;
    RenderDocStatus m_status = RenderDocStatus::NotInjected;
    std::string m_message;
};

} // namespace fuse::renderer
