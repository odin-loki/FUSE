#pragma once
// E02 UI / HUD composite of the frame composer (docs/unification/RENDERER-EXECUTION.md RE-FI-1, U4-HYBRID-FRAME.md
// U4-1): "frame.ui_composite" alpha-overs a premultiplied-RGBA UI image onto the frame's display-referred output
// (after post) into a separate target, so the upscaler / post images that feed the next frame's history stay
// HUD-less. The kernel (src/frame/shaders/fc_ui_composite.{slang,comp}, Slang primary + GLSL twin, `precise`) uses a
// classic three-binding descriptor set (storage target, sampled scene, sampled UI) rewritten in the pass callback,
// one set per ring slot and pass occurrence: no bindless registration, no steady-state heap allocations.
//
//   out.rgb = ui.rgb + (1 - ui.a) * scene.rgb      (premultiplied "over", display space)
//   out.a   = scene.a
//
// CPU reference: frame_ui_composite_texel (the gate compares the kernel against it).
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::frame {

enum class FrameKernelLanguage : u8;

/// CPU reference of one "frame.ui_composite" texel (premultiplied UI over the scene; the scene alpha is kept).
inline void frame_ui_composite_texel(const f32 scene[4], const f32 ui[4], f32 out[4]) {
    const f32 k = 1.f - ui[3];
    for (u32 c = 0; c < 3u; ++c) {
        const f32 t = k * scene[c];
        out[c] = ui[c] + t;
    }
    out[3] = scene[3];
}

struct FrameUiCompositeDesc {
    VulkanDevice* device = nullptr;
    u32 framesInFlight = 3;
    FrameKernelLanguage language{}; ///< Auto (0): Slang when built, else GLSL
};

class FrameUiComposite {
public:
    static constexpr u32 kMaxPassesPerFrame = 2u;

    FrameUiComposite() = default;
    ~FrameUiComposite();
    FrameUiComposite(const FrameUiComposite&) = delete;
    FrameUiComposite& operator=(const FrameUiComposite&) = delete;

    /// False without a Vulkan device / built kernel (the stub backend always fails).
    bool init(const FrameUiCompositeDesc& desc);
    /// The caller must have retired every frame that used the descriptor sets.
    void destroy();
    bool valid() const { return m_pipeline != nullptr; }
    const char* kernelLanguage() const { return m_language; }

    /// Selects the descriptor ring slot of the frame (serial % framesInFlight) and resets the pass cursor.
    void beginFrame(u64 serial);
    /// Records "frame.ui_composite" (scene: SampledRead, ui: SampledRead, target: StorageWrite; RGBA16F target,
    /// width x height texels from the origin). False when the per-frame pass budget is exhausted or refs are invalid.
    bool addPass(rg::Graph& graph, rg::TextureRef scene, rg::TextureRef ui, rg::TextureRef target, u32 width, u32 height);

private:
    struct Record {
        FrameUiComposite* self = nullptr;
        void* set = nullptr; ///< VkDescriptorSet
        rg::TextureRef scene;
        rg::TextureRef ui;
        rg::TextureRef target;
        u32 extent[2] = {0u, 0u};
    };
    static constexpr u32 kMaxSlots = 8u;
    static void record(const rg::PassContext& context, void* user);

    FrameUiCompositeDesc m_desc{};
    const char* m_language = "none";
    void* m_setLayout = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_pool = nullptr;
    void* m_sets[kMaxSlots][kMaxPassesPerFrame] = {};
    Record m_records[kMaxPassesPerFrame]{};
    u32 m_slot = 0;
    u32 m_used = 0;
};

} // namespace fuse::renderer::frame
