// B5.2 — G-buffer encoding helpers (P5 §5.2 reference shader)
// CPU mirror: fuse::renderer::GBufferEncoding / GBufferPacking (src/deferred/gbuffer.cpp).

#ifndef FUSE_GBUFFER_GLSL
#define FUSE_GBUFFER_GLSL

#include "exact_math.glsl" // fuse_div_rn / fuse_sqrt_rn (no shaderInt64)

// sign() returns 0 for 0; the octahedral fold needs +/-1 or (0,0,-1) collapses onto +Z.
vec2 sign_not_zero(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Signed octahedral mapping, [-1,1]^2.
vec2 oct_encode_signed(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
}

vec3 oct_decode_signed(vec2 o) {
    vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    }
    return normalize(n);
}

// Octahedral normal encoding, unsigned [0,1]^2 form (UNORM targets).
vec2 encode_normal(vec3 n) {
    return oct_encode_signed(n) * 0.5 + 0.5;
}

vec3 decode_normal(vec2 enc) {
    return oct_decode_signed(enc * 2.0 - 1.0);
}

uint half_next_up(uint h) {
    if ((h & 0x7FFFu) == 0u) return 1u;
    return (h & 0x8000u) != 0u ? h - 1u : h + 1u;
}

uint half_next_down(uint h) {
    if ((h & 0x7FFFu) == 0u) return 0x8001u;
    return (h & 0x8000u) != 0u ? h + 1u : h - 1u;
}

const float FUSE_GBUFFER_MAX_HALF = 65504.0;

// --- device-independent attachment quantisation ------------------------------------------------
// Vulkan leaves the float -> UNORM8 / float -> f16 conversion of colour attachment writes partly to the
// implementation: a value on (or within an ulp of) a rounding midpoint may go either way. G-buffer
// inputs hit midpoints often (albedo 0.5 -> 127.5, 0.9f -> 229.5 after f32 rounding, roughness
// 0.3f -> 76.5): Lavapipe rounds them to even, the RTX 3090 down, so a whole material's albedo
// moved by one code between devices. The shader therefore rounds itself and writes a value the
// conversion cannot misplace: UNORM8 channels round(x * 255) to nearest even (the f32 product, as
// Lavapipe's conversion does) and store k / 255 (any conversion within half a code of k returns k);
// f16 channels are rounded to the nearest-even half in integer arithmetic and stored exactly.
vec4 fuse_gbuffer_unorm8(vec4 v) {
    precise vec4 scaled = clamp(v, vec4(0.0), vec4(1.0)) * 255.0;
    return roundEven(scaled) / 255.0;
}

// f32 -> nearest-even f16 bits (GBufferQuantize::floatToHalf). Overflow saturates to infinity, NaN stays
// NaN, half subnormals are rounded to the 2^-24 grid.
uint fuse_gbuffer_half_bits(float v) {
    uint u = floatBitsToUint(v);
    const uint sign = u & 0x80000000u;
    u ^= sign;
    uint h;
    if (u >= 0x47800000u) { // >= 65536 (or inf / NaN)
        h = u > 0x7F800000u ? 0x7E00u : 0x7C00u;
    } else if (u < 0x38800000u) { // below 2^-14: the f32 add rounds to the 2^-24 grid (nearest even)
        precise float t = uintBitsToFloat(u) + 0.5;
        h = floatBitsToUint(t) - 0x3F000000u;
    } else { // rebias the exponent, round the 13 dropped mantissa bits to nearest even
        const uint odd = (u >> 13u) & 1u;
        u += 0xC8000FFFu;
        u += odd;
        h = u >> 13u;
    }
    return h | (sign >> 16u);
}

// f16 bits -> f32, exactly (GBufferQuantize::halfToFloat), half subnormals included: built from the bits
// instead of unpackHalf2x16, which may flush them.
float fuse_gbuffer_half_value(uint h) {
    const uint sign = (h & 0x8000u) << 16u;
    const uint e = (h >> 10u) & 0x1Fu;
    const uint m = h & 0x3FFu;
    if (e == 0u) {
        precise float s = float(m) * 5.9604644775390625e-8; // m * 2^-24, exact
        return uintBitsToFloat(floatBitsToUint(s) | sign);
    }
    if (e == 0x1Fu) {
        return uintBitsToFloat(sign | 0x7F800000u | (m << 13u));
    }
    return uintBitsToFloat(sign | ((e + 112u) << 23u) | (m << 13u));
}

float fuse_gbuffer_half(float v) { return fuse_gbuffer_half_value(fuse_gbuffer_half_bits(v)); }

vec4 fuse_gbuffer_half4(vec4 v) {
    return vec4(fuse_gbuffer_half(v.x), fuse_gbuffer_half(v.y), fuse_gbuffer_half(v.z), fuse_gbuffer_half(v.w));
}

// --- RT0 normal encoding, bit-identical to GBufferEncoding::encodeNormalRgba16f ----------------
// The snap to the best half neighbour compares dot products that differ in the last bits, and the
// GLSL builtins it used (normalize, `/`, dot, packHalf2x16) are not correctly rounded on every
// device. Every step is now the CPU mirror's IEEE operation in its order (`precise`; fuse_div_rn /
// fuse_sqrt_rn; integer half conversions), so any conformant device writes the CPU encoder's bits.
float fuse_gbuffer_dot3(vec3 a, vec3 b) {
    precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
    return d;
}

vec3 fuse_gbuffer_normalized(vec3 v) { // fuse::math::Vec3::normalized
    const float len = fuse_sqrt_rn(fuse_gbuffer_dot3(v, v));
    if (len < 1e-8) {
        return vec3(0.0);
    }
    const float inv = fuse_div_rn(1.0, len);
    precise vec3 r = vec3(v.x * inv, v.y * inv, v.z * inv);
    return r;
}

float fuse_gbuffer_sign_not_zero(float v) { return v >= 0.0 ? 1.0 : -1.0; }

vec2 fuse_gbuffer_oct_encode(vec3 n) { // GBufferEncoding::encodeOctSigned
    precise float sum = abs(n.x) + abs(n.y) + abs(n.z);
    if (sum < 1e-20) {
        return vec2(0.0);
    }
    const float inv = fuse_div_rn(1.0, sum);
    precise vec3 m = vec3(n.x * inv, n.y * inv, n.z * inv);
    if (m.z >= 0.0) {
        return m.xy;
    }
    precise vec2 r = vec2((1.0 - abs(m.y)) * fuse_gbuffer_sign_not_zero(m.x),
                          (1.0 - abs(m.x)) * fuse_gbuffer_sign_not_zero(m.y));
    return r;
}

vec3 fuse_gbuffer_oct_decode(vec2 o) { // GBufferEncoding::decodeOctSigned
    precise vec3 n = vec3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0) {
        precise vec2 f = vec2((1.0 - abs(n.y)) * fuse_gbuffer_sign_not_zero(n.x),
                              (1.0 - abs(n.x)) * fuse_gbuffer_sign_not_zero(n.y));
        n.xy = f;
    }
    return fuse_gbuffer_normalized(n);
}

// RT0 (RGBA16F) normal encoding: signed oct, snapped to the half-representable neighbour with the
// smallest angular error. Keeps the RGBA16F round trip below 0.001 rad everywhere.
vec2 encode_normal_rgba16f(vec3 normal) {
    const vec3 n = fuse_gbuffer_normalized(normal);
    const vec2 o = fuse_gbuffer_oct_encode(n);
    const uint hx = fuse_gbuffer_half_bits(o.x);
    const uint hy = fuse_gbuffer_half_bits(o.y);
    const uint cx[3] = uint[3](hx, half_next_down(hx), half_next_up(hx));
    const uint cy[3] = uint[3](hy, half_next_down(hy), half_next_up(hy));
    vec2 best = vec2(fuse_gbuffer_half_value(hx), fuse_gbuffer_half_value(hy));
    float best_dot = -2.0;
    for (int i = 0; i < 3; ++i) {
        const float x = fuse_gbuffer_half_value(cx[i]);
        if (abs(x) > 1.0) {
            continue;
        }
        for (int j = 0; j < 3; ++j) {
            const float y = fuse_gbuffer_half_value(cy[j]);
            if (abs(y) > 1.0) {
                continue;
            }
            const float d = fuse_gbuffer_dot3(fuse_gbuffer_oct_decode(vec2(x, y)), n);
            if (d > best_dot) {
                best_dot = d;
                best = vec2(x, y);
            }
        }
    }
    return best;
}

// Pack G-buffer from fragment shader outputs
void write_gbuffer(
    vec3 normal,
    vec3 albedo,
    float roughness,
    float metallic,
    vec3 emissive,
    float ao,
    vec2 velocity,
    uint shading_model,
    out vec4 out_rt0,
    out vec4 out_rt1,
    out vec4 out_rt2,
    out vec4 out_rt3,
    out vec4 out_rt5
) {
    out_rt0 = vec4(encode_normal_rgba16f(normal), 0.0, fuse_gbuffer_half(ao));
    out_rt1 = fuse_gbuffer_unorm8(vec4(albedo, 1.0));
    out_rt2 = fuse_gbuffer_unorm8(vec4(roughness, metallic, length(emissive) > 0.001 ? 1.0 : 0.0,
                                       float(shading_model) / 255.0));
    out_rt3 = fuse_gbuffer_half4(vec4(velocity, 0.0, 0.0));
    out_rt5 = fuse_gbuffer_half4(vec4(min(emissive, vec3(FUSE_GBUFFER_MAX_HALF)), 0.0));
}

// Inverse of write_gbuffer's normal channel.
vec3 read_gbuffer_normal(vec4 rt0) {
    return oct_decode_signed(rt0.xy);
}

#endif // FUSE_GBUFFER_GLSL
