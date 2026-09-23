// B5.3 — bindless material table (P5 §5.3 reference shader)
// CPU mirror: fuse::renderer::Material::GPUMaterial / MaterialLayout / MaterialEval (src/material/material.cpp).
// Row layout is std430, 128-byte stride; offsets are pinned by static_asserts in material.cpp.

#ifndef FUSE_MATERIAL_GLSL
#define FUSE_MATERIAL_GLSL

const uint FUSE_MATERIAL_FLAG_PROCEDURAL = 1u;
const uint FUSE_MATERIAL_PROCEDURAL_ID_SHIFT = 1u;
const uint FUSE_MATERIAL_PROCEDURAL_ID_MASK = 0x7FFFu;
const uint FUSE_MATERIAL_FLAG_HAS_NORMAL_MAP = 1u << 16;
const uint FUSE_MATERIAL_FLAG_HAS_AO_MAP = 1u << 17;
const uint FUSE_MATERIAL_FLAG_HAS_METALLIC_MAP = 1u << 18;
const uint FUSE_INVALID_TEXTURE = 0xFFFFFFFFu;

struct FuseGpuMaterial {
    vec4  base_color;          // rgb = base colour, w = metallic                    (offset   0)
    vec4  roughness_emissive;  // x = roughness, yzw = emissive colour              (offset  16)
    uint  base_color_tex;      //                                                   (offset  32)
    uint  roughness_tex;
    uint  metallic_tex;
    uint  normal_tex;
    uint  ao_tex;
    uint  emissive_tex;
    float emissive_intensity;  //                                                   (offset  56)
    float normal_strength;
    uint  shading_model;
    uint  flags;               //                                                   (offset  68)
    uint  procedural_seed;
    uint  padding;
    vec4  subsurface;          // rgb = scatter colour, w = scatter radius          (offset  80)
    vec4  clear_coat;          // x = coat, y = coat roughness, z = sheen roughness (offset  96)
    vec4  cloth;               // rgb = sheen colour                                (offset 112)
};

uint fuse_material_procedural_id(FuseGpuMaterial m) {
    return (m.flags >> FUSE_MATERIAL_PROCEDURAL_ID_SHIFT) & FUSE_MATERIAL_PROCEDURAL_ID_MASK;
}

// Emitted radiance (linear) — what the G-buffer RT5 and GI probe rays see.
vec3 fuse_material_emissive_radiance(FuseGpuMaterial m) {
    return max(m.roughness_emissive.yzw, vec3(0.0)) * max(m.emissive_intensity, 0.0);
}

// Radiance leaving a probe-ray hit: emission + Lambertian response of the diffuse albedo.
vec3 fuse_material_probe_ray_radiance(vec3 albedo, float metallic, vec3 emissive, vec3 irradiance) {
    return emissive + albedo * (clamp(1.0 - metallic, 0.0, 1.0) / 3.14159265358979) * irradiance;
}

// Usage: layout(std430, set = S, binding = B) readonly buffer FuseMaterials { FuseGpuMaterial fuse_materials[]; };
//        FuseGpuMaterial m = fuse_materials[nonuniformEXT(material_id)];

#endif // FUSE_MATERIAL_GLSL
