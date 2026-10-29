// Qt-independent scene renderer host for editor shells (UI9 zero-copy viewport, FUSE_EDITOR_QT_FIRST_PLAN §11).
//
// Owns one Vulkan instance + device (fuse_rhi VulkanInstance / VulkanDevice, surface extensions enabled) and a
// hybrid::HybridSceneRenderer on it, and renders the EditorHost's edited scene (ECS registry + material catalog) seen
// by a ViewportPanel's camera into the renderer's headless RGBA8 target. The FUSE shell's swapchain presenter adopts
// the same device (fuse::app::NativeOptions::adoptDevice) and samples that target through a fuse_ui_vk external slot:
// the 3D frame reaches the window without a CPU readback. readback() is the CPU copy for screenshots / headless shells.
//
// Threading: render() and the presenter submit on the same VkQueue, so call both from the UI thread; render() reads
// the scene, so the caller holds the scene lock (EditorPanels::withScene).
#pragma once

#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::ui {
struct Image;
}

namespace fuse::editor {

class EditorHost;
class ViewportPanel;

struct SceneViewportHostDesc {
    bool validation = false; ///< Khronos validation layer when present (gates count its errors)
    bool readback = true;    ///< keep a host-visible copy of every frame (readback())
};

/// The last rendered frame as a GPU image of the host's device.
struct SceneViewportFrame {
    void* image = nullptr; ///< VkImage, RGBA8, sampled usage
    void* view = nullptr;  ///< VkImageView
    u32 layout = 0;        ///< VkImageLayout it is in (restore it after sampling)
    u32 width = 0, height = 0;
    u64 serial = 0;        ///< frames rendered so far
};

class SceneViewportHost {
public:
    /// Never null; check valid() (reason() says why not: no Vulkan device, stub backend, renderer init failure).
    static std::unique_ptr<SceneViewportHost> create(const SceneViewportHostDesc& desc = {});
    ~SceneViewportHost();
    SceneViewportHost(const SceneViewportHost&) = delete;
    SceneViewportHost& operator=(const SceneViewportHost&) = delete;

    bool valid() const;
    const std::string& reason() const;
    std::string deviceName() const;

    // Raw handles for a presenter adopting this device (fuse::app::VulkanDeviceHandles).
    void* instanceHandle() const;
    void* physicalDeviceHandle() const;
    void* deviceHandle() const;
    void* queueHandle() const;
    u32 queueFamily() const;

    /// Renders one frame of `host`'s scene from `view`'s camera at width x height (resizes the target when the size
    /// changes) and submits it. False when invalid or the frame could not be recorded.
    bool render(EditorHost& host, const ViewportPanel& view, u32 width, u32 height, f32 dt = 1.f / 60.f);
    SceneViewportFrame frame() const;
    u64 framesRendered() const;
    /// Waits for the GPU and copies the last frame into `out` (premultiplied RGBA8 words, opaque). False without
    /// readback or before the first frame.
    bool readback(std::vector<u32>& out, u32& width, u32& height);
    /// Blocks until the device is idle.
    void waitIdle();
    /// Decoded triangle edges of an engine mesh id (x0 y0 z0 x1 y1 z1 per edge; wireframe view modes); null when
    /// the renderer has no such mesh.
    const std::vector<f32>* meshWireframe(u32 engineMeshId) const;
    /// Bumps whenever the renderer's meshes are replaced (wireframe caches key on it).
    u64 meshVersion() const;

private:
    SceneViewportHost();
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace fuse::editor
