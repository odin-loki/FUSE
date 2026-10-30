// FUSE UI GPU 3D mesh pass ("ui.mesh3d", charts / datavis3d): draws a fuse::ui::Mesh3D (mesh3d.hpp) with Vulkan vertex
// and index buffers, a D32 depth buffer and per-vertex two-sided Lambert lighting into an owned RGBA8 colour image.
// After record() the image is in SHADER_READ_ONLY_OPTIMAL and visible to compute shaders, so bind view() to an
// external slot of UiVkRenderer (setExternalImage) and draw an Image command whose Image::externalSlot is that slot:
// the draw-list kernel composites the 3D scene under the chart's axes and labels. fuse::ui::rasterMesh is the CPU
// specification (gate: vk_parity "mesh3d").
//
// Buffer capacities are fixed at init; upload() fails rather than reallocating them (the target images follow the
// frame size). One upload / record per frame; record into
// a command buffer whose previous use of this pass has completed (single-buffered).
#pragma once

#include "fuse/ui/mesh3d.hpp"

namespace fuse::ui::render {

struct UiVkMeshDesc {
    void* physicalDevice = nullptr; ///< VkPhysicalDevice
    void* device = nullptr;         ///< VkDevice
    u32 maxWidth = 2048, maxHeight = 2048;
    u32 maxVertices = 1u << 21;
    u32 maxIndices = 1u << 23;
};

class UiVkMesh3D {
public:
    UiVkMesh3D() = default;
    ~UiVkMesh3D() { destroy(); }
    UiVkMesh3D(const UiVkMesh3D&) = delete;
    UiVkMesh3D& operator=(const UiVkMesh3D&) = delete;

    /// True when the vertex / fragment SPIR-V was compiled into this build.
    static bool available();
    bool init(const UiVkMeshDesc& desc);
    void destroy();
    bool valid() const { return m_pipeline != nullptr; }
    /// Copies vertices and indices into the persistently mapped buffers. False when over capacity or the frame is
    /// larger than maxWidth x maxHeight.
    bool upload(const Mesh3D& mesh);
    /// Records the render pass for the last upload (clear, depth-tested indexed draw).
    void record(void* commandBuffer);
    /// VkImageView (RGBA8, sampled) of the target, sized exactly frame.width x height (the draw-list kernel scales
    /// an external texture over the command rect). A frame of a new size recreates it (device idle wait), so
    /// re-bind view() to the external slot after an upload that changed the size.
    void* view() const { return m_view; }
    void* image() const { return m_image; } ///< VkImage
    u32 width() const { return m_frame.width; }
    u32 height() const { return m_frame.height; }
    u64 uploadedBytes() const { return m_uploaded; }

private:
    bool ensureTarget(u32 w, u32 h);
    void releaseTarget();
    u32 m_targetW = 0, m_targetH = 0;
    UiVkMeshDesc m_desc{};
    MeshFrame m_frame{};
    u32 m_indexCount = 0;
    u64 m_uploaded = 0;
    void* m_device = nullptr;
    void* m_image = nullptr;
    void* m_imageMem = nullptr;
    void* m_view = nullptr;
    void* m_depth = nullptr;
    void* m_depthMem = nullptr;
    void* m_depthView = nullptr;
    void* m_renderPass = nullptr;
    void* m_framebuffer = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_buffer = nullptr; ///< vertices then indices, host visible + coherent
    void* m_bufferMem = nullptr;
    u8* m_mapped = nullptr;
    u64 m_indexOffset = 0;
};

} // namespace fuse::ui::render
