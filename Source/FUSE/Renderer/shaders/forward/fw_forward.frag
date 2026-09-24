#version 460
// WP-2.3 forward transparency, fragment stage. Early fragment tests (depth LESS against the opaque
// visibility-buffer depth, no depth writes), so only visible fragments are shaded and dumped.
//   1. material: fuse_mr_shade (the WP-1.5 resolve's material evaluation and write_gbuffer packing),
//      hardware UV derivatives taken first, in uniform control flow;
//   2. the packed surface quantised like the G-buffer attachments, then decoded exactly as light.shade
//      decodes RT0 / RT1 / RT2 / RT5;
//   3. world position and cluster from gl_FragCoord (pixel centre + device depth) with the oracle's
//      functions, lighting = fuse_fw_shade (light.shade's loop over the same frame's cluster lists);
//   4. out = (radiance * opacity, opacity), blended ONE / ONE_MINUS_SRC_ALPHA.
// GLSL twin of fw_forward_fs.slang.
#extension GL_GOOGLE_include_directive : require
#include "fw_includes.glsl"

layout(early_fragment_tests) in;

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) flat in float inSign;
layout(location = 4) flat in uint inDraw;

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 dx = dFdx(inUv);
    const vec2 dy = dFdy(inUv);
    const FuseFwFrame f = fuse_fw_frame();
    const FuseLcFrame F = FuseLcFrameRef(f.lighting).f;
    const FuseFwDraw draw = FuseFwDrawsRef(f.draws).v[inDraw];
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(f.scene);
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[draw.slot];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const uint material = fuse_mr_triangle_material(scene, inst, mesh, uint(gl_PrimitiveID));
    const FuseMrGBuffer g = fuse_mr_shade(scene, material, f.sampler_, FUSE_MR_FEATURE_TEXTURES | FUSE_MR_FEATURE_NORMAL_MAP,
                                          inUv, dx, dy, inNormal, inTangent, inSign, gl_FragCoord.z, vec2(0.0));
    const vec4 rt0 = fuse_fw_half4(g.rt0);
    const vec4 rt1 = fuse_fw_unorm8(g.rt1);
    const vec4 rt2 = fuse_fw_unorm8(g.rt2);
    const vec4 rt5 = fuse_fw_half4(g.rt5);

    // gl_FragCoord.xy is the pixel centre (p + 0.5), as light.shade's (float(p) + 0.5).
    precise float sx = gl_FragCoord.x * F.invWidth;
    precise float sy = gl_FragCoord.y * F.invHeight;
    const float viewDepth = fuse_lc_view_depth(gl_FragCoord.z, F.nearPlane, F.farPlane, (F.flags & FUSE_LC_FLAG_REVERSED_Z) != 0u);
    uint cluster = 0u;
    if (!(viewDepth > 0.0) || isinf(viewDepth) || !fuse_lc_map_cluster(sx, sy, viewDepth, F, cluster)) {
        discard;
    }
    FuseLcSurface s;
    s.position = fuse_lc_view_to_world(fuse_lc_view_position(sx, sy, viewDepth, F), F);
    s.normal = fuse_lc_oct_decode(rt0.x, rt0.y);
    s.ao = rt0.w;
    s.albedo = rt1.xyz;
    s.roughness = rt2.x;
    s.metallic = rt2.y;
    s.emissive = rt5.xyz;
    precise vec3 toCamera = fuse_lc_vec3(F.cameraPosition) - s.position;
    const vec3 v = fuse_lc_safe_normalize(toCamera, s.normal);
    const vec3 radiance = fuse_fw_shade(F, s, v, cluster);
    const float alpha = clamp(draw.opacity, 0.0, 1.0);
    precise vec3 premultiplied = radiance * alpha;
    outColor = vec4(premultiplied, alpha);
    if (f.dump != 0ul) {
        const uint p = uint(gl_FragCoord.y) * f.width + uint(gl_FragCoord.x);
        FuseFwDumpTexel t;
        t.radiance = vec4(radiance, alpha);
        t.rt0 = rt0;
        t.rt1 = rt1;
        t.rt2 = rt2;
        t.rt5 = rt5;
        t.depth = gl_FragCoord.z;
        t.covered = 1u;
        t.slot = draw.slot;
        t.cluster = cluster;
        FuseFwDumpRef(f.dump).v[inDraw * f.width * f.height + p] = t;
    }
}
