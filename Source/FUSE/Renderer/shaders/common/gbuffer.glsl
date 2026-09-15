// B5.2 — G-buffer encoding helpers (P5 §5.2 reference shader)

#ifndef FUSE_GBUFFER_GLSL
#define FUSE_GBUFFER_GLSL

// Octahedral normal encoding — 2 floats instead of 3, fully reversible
vec2 encode_normal(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 o = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
    return o * 0.5 + 0.5;
}

vec3 decode_normal(vec2 enc) {
    enc = enc * 2.0 - 1.0;
    vec3 n = vec3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    }
    return normalize(n);
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
    out_rt0 = vec4(encode_normal(normal), ao);
    out_rt1 = vec4(albedo, 1.0);
    out_rt2 = vec4(roughness, metallic, length(emissive) > 0.001 ? 1.0 : 0.0,
                   float(shading_model) / 255.0);
    out_rt3 = vec4(velocity, 0.0, 0.0);
    out_rt5 = vec4(emissive, 0.0);
}

#endif // FUSE_GBUFFER_GLSL
