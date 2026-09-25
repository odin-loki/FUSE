// WP-8.1 "fog.temporal". Slang twin of fog_temporal.comp (see there).
#include "fog_common.slang"

[[vk::push_constant]] ConstantBuffer<FogPush> pc;

bool fogReproject(FogFrame* R, float3 p, out float gx, out float gy, out float gz) {
    gx = 0.0;
    gy = 0.0;
    gz = 0.0;
    const float3 rel = float3(p.x - R.prevPosition[0], p.y - R.prevPosition[1], p.z - R.prevPosition[2]);
    const float vx = fogDot(rel, fogVec3(R.prevRight));
    const float vy = fogDot(rel, fogVec3(R.prevUp));
    const float depth = -fogDot(rel, fogVec3(R.prevBack));
    if (!(depth >= R.nearPlane) || !(depth <= R.farPlane)) {
        return false;
    }
    const float ndcX = vx / (R.prevTanX * depth);
    const float ndcY = vy / (R.prevTanY * depth);
    const float sx = (ndcX + 1.0) * 0.5;
    const float sy = (1.0 - ndcY) * 0.5;
    if (!(sx >= 0.0) || !(sx <= 1.0) || !(sy >= 0.0) || !(sy <= 1.0)) {
        return false;
    }
    const uint s = fogFindSlice(R, depth);
    const float z0 = R.sliceDepth[s];
    const float z1 = R.sliceDepth[s + 1u];
    gx = sx * float(R.gridX) - 0.5;
    gy = sy * float(R.gridY) - 0.5;
    gz = float(s) + (depth - z0) / (z1 - z0) - 0.5;
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    FogFrame* R = (FogFrame*)pc.frame;
    const uint i = tid.x;
    if (i >= R.froxelCount) {
        return;
    }
    const uint z = i % R.gridZ;
    const uint col = i / R.gridZ;
    const uint x = col % R.gridX;
    const uint y = col / R.gridX;
    const float4 cur = ((float4*)R.current)[i];
    float4 result = cur;
    if ((R.flags & kFogFlagHistory) != 0u) {
        float4* history = (float4*)R.historyPrev;
        bool valid = true;
        float4 h = history[i];
        if ((R.flags & kFogFlagReproject) != 0u) {
            const float3 p = fogFroxelPoint(R, x, y, z, 0.5, 0.5, 0.5);
            float gx;
            float gy;
            float gz;
            valid = fogReproject(R, p, gx, gy, gz);
            if (valid) {
                h = fogSampleGrid(R, history, gx, gy, gz);
            }
        }
        if (valid) {
            result = fogLerp4(h, cur, R.temporalAlpha);
        }
    }
    ((float4*)R.historyCur)[i] = result;
}
