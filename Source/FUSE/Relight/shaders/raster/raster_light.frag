// FUSE Relight RL-4.2: deferred lighting of the raster remaster's G-buffer into FUSE's output image (display space):
// background -> the game's clear colour; unlit (sky) -> its colour as is; lit -> the RL-4.3 BSDF over the GPU scene's
// lights (raster_lighting.glsl) + emissive + ambient, then D3D fog.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "raster_lighting.glsl"

layout(location = 0) out vec4 oColor;

void main() {
    const RasterFrame f = RasterFrameRef(pc.frame).f;
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const uint smp = f.handles.z;
    const vec4 emissive = fuse_fetch_2d(f.gbuffer.z, smp, p, 0);
    if (emissive.w < 0.5) {
        oColor = vec4(f.clearColor.rgb, 1.0);
        return;
    }
    const vec4 albedo = fuse_fetch_2d(f.gbuffer.x, smp, p, 0);
    if (emissive.w > 1.5) {
        oColor = vec4(emissive.rgb, 1.0);
        return;
    }
    const vec4 normal = fuse_fetch_2d(f.gbuffer.y, smp, p, 0);
    const vec4 position = fuse_fetch_2d(f.gbuffer.w, smp, p, 0);
    vec3 display = rasterToDisplay(rasterShade(f, albedo.rgb, normal.w, albedo.w, normalize(normal.xyz), position.xyz,
                                               emissive.rgb, gl_FragCoord.xy));
    if (position.w > 1.5) {
        display = rasterFog(f, display, position.xyz);
    }
    oColor = vec4(display, 1.0);
}
