// Batched SVO ray cast: CPU launches of the single-source "svo_ray_cast" kernel
// (fuse/scene/svo_ray_kernel.hpp) plus the Auto / Cuda routing. The per-ray walk itself lives only in
// the kernel header; SVO::rayCast calls the same function for one ray.

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/scene/svo.hpp>
#include <fuse/scene/svo_ray_device.hpp>
#include <fuse/scene/svo_ray_kernel.hpp>

#include <bit>
#include <vector>

namespace fuse::scene {

#if defined(FUSE_HAS_CUDA)
/// kernels/svo_ray_cast.cu: stages the view + rays on the device and runs the same kernel body.
bool launchSvoRayCastCuda(const svo_kernel::Params& params, void* stream);
#endif

svo_kernel::SvoView SvoRayLayout::view() const {
    svo_kernel::SvoView v = constants;
    v.nodes = {};
    v.packed = kernel::make_span(nodes.data(), static_cast<u32>(nodes.size()));
    v.bricks = kernel::make_span(bricks.data(), static_cast<u32>(bricks.size()));
    v.pool = kernel::make_span(pool.data(), static_cast<u32>(pool.size()));
    return v;
}

SvoRayLayout buildSvoRayLayout(const svo_kernel::SvoView& source) {
    namespace sk = svo_kernel;
    SvoRayLayout out{};
    out.constants = source;
    out.constants.nodes = {};
    out.constants.bricks = {};
    out.constants.pool = {};
    out.constants.packed = {};
    if (source.initialized == 0u || source.packed.size != 0u || source.bricks.size == 0u) {
        return out; // uninitialised, or already packed: nothing to linearise
    }
    const auto packBrick = [&](u32 brick) {
        SVOBrick b = source.bricks[brick];
        u32 words = 0;
        if (b.mode == sk::kModeSparse) {
            words = 2u * b.count;
        } else if (b.mode == sk::kModeMasked) {
            words = 1u + source.mask_words;
        } else if (b.mode == sk::kModeDense) {
            words = source.brick_voxels; // materials; the walk never reads the written mask or sdf plane
        }
        if (b.mode != sk::kModeUniform) {
            const u32 offset = static_cast<u32>(out.pool.size());
            out.pool.insert(out.pool.end(), source.pool.data + b.payload, source.pool.data + b.payload + words);
            b.payload = offset;
        }
        out.bricks.push_back(b);
    };
    if (source.brick_depth == 0u) {
        packBrick(0u);
        return out;
    }

    // Depth first with contiguous child blocks: expanding a node appends its whole child block, and
    // the children are expanded in octant order (LIFO stack, pushed in reverse).
    struct Item {
        u32 src;
        u32 dst;
        u32 depth;
    };
    out.nodes.reserve(source.nodes.size);
    out.bricks.reserve(source.bricks.size);
    out.nodes.push_back(sk::PackedNode{});
    std::vector<Item> stack;
    stack.push_back(Item{0u, 0u, 0u});
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const SVONode& node = source.nodes[item.src];
        const bool bricksNext = item.depth + 1u == source.brick_depth;
        sk::PackedNode packed{};
        packed.first = static_cast<u32>(bricksNext ? out.bricks.size() : out.nodes.size());
        for (u32 octant = 0; octant < 8u; ++octant) {
            const u32 child = node.children[octant];
            if (child == sk::kNoNode) {
                continue;
            }
            packed.mask |= 1u << octant;
            if (bricksNext) {
                packBrick(child);
            } else {
                out.nodes.push_back(sk::PackedNode{});
            }
        }
        out.nodes[item.dst] = packed;
        if (!bricksNext) {
            u32 slot = packed.first + static_cast<u32>(std::popcount(packed.mask));
            for (u32 octant = 8u; octant-- > 0u;) {
                const u32 child = node.children[octant];
                if (child != sk::kNoNode) {
                    stack.push_back(Item{child, --slot, item.depth + 1u});
                }
            }
        }
    }
    return out;
}

SvoRayLayout SVO::rayLayout() const {
    return buildSvoRayLayout(view());
}

bool SVO::rayCastBatch(kernel::Backend backend, const SvoRay* rays, SvoRayHit* hits, u32 count, void* stream) const {
    svo_kernel::Params params{};
    params.svo = view();
    params.rays = kernel::make_span(rays, count);
    params.hits = kernel::make_span(hits, count);
    if (!svo_kernel::params_valid(params)) {
        return false;
    }
#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
        kernel::backend_available(kernel::Backend::Cuda)) {
        return launchSvoRayCastCuda(params, stream);
    }
#else
    (void)stream;
#endif
    // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
    // (CpuParallel) and records the requested vs executed backend.
    return kernel::launch(backend, svo_kernel::make_launch(count), svo_kernel::Kernel{}, params).ok;
}

#if !defined(FUSE_HAS_CUDA)
// kernels/svo_ray_cast.cu defines the device benchmark in CUDA builds; without one there is no device.
bool benchmarkSvoRayCastCuda(const SVO& /*svo*/, const SvoRay* /*rays*/, SvoRayHit* /*hits*/, u32 /*count*/,
                             u32 /*iterations*/, SvoRayDeviceTiming& /*timing*/) {
    return false;
}
#endif

} // namespace fuse::scene
