// WP-4.5 depth of field. Slang twin of pp_dof.comp (see there).
#include "pp_common.slang"

float ppCocRadius(float depthM, PpFrame F) {
    const float f = F.dofFocalLength;
    const float s1 = F.dofFocalDistance * 1000.0;
    const float s2 = max(depthM * 1000.0, 1e-3);
    if (s1 <= f) {
        return 0.0;
    }
    const float aperture = f / max(F.dofFStop, 1e-4);
    const float coc = aperture * f * (s2 - s1) / (s2 * (s1 - f));
    if (coc < 0.0 && (F.flags & kPpFlagDofNearBlur) == 0u) {
        return 0.0;
    }
    const float sensor = max(F.dofSensorWidth, 1e-4);
    return 0.5 * coc / sensor * float(F.width);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const PpFrame F = ppFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint index = p.y * F.width + p.x;
    float2* cocs = ppPairs(F.cocs);
    if (pc.mode == 0u) {
        const float depth = ppFetch(F.inputDepth, int2(p)).x;
        const float radius = min(abs(ppCocRadius(depth, F)), max(F.dofMaxRadius, 0.0));
        const int reach = int(floor(radius));
        const float radiusSq = radius * radius;
        uint area = 0u;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                if (float(dx * dx + dy * dy) <= radiusSq) {
                    ++area;
                }
            }
        }
        cocs[index] = float2(radiusSq, 1.0 / float(max(area, 1u)));
        return;
    }
    float4* src = ppTexels(pc.src);
    const int reach = int(F.dofReach);
    float3 accum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (int dy = -reach; dy <= reach; ++dy) {
        const int sy = int(p.y) + dy;
        if (sy < 0 || sy >= int(F.height)) {
            continue;
        }
        for (int dx = -reach; dx <= reach; ++dx) {
            const int sx = int(p.x) + dx;
            if (sx < 0 || sx >= int(F.width)) {
                continue;
            }
            const uint s = uint(sy) * F.width + uint(sx);
            const float2 coc = cocs[s];
            if (float(dx * dx + dy * dy) > coc.x) {
                continue;
            }
            accum = accum + src[s].xyz * coc.y;
            weight += coc.y;
        }
    }
    const float3 result = weight > 0.0 ? accum * (1.0 / weight) : src[index].xyz;
    ppTexels(pc.dst)[index] = float4(result, 0.0);
}
