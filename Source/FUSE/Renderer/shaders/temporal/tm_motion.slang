// WP-4.1 "temporal.motion". Slang twin of tm_motion.comp (see there) and of motion_kernel.hpp.
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "tm_common.slang"

[[vk::binding(0, 0)]] [format("rg32ui")] RWTexture2D<uint2> fuse_tm_vis_images[];

struct TmMotion {
    float2 motion;
    float depth;
};

float2 fuseTmClipUv(float4 c) {
    float2 uv;
    uv.x = c.x / c.w * 0.5 + 0.5;
    uv.y = c.y / c.w * 0.5 + 0.5;
    return uv;
}

TmMotion fuseTmSky(TmMotionFrame f, uint2 p) {
    TmMotion r;
    r.motion = float2(0.0, 0.0);
    r.depth = 0.0;
    if ((f.flags & kFuseTmMotionSkyValid) == 0u) {
        return r;
    }
    const float sx = float(p.x) + 0.5 + f.jitterX;
    const float sy = float(p.y) + 0.5 + f.jitterY;
    const float nx = sx * (2.0 / float(f.width)) - 1.0;
    const float ny = sy * (2.0 / float(f.height)) - 1.0;
    const float px = f.skyReproj[0].x * nx + f.skyReproj[1].x * ny + f.skyReproj[2].x;
    const float py = f.skyReproj[0].y * nx + f.skyReproj[1].y * ny + f.skyReproj[2].y;
    const float pw = f.skyReproj[0].z * nx + f.skyReproj[1].z * ny + f.skyReproj[2].z;
    if (pw > 0.0) {
        const float cu = sx / float(f.width);
        const float cv = sy / float(f.height);
        r.motion = float2(cu - (px / pw * 0.5 + 0.5), cv - (py / pw * 0.5 + 0.5));
    }
    return r;
}

TmMotion fuseTmMotion(GpuSceneHeader* scene, TmMotionFrame f, uint2 p, uint2 vis) {
    const uint instance = vis.x;
    const uint triangle = vis.y;
    if (instance == kFuseVisInvalid) {
        return fuseTmSky(f, p);
    }
    TmMotion r;
    r.motion = float2(0.0, 0.0);
    r.depth = 0.0;
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
    float3 v[3];
    float4 c[3];
    for (uint k = 0u; k < 3u; ++k) {
        const uint vi = uint(int(indices[base + k]) + mesh.vertexOffset);
        if (vi >= mesh.vertexCount) {
            return r;
        }
        v[k] = fuseVisMeshPosition(mesh, vi);
        c[k] = fuseVisClip(t, f.drawViewProj, v[k]);
    }
    const float2 ndc = fuseVisPixelNdc(p, f.width, f.height);
    const float u0x = c[0].x - ndc.x * c[0].w;
    const float u0y = c[0].y - ndc.y * c[0].w;
    const float u1x = c[1].x - ndc.x * c[1].w;
    const float u1y = c[1].y - ndc.y * c[1].w;
    const float u2x = c[2].x - ndc.x * c[2].w;
    const float u2y = c[2].y - ndc.y * c[2].w;
    const float e0 = u1x * u2y - u1y * u2x;
    const float e1 = u2x * u0y - u2y * u0x;
    const float e2 = u0x * u1y - u0y * u1x;
    const float s = e0 + e1 + e2;
    if (s == 0.0) {
        return r;
    }
    const float b0 = e0 / s;
    const float b1 = e1 / s;
    const float b2 = e2 / s;
    float3 obj;
    obj.x = b0 * v[0].x + b1 * v[1].x + b2 * v[2].x;
    obj.y = b0 * v[0].y + b1 * v[1].y + b2 * v[2].y;
    obj.z = b0 * v[0].z + b1 * v[1].z + b2 * v[2].z;
    const float4 cc = fuseVisClip(t, f.viewProj, obj);
    const float4 pcl = fuseVisClip(fuseGpuScenePrevTransforms(scene)[instance], f.prevViewProj, obj);
    if (cc.w > 0.0) {
        r.depth = cc.w;
        if (pcl.w > 0.0) {
            r.motion = fuseTmClipUv(cc) - fuseTmClipUv(pcl);
        }
    }
    return r;
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const TmMotionFrame f = ((TmMotionFrame*)pc.frame)[0];
    const uint2 p = tid.xy;
    if (p.x >= f.width || p.y >= f.height) {
        return;
    }
    const uint2 vis = fuse_tm_vis_images[f.vis & 0xFFFFFu][p];
    const TmMotion m = fuseTmMotion(fuseGpuScene(f.scene), f, p, vis);
    const uint i = p.y * f.width + p.x;
    float2* motion = (float2*)f.motion;
    float* depth = (float*)f.depth;
    motion[i] = m.motion;
    depth[i] = m.depth;
}
