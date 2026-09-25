// FUSE Relight RL-6.1: the developer overlay's compose pass (Slang primary; GLSL twin overlay_compose.comp).
//
// One invocation per texel of the region (a copy of the back buffer's texels as raw 32-bit words): optionally the
// debug view (the debug buffer mapped to display values, nearest sampling), then the CPU-rasterised UI layer over it
// with the integer rule of raster.hpp (composeTexel). Bit-exact with composeRegionCpu for the layer; the debug view's
// float mapping may differ by one 8-bit step (FMA contraction).

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> uRegion;
[[vk::binding(1, 0)]] StructuredBuffer<uint> uLayer;
[[vk::binding(2, 0)]] Sampler2D<float4> uDebug;

// Member for member overlay_types.h OverlayPush (rl_overlay_layout checks the offsets).
struct OverlayPush {
    int regionX;
    int regionY;
    uint regionW;
    uint regionH;
    int panelX;
    int panelY;
    uint panelW;
    uint panelH;
    uint frameW;
    uint frameH;
    uint flags;
    uint debugSwizzle;
    uint debugW;
    uint debugH;
    uint pad0;
    uint pad1;
    float4 debugScale;
    float4 debugBias;
};
[[vk::push_constant]] ConstantBuffer<OverlayPush> pc;

static const uint kFlagBgra = 1u;
static const uint kFlagDebug = 2u;

uint toRgba(uint raw, bool bgra) {
    return bgra ? ((raw & 0xff00ff00u) | ((raw >> 16) & 0xffu) | ((raw & 0xffu) << 16)) : raw;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint2 p = id.xy;
    if (p.x >= pc.regionW || p.y >= pc.regionH) {
        return;
    }
    const bool bgra = (pc.flags & kFlagBgra) != 0u;
    uint base = toRgba(uRegion[p], bgra);
    const uint fx = uint(pc.regionX) + p.x;
    const uint fy = uint(pc.regionY) + p.y;
    if ((pc.flags & kFlagDebug) != 0u && pc.debugW > 0u && pc.debugH > 0u && pc.frameW > 0u && pc.frameH > 0u) {
        const uint sx = min(pc.debugW - 1u, (fx * pc.debugW) / pc.frameW);
        const uint sy = min(pc.debugH - 1u, (fy * pc.debugH) / pc.frameH);
        const float4 t = uDebug.Load(int3(int(sx), int(sy), 0));
        float4 v = clamp(t * pc.debugScale + pc.debugBias, float4(0.0), float4(1.0));
        if (pc.debugSwizzle == 1u) {
            v.g = v.r;
            v.b = v.r;
        } else if (pc.debugSwizzle == 2u) {
            v.b = 0.0;
        }
        const uint3 c = uint3(v.rgb * 255.0 + 0.5) & 0xffu;
        base = (base & 0xff000000u) | c.r | (c.g << 8) | (c.b << 16);
    }
    const int lx = int(fx) - pc.panelX;
    const int ly = int(fy) - pc.panelY;
    if (lx >= 0 && ly >= 0 && uint(lx) < pc.panelW && uint(ly) < pc.panelH) {
        const uint l = uLayer[uint(ly) * pc.panelW + uint(lx)];
        const uint ia = 255u - (l >> 24);
        uint outRgba = base & 0xff000000u;
        for (uint shift = 0u; shift < 24u; shift += 8u) {
            const uint lc = (l >> shift) & 0xffu;
            const uint bc = (base >> shift) & 0xffu;
            outRgba |= ((lc + (bc * ia + 127u) / 255u) & 0xffu) << shift;
        }
        base = outRgba;
    }
    uRegion[p] = toRgba(base, bgra);
}
