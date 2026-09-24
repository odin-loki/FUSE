// FUSE visibility buffer (WP-1.4): vertex pulling and triangle decode, Slang twin of
// vis_common.glsl (see there) and of include/fuse/renderer/visbuffer/vis_decode_kernel.hpp.
// #include after gpu_scene.slang and vis_format.slang. Compile with -fp-mode precise (Slang does not
// honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kFuseVisDecodeEmpty = 0u;
static const uint kFuseVisDecodeOk = 1u;
static const uint kFuseVisDecodeBadId = 2u;
static const uint kFuseVisDecodeDegenerate = 3u;

float3 fuseVisMeshPosition(GpuMesh mesh, uint vertex) {
    uint* words = (uint*)mesh.positions;
    const uint xy = words[vertex * 2u];
    const uint zw = words[vertex * 2u + 1u];
    float3 p;
    p.x = mesh.quantOffset[0] + float(xy & 0xFFFFu) * mesh.quantStep[0];
    p.y = mesh.quantOffset[1] + float(xy >> 16u) * mesh.quantStep[1];
    p.z = mesh.quantOffset[2] + float(zw & 0xFFFFu) * mesh.quantStep[2];
    return p;
}

float4 fuseVisClip(GpuTransform t, float4 viewProj[4], float3 p) {
    float3 w;
    w.x = t.rows[0].x * p.x + t.rows[0].y * p.y + t.rows[0].z * p.z + t.rows[0].w;
    w.y = t.rows[1].x * p.x + t.rows[1].y * p.y + t.rows[1].z * p.z + t.rows[1].w;
    w.z = t.rows[2].x * p.x + t.rows[2].y * p.y + t.rows[2].z * p.z + t.rows[2].w;
    return viewProj[0] * w.x + viewProj[1] * w.y + viewProj[2] * w.z + viewProj[3];
}

struct VisDecoded {
    float depth;
    float b1;
    float b2;
    uint flags;
};

VisDecoded fuseVisDecodeClip(float4 c0, float4 c1, float4 c2, float2 ndc) {
    VisDecoded r;
    const float u0x = c0.x - ndc.x * c0.w;
    const float u0y = c0.y - ndc.y * c0.w;
    const float u1x = c1.x - ndc.x * c1.w;
    const float u1y = c1.y - ndc.y * c1.w;
    const float u2x = c2.x - ndc.x * c2.w;
    const float u2y = c2.y - ndc.y * c2.w;
    const float e0 = u1x * u2y - u1y * u2x;
    const float e1 = u2x * u0y - u2y * u0x;
    const float e2 = u0x * u1y - u0y * u1x;
    const float s = e0 + e1 + e2;
    if (s == 0.0) {
        r.depth = 1.0;
        r.b1 = 0.0;
        r.b2 = 0.0;
        r.flags = kFuseVisDecodeDegenerate;
        return r;
    }
    const float b0 = e0 / s;
    const float b1 = e1 / s;
    const float b2 = e2 / s;
    const float z = b0 * c0.z + b1 * c1.z + b2 * c2.z;
    const float w = b0 * c0.w + b1 * c1.w + b2 * c2.w;
    r.depth = z / w;
    r.b1 = b1;
    r.b2 = b2;
    r.flags = kFuseVisDecodeOk;
    return r;
}

float2 fuseVisPixelNdc(uint2 pixel, uint width, uint height) {
    float2 ndc;
    ndc.x = (float(pixel.x) + 0.5) * (2.0 / float(width)) - 1.0;
    ndc.y = (float(pixel.y) + 0.5) * (2.0 / float(height)) - 1.0;
    return ndc;
}

VisDecoded fuseVisDecode(GpuSceneHeader* scene, float4 viewProj[4], uint instance, uint triangle, float2 ndc) {
    VisDecoded r;
    r.depth = 1.0;
    r.b1 = 0.0;
    r.b2 = 0.0;
    r.flags = kFuseVisDecodeEmpty;
    if (instance == kFuseVisInvalid) {
        return r;
    }
    r.flags = kFuseVisDecodeBadId;
    if (instance >= scene.counts[kFuseGpuSceneInstances]) {
        return r;
    }
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    if ((inst.flags & kFuseInstanceValid) == 0u || inst.mesh >= scene.counts[kFuseGpuSceneMeshes]) {
        return r;
    }
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    if (mesh.indexCount == 0u || scene.indexAddress == 0ull || triangle >= mesh.indexCount / 3u) {
        return r;
    }
    uint* indices = fuseGpuSceneIndices(scene);
    const GpuTransform t = fuseGpuSceneTransforms(scene)[instance];
    const uint base = mesh.firstIndex + triangle * 3u;
    const float4 c0 = fuseVisClip(t, viewProj, fuseVisMeshPosition(mesh, uint(int(indices[base]) + mesh.vertexOffset)));
    const float4 c1 = fuseVisClip(t, viewProj, fuseVisMeshPosition(mesh, uint(int(indices[base + 1u]) + mesh.vertexOffset)));
    const float4 c2 = fuseVisClip(t, viewProj, fuseVisMeshPosition(mesh, uint(int(indices[base + 2u]) + mesh.vertexOffset)));
    return fuseVisDecodeClip(c0, c1, c2, ndc);
}
