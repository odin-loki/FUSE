// FUSE Relight RL-4.2: legacy (fixed-function) surface evaluation of the raster remaster: texture stage 0 of Remix's
// LegacyMaterialData (colour / alpha operation over Texture, the draw's diffuse and TFACTOR), the alpha test, and the
// shading normal. Included by raster_surface.frag (G-buffer, decal and forward variants).
#ifndef FUSE_RELIGHT_RASTER_SURFACE_GLSL
#define FUSE_RELIGHT_RASTER_SURFACE_GLSL

#include "raster_common.glsl"

// scene/translate TextureArgSource: 0 None, 1 Texture, 2 VertexColor0, 3 TFactor. None is what Remix records for
// D3DTA_DIFFUSE / CURRENT when the colour comes from the material: the draw's diffuse (material or COLOR0).
vec4 rasterArg(uint src, vec4 tex, vec4 dif, vec4 tfactor) {
    if (src == 1u) {
        return tex;
    }
    if (src == 2u) {
        return dif;
    }
    if (src == 3u) {
        return tfactor;
    }
    return dif;
}

// scene/translate TextureOperation: 0 Disable, 1 SelectArg1, 2 SelectArg2, 3 Modulate, 4 Modulate2x, 5 Modulate4x,
// 6 Add, 7 Force_Modulate2x.
vec4 rasterOp(uint op, vec4 a1, vec4 a2, vec4 current) {
    switch (op) {
    case 0u:
        return current;
    case 1u:
        return a1;
    case 2u:
        return a2;
    case 4u:
    case 7u:
        return clamp(2.0 * a1 * a2, 0.0, 1.0);
    case 5u:
        return clamp(4.0 * a1 * a2, 0.0, 1.0);
    case 6u:
        return clamp(a1 + a2, 0.0, 1.0);
    default:
        return a1 * a2;
    }
}

struct RasterSurface {
    vec4 color;    // display space, a = coverage alpha
    vec3 emissive; // display space
    vec3 normal;   // world, unit, facing the viewer
    float roughness;
    float metallic;
    bool unlit;
    bool fog;
};

// VkCompareOp on the 8-bit alpha (D3D9 compares the render target's precision).
bool rasterAlphaTest(uint packed, float alpha) {
    const uint op = packed & 0xffu;
    const uint a = uint(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    const uint ref = (packed >> 8) & 0xffu;
    switch (op) {
    case 0u:
        return false;
    case 1u:
        return a < ref;
    case 2u:
        return a == ref;
    case 3u:
        return a <= ref;
    case 4u:
        return a > ref;
    case 5u:
        return a != ref;
    case 6u:
        return a >= ref;
    default:
        return true;
    }
}

RasterSurface rasterEvalSurface(RasterMaterial m, vec2 uv, vec4 vertexColor, vec4 vertexColor1, vec3 world,
                                vec3 vertexNormal, vec3 eye) {
    RasterSurface s;
    const vec4 dif = (m.flags & RASTER_MAT_VERTEX_COLOR) != 0u ? vertexColor : m.diffuse;
    vec4 tex = vec4(1.0);
    if ((m.flags & RASTER_MAT_TEXTURED) != 0u && m.texture != 0u) {
        tex = fuse_sample_2d(m.texture, m.samplerHandle, uv);
    }
    const uint ops = m.ops;
    const vec4 c = rasterOp(ops & 15u, rasterArg((ops >> 4) & 15u, tex, dif, m.tfactor),
                            rasterArg((ops >> 8) & 15u, tex, dif, m.tfactor), dif);
    const vec4 a = rasterOp((ops >> 12) & 15u, rasterArg((ops >> 16) & 15u, tex, dif, m.tfactor),
                            rasterArg((ops >> 20) & 15u, tex, dif, m.tfactor), dif);
    s.color = vec4(c.rgb, a.a);
    if ((m.flags & RASTER_MAT_REPLACEMENT) != 0u) {
        s.color.rgb = m.diffuse.rgb * ((m.flags & RASTER_MAT_TEXTURED) != 0u ? tex.rgb : vec3(1.0));
    }
    // D3DRS_EMISSIVEMATERIALSOURCE: the vertex's COLOR0 / COLOR1 instead of the material's emissive.
    s.emissive = (m.flags & RASTER_MAT_EMISSIVE_COLOR0) != 0u
                     ? vertexColor.rgb
                     : ((m.flags & RASTER_MAT_EMISSIVE_COLOR1) != 0u ? vertexColor1.rgb : m.emissive.rgb);
    vec3 n = (m.flags & RASTER_MAT_HAS_NORMALS) != 0u ? vertexNormal : vec3(0.0);
    if (dot(n, n) < 1e-12) {
        n = cross(dFdx(world), dFdy(world));
    }
    n = dot(n, n) > 1e-20 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    // Legacy geometry is often single-sided or inconsistently wound: shade the side facing the viewer.
    if (dot(n, eye - world) < 0.0) {
        n = -n;
    }
    s.normal = n;
    s.roughness = m.roughness;
    s.metallic = m.metallic;
    s.unlit = (m.flags & RASTER_MAT_UNLIT) != 0u;
    s.fog = (m.flags & RASTER_MAT_FOG) != 0u;
    return s;
}

#endif // FUSE_RELIGHT_RASTER_SURFACE_GLSL
