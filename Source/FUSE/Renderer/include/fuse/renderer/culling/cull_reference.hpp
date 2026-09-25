#pragma once

// WP-1.3 host helpers around the single-source CPU reference kernels: Hi-Z pyramid from a depth
// image (hiz_build_kernel.hpp) and the brute-force two-phase instance cull over a GpuScene mirror
// (instance_cull_kernel.hpp, every instance tested on its own). fuse_rp_culling compares the GPU
// against these; they also serve as the CPU fallback / oracle for later packages.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/cull_types.hpp>
#include <fuse/renderer/culling/instance_cull_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>

#include <vector>

namespace fuse::renderer::culling {

/// CPU Hi-Z pyramid (square, power of two; hiz_build_kernel.hpp layout).
struct HizPyramid {
    u32 dim0 = 0;
    u32 mipCount = 0;
    std::vector<f32> levels[kMaxHizMips];

    cull_kernel::HizLevels view() const;
    u64 texelCount() const;
};

/// Builds the pyramid of a row-major depth image with one kernel launch per level.
bool build_hiz_reference(const f32* depth, u32 width, u32 height, HizPyramid& out,
                         kernel::Backend backend = kernel::Backend::CpuReference);

/// Spans of a GpuScene's CPU mirror (instances, cur / prev transforms, meshes).
struct SceneSpans {
    kernel::Span<const gpu_scene::GpuInstance> instances;
    kernel::Span<const gpu_scene::GpuTransform> transforms;
    kernel::Span<const gpu_scene::GpuTransform> prevTransforms;
    kernel::Span<const gpu_scene::GpuMesh> meshes;
};
SceneSpans scene_spans(const gpu_scene::GpuScene& scene);

/// Both phases for every slot < c.instanceCount (results resized to it). `prevHiz` is last frame's
/// pyramid (phase 1), `hiz` this frame's after the phase-1 draws (phase 2).
void cull_reference(const SceneSpans& scene, const CullConstants& c, const cull_kernel::HizLevels& prevHiz,
                    const cull_kernel::HizLevels& hiz, f32 radiusScale, std::vector<u32>& results,
                    kernel::Backend backend = kernel::Backend::CpuReference);

/// cull_reference at radius scale 1 and 1 -/+ kParityEpsilon; `ambiguous[i]` = 1 when the three
/// final results differ (the instance sits on a test boundary; see the parity rule in
/// instance_cull_kernel.hpp).
struct CullParityReference {
    std::vector<u32> results;
    std::vector<u8> ambiguous;
    u32 ambiguousCount = 0;
};
void cull_reference_parity(const SceneSpans& scene, const CullConstants& c, const cull_kernel::HizLevels& prevHiz,
                           const cull_kernel::HizLevels& hiz, CullParityReference& out,
                           kernel::Backend backend = kernel::Backend::CpuReference);

} // namespace fuse::renderer::culling
