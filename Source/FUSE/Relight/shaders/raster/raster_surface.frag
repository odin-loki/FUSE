// FUSE Relight RL-4.2: raster remaster fragment stage, compiled three ways:
//   RASTER_GBUFFER  opaque / alpha-tested / sky draws -> the G-buffer (albedo + metallic, normal + roughness,
//                   emissive + shading class, world position + fog flag);
//   RASTER_DECAL    Decal-category draws blended into the G-buffer albedo (depth tested, not written);
//   RASTER_FORWARD  alpha-blended draws, lit (same lighting as the deferred pass) and fogged in display space, with
//                   the draw's D3D blend state on the output (submission order);
//   RASTER_SHADOW   the shadow map's casters: only the alpha test (depth-only pass).
#version 460
#extension GL_GOOGLE_include_directive : require

#include "raster_surface.glsl"
#if defined(RASTER_FORWARD)
#include "raster_lighting.glsl"
#endif

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vColor;
layout(location = 4) flat in uint vDraw;
layout(location = 5) in vec4 vColor1;

#if defined(RASTER_SHADOW)
#elif defined(RASTER_GBUFFER)
layout(location = 0) out vec4 oAlbedo;   // display rgb, a metallic
layout(location = 1) out vec4 oNormal;   // world normal, w roughness
layout(location = 2) out vec4 oEmissive; // display rgb (unlit: the colour), w 1 lit / 2 unlit (0 background)
layout(location = 3) out vec4 oPosition; // world xyz, w 1 / 2 with fog
#else
layout(location = 0) out vec4 oColor;
#endif

void main() {
    const RasterFrame f = RasterFrameRef(pc.frame).f;
    const RasterMaterial m = RasterDrawsRef(pc.draws).v[vDraw].mat;
    const RasterSurface s = rasterEvalSurface(m, vUv, vColor, vColor1, vWorld, vNormal, f.eye.xyz);
    if ((m.flags & RASTER_MAT_ALPHA_TEST) != 0u && !rasterAlphaTest(m.alphaTest, s.color.a)) {
        discard;
    }
#if defined(RASTER_SHADOW)
#elif defined(RASTER_GBUFFER)
    oAlbedo = vec4(s.color.rgb, s.metallic);
    oNormal = vec4(s.normal, s.roughness);
    oEmissive = s.unlit ? vec4(s.color.rgb, 2.0) : vec4(s.emissive, 1.0);
    oPosition = vec4(vWorld, s.fog ? 2.0 : 1.0);
#elif defined(RASTER_DECAL)
    oColor = vec4(s.color.rgb, clamp(s.color.a, 0.0, 1.0));
#else
    vec3 display;
    if (s.unlit) {
        display = s.color.rgb;
    } else {
        display = rasterToDisplay(rasterShade(f, s.color.rgb, s.roughness, s.metallic, s.normal, vWorld, s.emissive,
                                              gl_FragCoord.xy));
        if (s.fog) {
            display = rasterFog(f, display, vWorld);
        }
    }
    oColor = vec4(display, s.color.a);
#endif
}
