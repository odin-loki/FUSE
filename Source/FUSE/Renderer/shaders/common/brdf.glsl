// B5.3 — PBR BRDF helpers (P5 §5.3 reference shader)

#ifndef FUSE_BRDF_GLSL
#define FUSE_BRDF_GLSL

const float FUSE_PI = 3.14159265358979;

// GGX normal distribution function — D term
float D_GGX(float NoH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (FUSE_PI * d * d);
}

// Smith masking-shadowing — G term
float G_SmithGGX(float NoV, float NoL, float roughness) {
    float a  = roughness * roughness;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a) + a);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a) + a);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick Fresnel — F term
vec3 F_Schlick(float VoH, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
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

    vec3  F0   = mix(vec3(0.04), albedo, metallic);
    vec3  F    = F_Schlick(VoH, F0);
    float D    = D_GGX(NoH, roughness);
    float G    = G_SmithGGX(NoV, NoL, roughness);

    vec3  spec = (D * G * F) / max(4.0 * NoV * NoL, 1e-5);
    vec3  diff = (1.0 - F) * (1.0 - metallic) * albedo / FUSE_PI;

    return (diff + spec) * light_color * light_intensity * NoL;
}

#endif // FUSE_BRDF_GLSL
