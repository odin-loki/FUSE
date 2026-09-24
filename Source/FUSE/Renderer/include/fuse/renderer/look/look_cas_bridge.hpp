#pragma once

// Bridges the look chain's `sharpen` node to the upscaler module's AMD FidelityFX CAS port
// (fuse/renderer/upscale/upscale_passes.hpp, run_cas). Owns preallocated RGBA staging so sharpening
// stays allocation-free per frame. When the upscale module is not part of the build, `available()` is
// false and the chain keeps its built-in look_sharpen kernel.
//
//   LookCasSharpener cas;
//   cas.init(width, height);
//   chain.setSharpenHook(&LookCasSharpener::hook, &cas);

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::look {

class LookCasSharpener {
public:
    static bool available();
    bool init(u32 width, u32 height, kernel::Backend backend = kernel::Backend::CpuParallel);
    /// LookSharpenHook-compatible entry (`user` = LookCasSharpener*). Returns false (-> built-in fallback)
    /// when CAS is unavailable or the frame size does not match init().
    static bool hook(const math::Vec3* src, math::Vec3* dst, u32 width, u32 height, f32 sharpness, void* user);
    u32 calls() const { return m_calls; }

private:
    std::vector<math::Vec4> m_src;
    std::vector<math::Vec4> m_dst;
    u32 m_width = 0;
    u32 m_height = 0;
    kernel::Backend m_backend = kernel::Backend::CpuParallel;
    u32 m_calls = 0;
};

} // namespace fuse::renderer::look
