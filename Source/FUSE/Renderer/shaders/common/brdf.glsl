// B5.3 — PBR BRDF helpers (P5 §5.3 reference shader)
// CPU mirror: fuse::renderer::Brdf (src/material/brdf.cpp).

#ifndef FUSE_BRDF_GLSL
#define FUSE_BRDF_GLSL

const float FUSE_PI = 3.14159265358979;
const float FUSE_MIN_ROUGHNESS = 0.045;

// GGX normal distribution function — D term
float D_GGX(float NoH, float roughness) {
    float r  = clamp(roughness, FUSE_MIN_ROUGHNESS, 1.0);
    float a  = r * r;
    float a2 = a * a;
    float d  = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (FUSE_PI * d * d);
}

// Height-correlated Smith visibility — V = G / (4 NoV NoL). It already contains the
// Cook-Torrance denominator, so the specular lobe is D * V * F (no further division).
float G_SmithGGX(float NoV, float NoL, float roughness) {
    float r  = clamp(roughness, FUSE_MIN_ROUGHNESS, 1.0);
    float a  = r * r;
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick Fresnel — F term
vec3 F_Schlick(float VoH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

// Full Cook-Torrance BRDF evaluation
vec3 brdf_evaluate(
    vec3  albedo,
    float roughness,
    float metallic,
    vec3  N,
    vec3  V,
    vec3  L,
    vec3  light_color,
    float light_intensity
) {
    vec3  H    = normalize(V + L);
    float NoV  = max(dot(N, V), 1e-4);
    float NoL  = max(dot(N, L), 0.0);
    float NoH  = max(dot(N, H), 0.0);
    float VoH  = max(dot(V, H), 0.0);

    metallic   = clamp(metallic, 0.0, 1.0);
    vec3  F0   = mix(vec3(0.04), albedo, metallic);
    vec3  F    = F_Schlick(VoH, F0);
    float D    = D_GGX(NoH, roughness);
    float Vis  = G_SmithGGX(NoV, NoL, roughness);

    vec3  spec = D * Vis * F;
    vec3  diff = (1.0 - F) * (1.0 - metallic) * albedo / FUSE_PI;

    return (diff + spec) * light_color * light_intensity * NoL;
}

#endif // FUSE_BRDF_GLSL
