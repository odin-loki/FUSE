// B5.2 — G-buffer encoding helpers (P5 §5.2 reference shader)
// CPU mirror: fuse::renderer::GBufferEncoding / GBufferPacking (src/deferred/gbuffer.cpp).

#ifndef FUSE_GBUFFER_GLSL
#define FUSE_GBUFFER_GLSL

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

// RT0 (RGBA16F) normal encoding: signed oct, snapped to the half-representable neighbour with the
// smallest angular error. Keeps the RGBA16F round trip below 0.001 rad everywhere.
vec2 encode_normal_rgba16f(vec3 n) {
    n = normalize(n);
    vec2 o = oct_encode_signed(n);
    uint hx = packHalf2x16(vec2(o.x, 0.0)) & 0xFFFFu;
    uint hy = packHalf2x16(vec2(o.y, 0.0)) & 0xFFFFu;
    uint cx[3] = uint[3](hx, half_next_down(hx), half_next_up(hx));
    uint cy[3] = uint[3](hy, half_next_down(hy), half_next_up(hy));
    vec2 best = unpackHalf2x16(hx | (hy << 16));
    float best_dot = -2.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            vec2 c = unpackHalf2x16(cx[i] | (cy[j] << 16));
            if (abs(c.x) > 1.0 || abs(c.y) > 1.0) continue;
            float d = dot(oct_decode_signed(c), n);
            if (d > best_dot) {
                best_dot = d;
                best = c;
            }
        }
    }
    return best;
}

const float FUSE_GBUFFER_MAX_HALF = 65504.0;

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
    out_rt0 = vec4(encode_normal_rgba16f(normal), 0.0, ao);
    out_rt1 = vec4(albedo, 1.0);
    out_rt2 = vec4(roughness, metallic, length(emissive) > 0.001 ? 1.0 : 0.0,
                   float(shading_model) / 255.0);
    out_rt3 = vec4(velocity, 0.0, 0.0);
    out_rt5 = vec4(min(emissive, vec3(FUSE_GBUFFER_MAX_HALF)), 0.0);
}

// Inverse of write_gbuffer's normal channel.
vec3 read_gbuffer_normal(vec4 rt0) {
    return oct_decode_signed(rt0.xy);
}

#endif // FUSE_GBUFFER_GLSL
