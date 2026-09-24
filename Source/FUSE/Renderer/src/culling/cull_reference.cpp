// WP-1.3 CPU reference helpers: see include/fuse/renderer/culling/cull_reference.hpp.
#include <fuse/renderer/culling/cull_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/culling/hiz_build_kernel.hpp>

namespace fuse::renderer::culling {

cull_kernel::HizLevels HizPyramid::view() const {
    cull_kernel::HizLevels v{};
    v.dim0 = dim0;
    v.mipCount = mipCount;
    for (u32 i = 0; i < mipCount; ++i) {
        v.level[i] = levels[i].data();
    }
    return v;
}

u64 HizPyramid::texelCount() const {
    u64 n = 0;
    for (u32 i = 0; i < mipCount; ++i) {
        n += levels[i].size();
    }
    return n;
}

bool build_hiz_reference(const f32* depth, u32 width, u32 height, HizPyramid& out, kernel::Backend backend) {
    const hiz_kernel::HizDims dims = hiz_kernel::hiz_dims(width, height);
    if (dims.dim0 == 0u || depth == nullptr) {
        out.dim0 = 0;
        out.mipCount = 0;
        return false;
    }
    out.dim0 = dims.dim0;
    out.mipCount = dims.mipCount;
    for (u32 level = 0; level < dims.mipCount; ++level) {
        const u32 dim = hiz_kernel::mip_dim(dims.dim0, level);
        out.levels[level].resize(static_cast<usize>(dim) * dim);
        hiz_kernel::Params p{};
        if (level == 0u) {
            p.src = kernel::Span<const f32>{depth, width * height};
            p.srcWidth = width;
            p.srcHeight = height;
            p.fromDepth = true;
        } else {
            const u32 srcDim = hiz_kernel::mip_dim(dims.dim0, level - 1u);
            p.src = kernel::Span<const f32>{out.levels[level - 1u].data(), srcDim * srcDim};
            p.srcWidth = srcDim;
            p.srcHeight = srcDim;
            p.fromDepth = false;
        }
        p.dst = kernel::Span<f32>{out.levels[level].data(), dim * dim};
        p.dstDim = dim;
        kernel::launch(backend, hiz_kernel::make_launch(dim), hiz_kernel::Kernel{}, p);
    }
    for (u32 level = dims.mipCount; level < kMaxHizMips; ++level) {
        out.levels[level].clear();
    }
    return true;
}

SceneSpans scene_spans(const gpu_scene::GpuScene& scene) {
    using gpu_scene::GpuSceneTable;
    const gpu_scene::TableBytes inst = scene.tableBytes(GpuSceneTable::Instances);
    const gpu_scene::TableBytes xf = scene.tableBytes(GpuSceneTable::Transforms);
    const gpu_scene::TableBytes prev = scene.tableBytes(GpuSceneTable::PrevTransforms);
    const gpu_scene::TableBytes meshes = scene.tableBytes(GpuSceneTable::Meshes);
    SceneSpans s{};
    s.instances = {reinterpret_cast<const gpu_scene::GpuInstance*>(inst.data), inst.count};
    s.transforms = {reinterpret_cast<const gpu_scene::GpuTransform*>(xf.data), xf.count};
    s.prevTransforms = {reinterpret_cast<const gpu_scene::GpuTransform*>(prev.data), prev.count};
    s.meshes = {reinterpret_cast<const gpu_scene::GpuMesh*>(meshes.data), meshes.count};
    return s;
}

void cull_reference(const SceneSpans& scene, const CullConstants& c, const cull_kernel::HizLevels& prevHiz,
                    const cull_kernel::HizLevels& hiz, f32 radiusScale, std::vector<u32>& results,
                    kernel::Backend backend) {
    const u32 n = c.instanceCount < scene.instances.size ? c.instanceCount : scene.instances.size;
    results.resize(n);
    if (n == 0u) {
        return;
    }
    cull_kernel::Params p{};
    p.instances = scene.instances;
    p.transforms = scene.transforms;
    p.prevTransforms = scene.prevTransforms;
    p.meshes = scene.meshes;
    p.constants = &c;
    p.prevHiz = prevHiz;
    p.hiz = hiz;
    p.radiusScale = radiusScale;
    p.results = kernel::Span<u32>{results.data(), n};
    p.phase = 1u;
    kernel::launch(backend, cull_kernel::make_launch(n), cull_kernel::Kernel{}, p);
    if ((c.flags & kCullOcclusion) != 0u) {
        p.phase = 2u;
        kernel::launch(backend, cull_kernel::make_launch(n), cull_kernel::Kernel{}, p);
    }
}

void cull_reference_parity(const SceneSpans& scene, const CullConstants& c, const cull_kernel::HizLevels& prevHiz,
                           const cull_kernel::HizLevels& hiz, CullParityReference& out, kernel::Backend backend) {
    std::vector<u32> lo;
    std::vector<u32> hi;
    cull_reference(scene, c, prevHiz, hiz, 1.f, out.results, backend);
    cull_reference(scene, c, prevHiz, hiz, 1.f - cull_kernel::kParityEpsilon, lo, backend);
    cull_reference(scene, c, prevHiz, hiz, 1.f + cull_kernel::kParityEpsilon, hi, backend);
    out.ambiguous.assign(out.results.size(), 0u);
    out.ambiguousCount = 0;
    for (usize i = 0; i < out.results.size(); ++i) {
        if (lo[i] != out.results[i] || hi[i] != out.results[i]) {
            out.ambiguous[i] = 1u;
            ++out.ambiguousCount;
        }
    }
}

} // namespace fuse::renderer::culling
