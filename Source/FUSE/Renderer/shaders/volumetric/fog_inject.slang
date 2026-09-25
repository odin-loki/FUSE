// WP-8.1 "fog.inject". Slang twin of fog_inject.comp (see there).
#include "gpu_scene.slang"
#include "lc_common.slang"
#include "../shadow_vsm/vsm_shadow.slang"
#include "fog_common.slang"

[[vk::push_constant]] ConstantBuffer<FogPush> pc;

float fogPhase(float g, float cosTheta) {
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-6);
    return (1.0 - g2) * kFogInv4Pi / (denom * sqrt(denom));
}

float fogFalloff(float d, float radius) {
    if (!(radius > 0.0) || !(d < radius)) {
        return 0.0;
    }
    const float ratio = d / radius;
    const float ratio2 = ratio * ratio;
    const float w = fogClamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    return (w * w) / max(d * d, 1e-4);
}

float fogSpot(float cosAngle, float cosInner, float cosOuter) {
    const float t = fogClamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
    return t * t;
}

float3 fogLightIn(FogFrame* R, GpuLight light, uint slot, float3 p, float3 v) {
    float3 l = float3(0.0);
    float attenuation = 1.0;
    if (light.type == kFuseLightDirectional) {
        l = fogSafeNormalize(float3(-light.direction[0], -light.direction[1], -light.direction[2]), float3(0.0, 0.0, 1.0));
    } else if (light.type == kFuseLightPoint || light.type == kFuseLightSpot) {
        const float3 toLight = float3(light.position[0] - p.x, light.position[1] - p.y, light.position[2] - p.z);
        const float distance = sqrt(fogDot(toLight, toLight));
        attenuation = fogFalloff(distance, light.range);
        if (attenuation == 0.0) {
            return float3(0.0);
        }
        const float inv = 1.0 / max(distance, 1e-6);
        l = float3(toLight.x * inv, toLight.y * inv, toLight.z * inv);
        if (light.type == kFuseLightSpot) {
            const float3 axis = fogSafeNormalize(fogVec3(light.direction), float3(0.0, 0.0, -1.0));
            attenuation = attenuation * fogSpot(-fogDot(l, axis), light.cosInner, light.cosOuter);
            if (attenuation == 0.0) {
                return float3(0.0);
            }
        }
    } else {
        return float3(0.0);
    }
    float visibility = 1.0;
    if ((R.flags & kFogFlagShadows) != 0u) {
        visibility = fuseVsmShadow(R.shadows, slot, p, float3(0.0));
    }
    const float ph = fogPhase(R.anisotropy, fogDot(l, v));
    const float s = light.intensity * attenuation * visibility * ph;
    return float3(light.color[0] * s, light.color[1] * s, light.color[2] * s);
}

bool fogLightCluster(LcFrame F, float3 p, out uint cluster) {
    cluster = 0u;
    const float3 rel = float3(p.x - F.cameraPosition[0], p.y - F.cameraPosition[1], p.z - F.cameraPosition[2]);
    const float vx = fogDot(rel, fuseLcVec3(F.right));
    const float vy = fogDot(rel, fuseLcVec3(F.up));
    const float depth = -fogDot(rel, fuseLcVec3(F.back));
    if (!(depth > 0.0)) {
        return false;
    }
    const float ndcX = vx / (F.tanX * depth);
    const float ndcY = vy / (F.tanY * depth);
    const float sx = (ndcX + 1.0) * 0.5;
    const float sy = (1.0 - ndcY) * 0.5;
    return fuseLcMapCluster(sx, sy, depth, F, cluster);
}

float3 fogInScattering(FogFrame* R, float3 p, float3 v) {
    float3 L = fogVec3(R.ambient);
    if ((R.flags & kFogFlagLights) == 0u) {
        return L;
    }
    const LcFrame F = ((LcFrame*)R.lighting)[0];
    GpuLight* lights = fuseGpuSceneLights(fuseGpuScene(F.scene));
    uint* directional = (uint*)F.directional;
    const uint directionalCount = ((uint*)F.listHeader)[kFuseLcHeaderDirectional];
    for (uint i = 0u; i < directionalCount; ++i) {
        const uint slot = directional[i];
        if (slot < F.lightCount) {
            L = L + fogLightIn(R, lights[slot], slot, p, v);
        }
    }
    uint cluster = 0u;
    if (fogLightCluster(F, p, cluster)) {
        uint* grid = (uint*)F.grid;
        uint* list = (uint*)F.lightList;
        const uint offset = grid[cluster * 2u];
        const uint count = grid[cluster * 2u + 1u];
        for (uint i = 0u; i < count; ++i) {
            const uint slot = list[offset + i];
            if (slot < F.lightCount) {
                L = L + fogLightIn(R, lights[slot], slot, p, v);
            }
        }
    }
    return L;
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
    const float3 p = fogFroxelPoint(R, x, y, z, R.jitter[0], R.jitter[1], R.jitter[2]);
    float3 scattering;
    const float extinction = fogMedium(R, p, scattering);
    float4 result = float4(0.0);
    if (extinction > 0.0) {
        const float3 rel = float3(p.x - R.position[0], p.y - R.position[1], p.z - R.position[2]);
        const float3 v = fogSafeNormalize(rel, float3(-R.back[0], -R.back[1], -R.back[2]));
        const float3 L = fogInScattering(R, p, v);
        result = float4(scattering.x * L.x, scattering.y * L.y, scattering.z * L.z, extinction);
    }
    ((float4*)R.current)[i] = result;
}
