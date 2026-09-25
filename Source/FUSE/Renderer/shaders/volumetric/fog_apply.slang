// WP-8.1 "fog.apply". Slang twin of fog_apply.comp (see there).
#include "fog_common.slang"

[[vk::push_constant]] ConstantBuffer<FogPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_fog_textures[];
[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_fog_images[];

float fogViewDepth(FogFrame* R, float d) {
    if ((R.flags & kFogFlagReversedZ) != 0u) {
        const float z = R.depthNear / d;
        return d > 0.0 ? z : 0.0;
    }
    if (d >= 1.0 || R.depthFar <= R.depthNear) {
        return 0.0;
    }
    return (R.depthNear * R.depthFar) / (R.depthFar - d * (R.depthFar - R.depthNear));
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    FogFrame* R = (FogFrame*)pc.frame;
    const uint2 p = tid.xy;
    if (p.x >= R.width || p.y >= R.height) {
        return;
    }
    const int2 ip = int2(p);
    const float device = fuse_fog_textures[R.inputDepth & 0xFFFFFu].Load(int3(ip, 0)).x;
    const float4 lit = fuse_fog_textures[R.inputLit & 0xFFFFFu].Load(int3(ip, 0));
    float depth = fogViewDepth(R, device);
    if (!(depth > 0.0) || isinf(depth)) {
        depth = R.farPlane;
    }
    const float sx = (float(p.x) + 0.5) * R.invWidth;
    const float sy = (float(p.y) + 0.5) * R.invHeight;
    const float4 fog = fogSample(R, (float4*)R.integrated, sx, sy, depth);
    const float4 result = float4(lit.x * fog.w + fog.x, lit.y * fog.w + fog.y, lit.z * fog.w + fog.z, lit.w);
    fuse_fog_images[R.output_ & 0xFFFFFu][p] = result;
    if (R.dump != 0) {
        ((float4*)R.dump)[p.y * R.width + p.x] = result;
    }
}
