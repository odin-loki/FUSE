// WP-0.5 proof shader: float twin of rp_twin_float.comp (GLSL). Exercises vector math, builtins
// (normalize, dot, sqrt, exp2, clamp, fract, floor) and push constants; the test requires the
// output buffers of both builds to match bit for bit on Lavapipe.
struct TwinPush {
    uint count;
    float scale;
    float bias;
    uint pad;
};

[[vk::push_constant]] ConstantBuffer<TwinPush> pc;
[[vk::binding(0, 0)]] StructuredBuffer<float4> inputs;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> outputs;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchId: SV_DispatchThreadID) {
    const uint i = dispatchId.x;
    if (i >= pc.count) {
        return;
    }
    const float4 v = inputs[i];
    const float3 n = normalize(v.xyz + float3(1e-3, 1e-3, 1e-3));
    const float d = dot(n, float3(0.2126, 0.7152, 0.0722));
    const float s = sqrt(abs(v.w)) * pc.scale + pc.bias;
    const float e = exp2(clamp(d * 4.0, -8.0, 8.0));
    outputs[i] = float4(n * s, fract(e) + floor(v.w) * 0.5);
}
