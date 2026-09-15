#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer {

/// How a presentation surface is supplied to the RHI.
enum class SurfaceKind : u8 {
    /// No WSI surface — headless bootstrap / Lavapipe without a window (CI default).
    Headless = 0,
    /// Platform-owned VkSurfaceKHR passed as an opaque handle (Qt viewport, Android ANativeWindow, etc.).
    External = 1,
};

struct SurfaceDesc {
    SurfaceKind kind = SurfaceKind::Headless;
    /// Opaque VkSurfaceKHR when kind == External; null for Headless.
    void* nativeSurface = nullptr;
};

struct SurfaceInfo {
    SurfaceKind kind = SurfaceKind::Headless;
    bool valid = false;
    std::string message;
};

/// Thin surface abstraction — B2.2 keeps WSI creation in platform modules.
class VulkanSurface {
public:
    static VulkanSurface fromDesc(const SurfaceDesc& desc);

    SurfaceKind kind() const { return m_info.kind; }
    const SurfaceInfo& info() const { return m_info; }
    bool isPresentable() const { return m_info.valid && m_info.kind == SurfaceKind::External; }

    /// Opaque VkSurfaceKHR or null.
    void* nativeHandle() const { return m_nativeSurface; }

private:
    SurfaceInfo m_info;
    void* m_nativeSurface = nullptr;
};

} // namespace fuse::renderer
