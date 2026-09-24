// WP-5.4 software rasteriser, Slang twin of swraster_raster.comp (see there) and of
// swraster_kernel.hpp RasterKernel. FUSE_VIS64_IMAGE selects the R64_UINT storage image, else the u64
// storage buffer. Compile with -fp-mode precise.
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "swraster_common.slang"

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_sw_words[];
#if defined(FUSE_VIS64_IMAGE)
[[vk::binding(0, 0)]] [format("r64ui")] RWTexture2D<uint64_t> fuse_vis64_images[];
#else
[[vk::binding(3, 0)]] RWStructuredBuffer<uint64_t> fuse_vis64_buffers[];
#endif

struct Push {
    uint64_t constants;
    uint region;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

groupshared int s_x[64];
groupshared int s_y[64];
groupshared uint s_z[64];
groupshared int s_bounds[4];
groupshared uint s_invalid;

void fuseSwWrite(uint target, uint width, uint pixel, uint64_t word) {
#if defined(FUSE_VIS64_IMAGE)
    InterlockedMin(fuse_vis64_images[target][uint2(pixel % width, pixel / width)], word);
#else
    InterlockedMin(fuse_vis64_buffers[target][pixel], word);
#endif
}

[numthreads(64, 1, 1)]
void main(uint3 groupId: SV_GroupID, uint3 localId: SV_GroupThreadID) {
    SwConstants* C = (SwConstants*)pc.constants;
    const uint region = pc.region;
    const uint ci = groupId.x;
    const uint lane = localId.x;
    const uint cap = C.capacity;
    if (ci >= cap) {
        return;
    }
    const uint counts = fuseSwHandleIndex(C.countsBuffer);
    const uint sw = fuseSwHandleIndex(C.swBuffer);
    const uint r = (region * cap + ci) * 2u;
    const uint instance = fuse_sw_words[sw][r];
    const uint meshlet = fuse_sw_words[sw][r + 1u];
    GpuSceneHeader* scene = fuseGpuScene(C.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuMeshlet ml = ((GpuMeshlet*)mesh.meshlets)[meshlet];
    const uint vertices = fuseSwMeshletVertices(ml);
    const uint triangles = fuseSwMeshletTriangles(ml);
    const uint width = C.width;
    const uint height = C.height;
    uint previous;
    int previousInt;
    if (lane == 0u) {
        s_bounds[0] = 0x7FFFFFFF;
        s_bounds[1] = -0x7FFFFFFF;
        s_bounds[2] = 0x7FFFFFFF;
        s_bounds[3] = -0x7FFFFFFF;
        s_invalid = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < vertices) {
        float4 vp[4];
        fuseSwViewProj(C, vp);
        const GpuTransform xf = fuseGpuSceneTransforms(scene)[instance];
        uint* mvrt = (uint*)mesh.meshletVertices;
        const uint vertex = uint(int(mvrt[ml.vertexOffset + lane]) + mesh.vertexOffset);
        const SwVertex v = fuseSwProject(fuseVisClip(xf, vp, fuseVisMeshPosition(mesh, vertex)), width, height);
        s_x[lane] = v.x;
        s_y[lane] = v.y;
        s_z[lane] = v.z;
        if (v.valid == 0u) {
            InterlockedOr(s_invalid, 1u, previous);
        } else {
            InterlockedMin(s_bounds[0], v.x, previousInt);
            InterlockedMax(s_bounds[1], v.x, previousInt);
            InterlockedMin(s_bounds[2], v.y, previousInt);
            InterlockedMax(s_bounds[3], v.y, previousInt);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const int spanX = s_bounds[1] - s_bounds[0];
    const int spanY = s_bounds[3] - s_bounds[2];
    if (s_invalid != 0u || spanX > kFuseSwMaxSpanSubpixels || spanY > kFuseSwMaxSpanSubpixels) {
        if (lane == 0u) {
            InterlockedAdd(fuse_sw_words[counts][kFuseSwCountDemoted], 1u, previous);
            uint at;
            InterlockedAdd(fuse_sw_words[counts][kFuseSwCountHwRequested0 + region], 1u, at);
            if (at < cap) {
                const uint o = (region * cap + at) * 2u;
                fuse_sw_words[fuseSwHandleIndex(C.hwBuffer)][o] = instance;
                fuse_sw_words[fuseSwHandleIndex(C.hwBuffer)][o + 1u] = meshlet;
                InterlockedMax(fuse_sw_words[counts][kFuseSwCountHardware0 + region * 4u + 1u], at + 1u, previous);
            } else {
                InterlockedAdd(fuse_sw_words[counts][kFuseSwCountOverflow], 1u, previous);
            }
        }
        return;
    }
    if (lane == 0u) {
        InterlockedAdd(fuse_sw_words[counts][kFuseSwCountSwTriangles], triangles, previous);
    }
    if ((C.flags & kFuseSwFlagSkipSoftware) != 0u) {
        return;
    }
    const uint target = fuseSwHandleIndex(C.target64);
    uint* mtri = (uint*)mesh.meshletTriangles;
    for (uint t = lane; t < triangles; t += kFuseSwRasterThreads) {
        const uint packed = mtri[ml.triangleOffset + t];
        const uint i0 = packed & 0xFFu;
        uint i1 = (packed >> 8u) & 0xFFu;
        uint i2 = (packed >> 16u) & 0xFFu;
        int area = (s_x[i1] - s_x[i0]) * (s_y[i2] - s_y[i0]) - (s_y[i1] - s_y[i0]) * (s_x[i2] - s_x[i0]);
        if (area == 0) {
            continue;
        }
        if (area < 0) {
            const uint tmp = i1;
            i1 = i2;
            i2 = tmp;
            area = -area;
        }
        const int x0 = s_x[i0];
        const int y0 = s_y[i0];
        const int x1 = s_x[i1];
        const int y1 = s_y[i1];
        const int x2 = s_x[i2];
        const int y2 = s_y[i2];
        const uint64_t z0 = uint64_t(s_z[i0]);
        const uint64_t z1 = uint64_t(s_z[i1]);
        const uint64_t z2 = uint64_t(s_z[i2]);
        const int minX = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        const int maxX = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        const int minY = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        const int maxY = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        int px0 = fuseSwFirstCentre(minX);
        int px1 = fuseSwLastCentre(maxX);
        int py0 = fuseSwFirstCentre(minY);
        int py1 = fuseSwLastCentre(maxY);
        px0 = px0 > 0 ? px0 : 0;
        py0 = py0 > 0 ? py0 : 0;
        px1 = px1 < int(width) - 1 ? px1 : int(width) - 1;
        py1 = py1 < int(height) - 1 ? py1 : int(height) - 1;
        if (px0 > px1 || py0 > py1) {
            continue;
        }
        const int dx0 = x2 - x1;
        const int dy0 = y2 - y1;
        const int dx1 = x0 - x2;
        const int dy1 = y0 - y2;
        const int dx2 = x1 - x0;
        const int dy2 = y1 - y0;
        const int b0 = fuseSwEdgeBias(dx0, dy0);
        const int b1 = fuseSwEdgeBias(dx1, dy1);
        const int b2 = fuseSwEdgeBias(dx2, dy2);
        const int ox = px0 * kFuseSwSubpixelOne + kFuseSwSubpixelHalf;
        const int oy = py0 * kFuseSwSubpixelOne + kFuseSwSubpixelHalf;
        int r0 = dx0 * (oy - y1) - dy0 * (ox - x1);
        int r1 = dx1 * (oy - y2) - dy1 * (ox - x2);
        int r2 = dx2 * (oy - y0) - dy2 * (ox - x0);
        const int sx0 = -dy0 * kFuseSwSubpixelOne;
        const int sx1 = -dy1 * kFuseSwSubpixelOne;
        const int sx2 = -dy2 * kFuseSwSubpixelOne;
        const int sy0 = dx0 * kFuseSwSubpixelOne;
        const int sy1 = dx1 * kFuseSwSubpixelOne;
        const int sy2 = dx2 * kFuseSwSubpixelOne;
        const uint triangle = ml.triangleOffset + t;
        for (int py = py0; py <= py1; ++py) {
            int e0 = r0;
            int e1 = r1;
            int e2 = r2;
            for (int px = px0; px <= px1; ++px) {
                if (e0 + b0 >= 0 && e1 + b1 >= 0 && e2 + b2 >= 0) {
                    const uint64_t num = uint64_t(uint(e0)) * z0 + uint64_t(uint(e1)) * z1 + uint64_t(uint(e2)) * z2;
                    const uint64_t z = num / uint64_t(uint(area));
                    fuseSwWrite(target, width, uint(py) * width + uint(px), fuseSwPack(uint(z >> 8u), instance, triangle));
                }
                e0 += sx0;
                e1 += sx1;
                e2 += sx2;
            }
            r0 += sy0;
            r1 += sy1;
            r2 += sy2;
        }
    }
}
