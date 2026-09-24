#pragma once

// WP-4.1 GPU motion vectors ("temporal.motion"): camera and per-object motion from the WP-1.1 previous
// transforms, reconstructed from the WP-1.4 visibility buffer, with the jitter handled explicitly (conventions:
// temporal_types.hpp; CPU reference: motion_kernel.hpp).
//
//   f32 draw[16];
//   jitter_view_proj(viewProj, jitter.x, jitter.y, w, h, draw);          // cull + draw the visibility buffer with it
//   motion.beginFrame(serial, {viewProj, prevViewProj, jitter, scene.headerHandle(), vb.visStorageHandle()});
//   MotionGraphRefs m = motion.importInto(graph);
//   motion.addMotion(graph, m, visRefs.vis, sceneRefs);                   // after the visibility draws
//   // TaauGpu reads m.depth / m.motion (TaauGpuFrameDesc::depth / motion = depthAddress() / motionAddress())
//
// Output (render resolution, persistent device buffers, BDA): motion f32 x 2 per pixel = UV motion
// current - previous from the unjittered projections (UpscaleInputs contract: static geometry = camera motion,
// moving instances = object + camera motion, sky = camera rotation only), depth f32 per pixel = linear view
// depth (0 = sky). No jitter term reaches either buffer. One compute pass, 8 x 8 tiles, declared on the
// render graph (no manual barriers); steady-state frames make no heap allocations.
//
// Relation to the WP-1.5 G-buffer velocity (RG16F, pixels, current - previous): that attachment keeps the
// jitter of the matrices it is given (pixel centre under the jittered matrix minus the previous projection),
// i.e. velocity_px = true_px - jitter_cur (+ jitter_prev when prevViewProj was jittered too), and it is
// half-precision; temporal consumers read this pass's buffers instead.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/renderer/temporal/temporal_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::temporal {

struct TemporalMotionDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    u32 width = 0; ///< render resolution (the visibility buffer's)
    u32 height = 0;
    TemporalKernelLanguage language = TemporalKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
    const char* name = "temporal_motion";
};

struct MotionFrameDesc {
    f32 viewProj[16] = {};     ///< column-major, UNJITTERED, this frame (Vulkan clip space, forward depth)
    f32 prevViewProj[16] = {}; ///< column-major, UNJITTERED, last frame
    math::Vec2 jitter_px{};    ///< this frame's render-pixel jitter; the visibility buffer used jitter_view_proj()
    u32 scene = 0;             ///< GpuScene::headerHandle()
    u32 vis = 0;               ///< VisBuffer::visStorageHandle()
};

struct MotionGraphRefs {
    rg::BufferRef motion;
    rg::BufferRef depth;
};

struct MotionStats {
    u32 motionPasses = 0; ///< this frame
    bool skyValid = false;
};

class TemporalMotion {
public:
    TemporalMotion() = default;
    ~TemporalMotion();
    TemporalMotion(const TemporalMotion&) = delete;
    TemporalMotion& operator=(const TemporalMotion&) = delete;

    bool init(const TemporalMotionDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }

    /// Writes this frame's constants (ring slot of `frameSerial`), including the jittered draw matrix
    /// (drawViewProj()) and the sky reprojection.
    bool beginFrame(u64 frameSerial, const MotionFrameDesc& frame);
    MotionGraphRefs importInto(rg::Graph& graph);
    /// "temporal.motion": reads `vis` (storage) and the scene (BDA), writes the motion / depth buffers.
    void addMotion(rg::Graph& graph, const MotionGraphRefs& refs, rg::TextureRef vis, const gpu_scene::GpuSceneGraphRefs& scene);

    // --- inspection ------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const Buffer& motionBuffer() const { return m_motion; }
    const Buffer& depthBuffer() const { return m_depth; }
    u64 motionAddress() const { return m_motion.deviceAddress; }
    u64 depthAddress() const { return m_depth.deviceAddress; }
    /// This frame's constants as written by beginFrame (the CPU reference kernel takes them verbatim).
    const MotionFrameConstants& frameConstants() const { return m_constants; }
    const f32* drawViewProj() const { return m_constants.drawViewProj; }
    const MotionStats& stats() const { return m_stats; }

private:
    struct PassRecord {
        TemporalMotion* self = nullptr;
        TemporalPush push{};
    };
    static void recordMotion(const rg::PassContext& context, void* user);

    TemporalMotionDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u32 m_width = 0;
    u32 m_height = 0;
    Buffer m_motion{};
    Buffer m_depth{};
    u8 m_queues[2] = {rg::kNoQueue, rg::kNoQueue};
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    MotionFrameConstants m_constants{};
    PassRecord m_record{};
    void* m_layoutHandle = nullptr;
    void* m_pipeline = nullptr;
    MotionStats m_stats{};
};

} // namespace fuse::renderer::temporal
