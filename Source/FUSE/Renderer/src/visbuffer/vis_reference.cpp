// WP-1.4 CPU references: see include/fuse/renderer/visbuffer/vis_reference.hpp.
#include <fuse/renderer/visbuffer/vis_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace fuse::renderer::visbuffer {

VisSceneView vis_scene_view(const gpu_scene::GpuScene& scene, const std::vector<decode_kernel::MeshPositions>& positions) {
    using gpu_scene::GpuSceneTable;
    VisSceneView v{};
    const gpu_scene::TableBytes instances = scene.tableBytes(GpuSceneTable::Instances);
    const gpu_scene::TableBytes transforms = scene.tableBytes(GpuSceneTable::Transforms);
    const gpu_scene::TableBytes meshes = scene.tableBytes(GpuSceneTable::Meshes);
    v.instances = {reinterpret_cast<const gpu_scene::GpuInstance*>(instances.data), instances.count};
    v.transforms = {reinterpret_cast<const gpu_scene::GpuTransform*>(transforms.data), transforms.count};
    v.meshes = {reinterpret_cast<const gpu_scene::GpuMesh*>(meshes.data), meshes.count};
    v.indices = {scene.indexData(), scene.indexCount()};
    v.positions = {positions.data(), static_cast<u32>(positions.size())};
    return v;
}

void decode_reference(const VisSceneView& scene, const f32 viewProj[16], const u32* vis, u32 width, u32 height,
                      std::vector<VisDecodeTexel>& out, kernel::Backend backend) {
    const u32 pixels = width * height;
    out.assign(pixels, VisDecodeTexel{});
    decode_kernel::Params p{};
    p.vis = {vis, pixels * 2u};
    p.instances = scene.instances;
    p.transforms = scene.transforms;
    p.meshes = scene.meshes;
    p.indices = scene.indices;
    p.positions = scene.positions;
    std::memcpy(p.viewProj, viewProj, sizeof(p.viewProj));
    p.width = width;
    p.height = height;
    p.out = {out.data(), pixels};
    kernel::launch(backend, decode_kernel::make_launch(width, height), decode_kernel::Kernel{}, p);
}

namespace {

constexpr f64 kInf = std::numeric_limits<f64>::infinity();
constexpr f64 kMinW = 1e-6;

struct Acc {
    f64 exactDepth = kInf;
    u64 exactKey = ~u64{0};
    f64 exactMargin = 0.0;
    f64 loose1Depth = kInf;
    u64 loose1Key = ~u64{0};
    f64 loose2Depth = kInf;
    bool unrobust = false;
};

void addLoose(Acc& a, f64 depth, u64 key) {
    if (depth < a.loose1Depth) {
        a.loose2Depth = a.loose1Depth;
        a.loose1Depth = depth;
        a.loose1Key = key;
    } else if (depth < a.loose2Depth) {
        a.loose2Depth = depth;
    }
}

void addExact(Acc& a, f64 depth, u64 key, f64 margin) {
    if (depth < a.exactDepth) {
        a.exactDepth = depth;
        a.exactKey = key;
        a.exactMargin = margin;
    }
}

} // namespace

void raster_reference(const VisSceneView& scene, const f32 viewProj[16], u32 width, u32 height,
                      const RasterRefOptions& options, std::vector<RasterRefPixel>& out, RasterRefStats* stats) {
    RasterRefStats st{};
    std::vector<Acc> acc(static_cast<usize>(width) * height);
    decode_kernel::Params p{};
    p.instances = scene.instances;
    p.transforms = scene.transforms;
    p.meshes = scene.meshes;
    p.indices = scene.indices;
    p.positions = scene.positions;
    std::memcpy(p.viewProj, viewProj, sizeof(p.viewProj));
    const f64 em = options.edgeMargin;
    const f64 dm = options.depthMargin;
    const f64 W = static_cast<f64>(width);
    const f64 H = static_cast<f64>(height);

    for (u32 i = 0; i < scene.instances.size; ++i) {
        const gpu_scene::GpuInstance& inst = scene.instances[i];
        constexpr u32 kNeed = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible;
        if ((inst.flags & (kNeed | gpu_scene::kInstanceTransparent)) != kNeed || inst.mesh >= scene.meshes.size) {
            continue;
        }
        const gpu_scene::GpuMesh& mesh = scene.meshes[inst.mesh];
        const u32 triangles = mesh.indexCount / 3u;
        for (u32 t = 0; t < triangles; ++t) {
            decode_kernel::Clip c[3];
            if (!decode_kernel::triangle_clip(p, i, t, c)) {
                continue;
            }
            ++st.triangles;
            const u64 key = (static_cast<u64>(i) << 32u) | t;
            const bool nearPlane = c[0].w <= kMinW || c[1].w <= kMinW || c[2].w <= kMinW;
            if (nearPlane) {
                // Homogeneous (2DH) test at every pixel: no screen-space margin exists, so every
                // pixel the triangle touches is excluded from the comparison.
                ++st.nearPlaneTriangles;
                for (u32 y = 0; y < height; ++y) {
                    for (u32 x = 0; x < width; ++x) {
                        const f64 nx = (x + 0.5) * 2.0 / W - 1.0;
                        const f64 ny = (y + 0.5) * 2.0 / H - 1.0;
                        f64 ux[3], uy[3];
                        for (u32 k = 0; k < 3u; ++k) {
                            ux[k] = static_cast<f64>(c[k].x) - nx * c[k].w;
                            uy[k] = static_cast<f64>(c[k].y) - ny * c[k].w;
                        }
                        const f64 e[3] = {ux[1] * uy[2] - uy[1] * ux[2], ux[2] * uy[0] - uy[2] * ux[0],
                                          ux[0] * uy[1] - uy[0] * ux[1]};
                        const f64 s = e[0] + e[1] + e[2];
                        ++st.pixelTests;
                        if (s == 0.0) {
                            continue;
                        }
                        const f64 b[3] = {e[0] / s, e[1] / s, e[2] / s};
                        const f64 w = b[0] * c[0].w + b[1] * c[1].w + b[2] * c[2].w;
                        if (w <= 0.0 || std::min({b[0], b[1], b[2]}) < -1e-3) {
                            continue;
                        }
                        const f64 depth = (b[0] * c[0].z + b[1] * c[1].z + b[2] * c[2].z) / w;
                        Acc& a = acc[static_cast<usize>(y) * width + x];
                        if (depth < -dm || depth > 1.0 + dm) {
                            continue;
                        }
                        a.unrobust = true;
                        addLoose(a, depth, key);
                        if (std::min({b[0], b[1], b[2]}) >= 0.0 && depth >= 0.0 && depth <= 1.0) {
                            addExact(a, depth, key, 0.0);
                        }
                    }
                }
                continue;
            }
            f64 sx[3], sy[3], sz[3];
            for (u32 k = 0; k < 3u; ++k) {
                const f64 w = c[k].w;
                sx[k] = (static_cast<f64>(c[k].x) / w + 1.0) * 0.5 * W;
                sy[k] = (static_cast<f64>(c[k].y) / w + 1.0) * 0.5 * H;
                sz[k] = static_cast<f64>(c[k].z) / w;
            }
            const f64 area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
            if (!(std::fabs(area) > 1e-12)) {
                continue; // zero area: never rasterised
            }
            const f64 sign = area > 0.0 ? 1.0 : -1.0;
            // Edge k is opposite vertex k: from vertex k+1 to k+2.
            f64 ex[3], ey[3], len[3];
            for (u32 k = 0; k < 3u; ++k) {
                const u32 a = (k + 1u) % 3u;
                const u32 b = (k + 2u) % 3u;
                ex[k] = sx[b] - sx[a];
                ey[k] = sy[b] - sy[a];
                len[k] = std::sqrt(ex[k] * ex[k] + ey[k] * ey[k]);
            }
            const f64 minX = std::min({sx[0], sx[1], sx[2]}) - em - 0.5;
            const f64 maxX = std::max({sx[0], sx[1], sx[2]}) + em - 0.5;
            const f64 minY = std::min({sy[0], sy[1], sy[2]}) - em - 0.5;
            const f64 maxY = std::max({sy[0], sy[1], sy[2]}) + em - 0.5;
            if (maxX < 0.0 || maxY < 0.0 || minX > W - 1.0 || minY > H - 1.0) {
                continue;
            }
            const u32 x0 = static_cast<u32>(std::max(0.0, std::ceil(minX)));
            const u32 y0 = static_cast<u32>(std::max(0.0, std::ceil(minY)));
            const u32 x1 = static_cast<u32>(std::min(W - 1.0, std::floor(maxX)));
            const u32 y1 = static_cast<u32>(std::min(H - 1.0, std::floor(maxY)));
            for (u32 y = y0; y <= y1; ++y) {
                for (u32 x = x0; x <= x1; ++x) {
                    ++st.pixelTests;
                    const f64 px = x + 0.5;
                    const f64 py = y + 0.5;
                    f64 lambda[3];
                    f64 margin = kInf;
                    for (u32 k = 0; k < 3u; ++k) {
                        const u32 a = (k + 1u) % 3u;
                        // Signed area of (edge k, p), positive inside for either winding.
                        const f64 e = sign * (ex[k] * (py - sy[a]) - ey[k] * (px - sx[a]));
                        lambda[k] = e / std::fabs(area);
                        margin = std::min(margin, len[k] > 0.0 ? e / len[k] : -kInf);
                    }
                    if (margin < -em) {
                        continue;
                    }
                    const f64 depth = lambda[0] * sz[0] + lambda[1] * sz[1] + lambda[2] * sz[2];
                    if (depth < -dm || depth > 1.0 + dm) {
                        continue;
                    }
                    Acc& a = acc[static_cast<usize>(y) * width + x];
                    if (depth < dm || depth > 1.0 - dm) {
                        a.unrobust = true; // near / far clip plane
                    }
                    addLoose(a, depth, key);
                    if (margin >= 0.0 && depth >= 0.0 && depth <= 1.0) {
                        addExact(a, depth, key, margin);
                    }
                }
            }
        }
    }

    out.assign(acc.size(), RasterRefPixel{});
    for (usize k = 0; k < acc.size(); ++k) {
        const Acc& a = acc[k];
        RasterRefPixel& o = out[k];
        if (a.exactDepth < kInf) {
            o.instance = static_cast<u32>(a.exactKey >> 32u);
            o.triangle = static_cast<u32>(a.exactKey & 0xFFFFFFFFu);
            o.depth = static_cast<f32>(a.exactDepth);
            o.robust = (!a.unrobust && a.exactMargin >= em && a.loose1Key == a.exactKey &&
                        a.loose2Depth > a.exactDepth + dm)
                           ? 1u
                           : 0u;
            ++st.covered;
            st.robustCovered += o.robust;
        } else {
            o.robust = (!a.unrobust && a.loose1Depth == kInf) ? 1u : 0u;
        }
        st.robust += o.robust;
    }
    if (stats != nullptr) {
        *stats = st;
    }
}

} // namespace fuse::renderer::visbuffer
