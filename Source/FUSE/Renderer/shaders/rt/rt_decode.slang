// WP-6.0 rt_decode, Slang twin of rt_decode.comp (see there). Compiled with -fp-mode precise.
#include "rt_common.slang"

struct Push { // RtDecodePush
    uint* src;
    float* dst;
    uint vertexCount;
    float qo0, qo1, qo2;
    float qs0, qs1, qs2;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.vertexCount) {
        return;
    }
    const uint xy = pc.src[i * 2u + 0u];
    const uint zw = pc.src[i * 2u + 1u];
    const float x = pc.qo0 + float(xy & 0xFFFFu) * pc.qs0;
    const float y = pc.qo1 + float(xy >> 16u) * pc.qs1;
    const float z = pc.qo2 + float(zw & 0xFFFFu) * pc.qs2;
    pc.dst[i * 3u + 0u] = x;
    pc.dst[i * 3u + 1u] = y;
    pc.dst[i * 3u + 2u] = z;
}
