// FUSE UI GPU 3D mesh pass ("ui.mesh3d", charts / datavis3d): draws a fuse::ui::Mesh3D (mesh3d.hpp) with Vulkan vertex
// and index buffers, a D32 depth buffer and per-vertex two-sided Lambert lighting into an owned RGBA8 colour image.
// After record() the image is in SHADER_READ_ONLY_OPTIMAL and visible to compute shaders, so bind view() to an
// external slot of UiVkRenderer (setExternalImage) and draw an Image command whose Image::externalSlot is that slot:
// the draw-list kernel composites the 3D scene under the chart's axes and labels. fuse::ui::rasterMesh is the CPU
// specification (gate: vk_parity "mesh3d").
//
// Picking: a second R32_UINT attachment receives MeshVertex::id of the visible triangle (0: background).
// requestPick(x, y[, w, h]) before record() copies that rect into a host buffer; after the submission completes,
// pickData() / pickedId() read it (rasterMesh(..., ids) is the CPU twin). Timing: record() brackets the pass with
// timestamps; gpuMs() reads the last completed pass without waiting.
// Exact depth (UiVkMeshDesc::exactDepth, needs the device's geometryShader feature enabled): the fragment stage
// recomputes depth with rasterMesh's float sequence, so depth ties resolve identically (strict CPU parity).
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
    bool exactDepth = false; ///< the device was created with geometryShader enabled (PrimitiveId in fragment)
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
    bool exactDepth() const { return m_exact; }
    void* idView() const { return m_idView; } ///< VkImageView, R32_UINT ids (TRANSFER_SRC_OPTIMAL after record)
    /// Copy the id rect (clamped to the frame) into the host read-back buffer in the next record().
    void requestPick(u32 x, u32 y, u32 w = 1, u32 h = 1);
    /// The ids of the last recorded pick rect, row-major pickWidth() x pickHeight(); valid once that submission
    /// has completed. nullptr before any pick.
    const u32* pickData() const { return m_pickW ? m_readMapped : nullptr; }
    u32 pickWidth() const { return m_pickW; }
    u32 pickHeight() const { return m_pickH; }
    u32 pickedId() const { return m_pickW ? m_readMapped[0] : 0u; }
    /// GPU time of the last completed record() in milliseconds (timestamp queries); < 0 when not available yet.
    double gpuMs() const;

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
    bool m_exact = false;
    void* m_dsl = nullptr;   ///< VkDescriptorSetLayout (exact depth: vertex / index storage views)
    void* m_dpool = nullptr; ///< VkDescriptorPool
    void* m_dset = nullptr;  ///< VkDescriptorSet
    void* m_idImage = nullptr;
    void* m_idMem = nullptr;
    void* m_idView = nullptr;
    void* m_readBuf = nullptr; ///< host-visible id read-back, w*h u32
    void* m_readMem = nullptr;
    u32* m_readMapped = nullptr;
    void* m_query = nullptr; ///< VkQueryPool, 2 timestamps
    f32 m_tsPeriod = 0;      ///< ns per tick; 0: no timestamps
    bool m_recorded = false;
    u32 m_reqX = 0, m_reqY = 0, m_reqW = 0, m_reqH = 0; ///< pending pick
    u32 m_pickW = 0, m_pickH = 0;
};

} // namespace fuse::ui::render
