// WP-2.1 "light.bounds". Slang twin of lc_bounds.comp (see there).
#include "lc_includes.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const LcFrame F = fuseLcFrame();
    const uint i = tid.x;
    if (i < F.clusterCount) {
        const uint slice = i % F.slicesZ;
        const uint tile = i / F.slicesZ;
        const uint tx = tile % F.tilesX;
        const uint ty = tile / F.tilesX;
        const float depths[2] = { F.sliceDepth[slice], F.sliceDepth[slice + 1u] };
        const float nx[2] = { F.ndcX[tx], F.ndcX[tx + 1u] };
        const float ny[2] = { F.ndcY[ty + 1u], F.ndcY[ty] };
        float3 mn = float3(0.0, 0.0, 0.0);
        float3 mx = float3(0.0, 0.0, 0.0);
        bool first = true;
        for (uint d = 0u; d < 2u; ++d) {
            for (uint ix = 0u; ix < 2u; ++ix) {
                for (uint iy = 0u; iy < 2u; ++iy) {
                    const float depth = depths[d];
                    const float3 corner = float3(nx[ix] * F.tanX * depth, ny[iy] * F.tanY * depth, -depth);
                    if (first) {
                        mn = corner;
                        mx = corner;
                        first = false;
                        continue;
                    }
                    mn = min(mn, corner);
                    mx = max(mx, corner);
                }
            }
        }
        LcAabb a;
        a.minP[0] = mn.x;
        a.minP[1] = mn.y;
        a.minP[2] = mn.z;
        a.pad0 = 0.0;
        a.maxP[0] = mx.x;
        a.maxP[1] = mx.y;
        a.maxP[2] = mx.z;
        a.pad1 = 0.0;
        ((LcAabb*)F.aabbs)[i] = a;
    }
    if (i < F.lightCount) {
        const GpuLight light = fuseGpuSceneLights(fuseGpuScene(F.scene))[i];
        LcBounds b;
        b.center[0] = 0.0;
        b.center[1] = 0.0;
        b.center[2] = 0.0;
        b.radius = 0.0;
        b.sliceLo = 1u;
        b.sliceHi = 0u;
        b.type = light.type;
        b.pad = 0u;
        const float3 p = fuseLcVec3(light.position);
        const float radius = light.range;
        const bool clustered = light.type == kFuseLightPoint || light.type == kFuseLightSpot ||
                               light.type == kFuseLightRect || light.type == kFuseLightDisk; // WP-2.2 area lights: range sphere
        if (clustered && radius > 0.0 && !isinf(radius) && !any(isinf(p)) && !any(isnan(p))) {
            const float3 c = fuseLcWorldToView(p, F);
            b.center[0] = c.x;
            b.center[1] = c.y;
            b.center[2] = c.z;
            b.radius = radius;
            const float depth = -c.z;
            const uint sLo = fuseLcSliceFromDepth(clamp(depth - radius, F.nearPlane, F.farPlane), F.slicesZ, F.nearPlane, F.farPlane);
            const uint sHi = fuseLcSliceFromDepth(clamp(depth + radius, F.nearPlane, F.farPlane), F.slicesZ, F.nearPlane, F.farPlane);
            b.sliceLo = sLo > kFuseLcSlicePad ? sLo - kFuseLcSlicePad : 0u;
            b.sliceHi = min(sHi + kFuseLcSlicePad, F.slicesZ - 1u);
        }
        ((LcBounds*)F.bounds)[i] = b;
    }
}
