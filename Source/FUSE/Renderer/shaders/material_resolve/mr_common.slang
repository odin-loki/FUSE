// FUSE material resolve (WP-1.5): Slang twin of mr_common.glsl (see there) and of
// include/fuse/renderer/material_resolve/resolve_kernel.hpp. #include after gpu_scene.slang,
// vis_format.slang, vis_common.slang and mr_gbuffer.slang. Compile with -fp-mode precise (Slang does
// not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kFuseMrBinEmpty = 0u;
static const uint kFuseMrBinFlat = 1u;
static const uint kFuseMrBinTextured = 2u;
static const uint kFuseMrBinNormalMapped = 3u;
static const uint kFuseMrBinCount = 4u;
static const uint kFuseMrBinUber = 4u;
static const uint kFuseMrTile = 8u;
static const uint kFuseMrFeatureTextures = 1u;
static const uint kFuseMrFeatureNormalMap = 2u;
static const uint kFuseMrAttrEmpty = 0u;
static const uint kFuseMrAttrOk = 1u;
static const uint kFuseMrAttrBadId = 2u;
static const uint kFuseMrAttrDegenerate = 3u;
static const uint kFuseMrNoMaterial = 0xFFFFFFFFu;
static const uint kFuseMrBinListOffsetWords = 16u;

[[vk::binding(1, 0)]] Texture2D<float4> fuse_mr_textures[];
[[vk::binding(2, 0)]] SamplerState fuse_mr_samplers[];

// ResolveFrameConstants (resolve_types.hpp), 176 bytes.
struct MrFrame {
    float4 viewProj[4];
    float4 prevViewProj[4];
    uint width;
    uint height;
    uint tilesX;
    uint tilesY;
    uint scene;
    uint vis;
    uint sampler_;
    uint tileCapacity;
    uint64_t bins;
    uint binsHandle;
    uint flags;
};

uint fuseMrBinFeatures(uint bin) {
    return bin >= kFuseMrBinNormalMapped ? (kFuseMrFeatureTextures | kFuseMrFeatureNormalMap)
                                         : (bin == kFuseMrBinTextured ? kFuseMrFeatureTextures : 0u);
}

// --- vertices ------------------------------------------------------------------------------------
struct MrVertex {
    float3 position;
    float3 normal;
    float3 tangent;
    float sign_;
    float2 uv;
};

float fuseMrSnorm16(uint bits16) {
    const int q = int(bits16 << 16) >> 16;
    const float v = float(q) * asfloat(0x38000100u);
    return clamp(v, -1.0, 1.0);
}

float3 fuseMrOctDecode(uint packed) {
    float x = fuseMrSnorm16(packed & 0xFFFFu);
    float y = fuseMrSnorm16(packed >> 16u);
    const float z = 1.0 - abs(x) - abs(y);
    const float t = z < 0.0 ? -z : 0.0;
    x += x >= 0.0 ? -t : t;
    y += y >= 0.0 ? -t : t;
    const float len = sqrt(x * x + y * y + z * z);
    const float inv = len > 0.0 ? 1.0 / len : 0.0;
    return float3(x * inv, y * inv, z * inv);
}


MrVertex fuseMrVertex(GpuMesh mesh, uint v) {
    MrVertex r;
    r.position = fuseVisMeshPosition(mesh, v);
    uint* vpos = (uint*)mesh.positions;
    r.sign_ = ((vpos[v * 2u + 1u] >> 16u) & 1u) != 0u ? -1.0 : 1.0;
    r.normal = float3(0.0, 0.0, 1.0);
    r.tangent = float3(1.0, 0.0, 0.0);
    r.uv = float2(0.0);
    if (mesh.normals != 0ull) {
        r.normal = fuseMrOctDecode(((uint*)mesh.normals)[v]);
    }
    if (mesh.tangents != 0ull) {
        r.tangent = fuseMrOctDecode(((uint*)mesh.tangents)[v]);
    }
    if (mesh.uvs != 0ull) {
        r.uv = fuseUnpackHalf2x16(((uint*)mesh.uvs)[v]);
    }
    return r;
}

// --- transforms ----------------------------------------------------------------------------------
float3 fuseMrTransformVector(GpuTransform t, float3 v) {
    float3 r;
    r.x = t.rows[0].x * v.x + t.rows[0].y * v.y + t.rows[0].z * v.z;
    r.y = t.rows[1].x * v.x + t.rows[1].y * v.y + t.rows[1].z * v.z;
    r.z = t.rows[2].x * v.x + t.rows[2].y * v.y + t.rows[2].z * v.z;
    return r;
}

float fuseMrDetSign(GpuTransform t) {
    const float3 a = t.rows[0].xyz;
    const float3 b = t.rows[1].xyz;
    const float3 c = t.rows[2].xyz;
    const float d = a.x * (b.y * c.z - b.z * c.y) + a.y * (b.z * c.x - b.x * c.z) + a.z * (b.x * c.y - b.y * c.x);
    return d < 0.0 ? -1.0 : 1.0;
}

float3 fuseMrTransformNormal(GpuTransform t, float3 n) {
    const float3 a = t.rows[0].xyz;
    const float3 b = t.rows[1].xyz;
    const float3 c = t.rows[2].xyz;
    const float sg = fuseMrDetSign(t);
    const float3 bc = float3(b.y * c.z - b.z * c.y, b.z * c.x - b.x * c.z, b.x * c.y - b.y * c.x);
    const float3 ca = float3(c.y * a.z - c.z * a.y, c.z * a.x - c.x * a.z, c.x * a.y - c.y * a.x);
    const float3 ab = float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    float3 r;
    r.x = (bc.x * n.x + bc.y * n.y + bc.z * n.z) * sg;
    r.y = (ca.x * n.x + ca.y * n.y + ca.z * n.z) * sg;
    r.z = (ab.x * n.x + ab.y * n.y + ab.z * n.z) * sg;
    return r;
}

// --- barycentrics and their analytic derivatives -------------------------------------------------
struct MrBary {
    float3 b;
    float3 dbdx;
    float3 dbdy;
    float depth;
    uint flags;
};

MrBary fuseMrBary(float4 c0, float4 c1, float4 c2, float2 ndc, float2 s) {
    MrBary r;
    r.b = float3(0.0);
    r.dbdx = float3(0.0);
    r.dbdy = float3(0.0);
    r.depth = 1.0;
    const float u0x = c0.x - ndc.x * c0.w;
    const float u0y = c0.y - ndc.y * c0.w;
    const float u1x = c1.x - ndc.x * c1.w;
    const float u1y = c1.y - ndc.y * c1.w;
    const float u2x = c2.x - ndc.x * c2.w;
    const float u2y = c2.y - ndc.y * c2.w;
    const float e0 = u1x * u2y - u1y * u2x;
    const float e1 = u2x * u0y - u2y * u0x;
    const float e2 = u0x * u1y - u0y * u1x;
    const float sum = e0 + e1 + e2;
    if (sum == 0.0) {
        r.flags = kFuseMrAttrDegenerate;
        return r;
    }
    const float b0 = e0 / sum;
    const float b1 = e1 / sum;
    const float b2 = e2 / sum;
    const float z = b0 * c0.z + b1 * c1.z + b2 * c2.z;
    const float w = b0 * c0.w + b1 * c1.w + b2 * c2.w;
    r.depth = z / w;
    r.b = float3(b0, b1, b2);
    const float bx0 = c1.y * c2.w - c1.w * c2.y;
    const float bx1 = c2.y * c0.w - c2.w * c0.y;
    const float bx2 = c0.y * c1.w - c0.w * c1.y;
    const float cy0 = c1.w * c2.x - c1.x * c2.w;
    const float cy1 = c2.w * c0.x - c2.x * c0.w;
    const float cy2 = c0.w * c1.x - c0.x * c1.w;
    const float sbx = bx0 + bx1 + bx2;
    const float scy = cy0 + cy1 + cy2;
    r.dbdx = float3((bx0 - b0 * sbx) / sum * s.x, (bx1 - b1 * sbx) / sum * s.x, (bx2 - b2 * sbx) / sum * s.x);
    r.dbdy = float3((cy0 - b0 * scy) / sum * s.y, (cy1 - b1 * scy) / sum * s.y, (cy2 - b2 * scy) / sum * s.y);
    r.flags = kFuseMrAttrOk;
    return r;
}

float fuseMrInterp(float3 b, float a0, float a1, float a2) { return b.x * a0 + b.y * a1 + b.z * a2; }

// --- materials -----------------------------------------------------------------------------------
uint fuseMrMaterialBin(GpuMaterial m) {
    if (m.normal_tex != kFuseInvalidTexture) {
        return kFuseMrBinNormalMapped;
    }
    if (m.base_color_tex != kFuseInvalidTexture || m.roughness_tex != kFuseInvalidTexture ||
        m.metallic_tex != kFuseInvalidTexture || m.ao_tex != kFuseInvalidTexture || m.emissive_tex != kFuseInvalidTexture) {
        return kFuseMrBinTextured;
    }
    return kFuseMrBinFlat;
}

uint fuseMrTriangleMaterial(GpuSceneHeader* scene, GpuInstance inst, GpuMesh mesh, uint triangle) {
    if (inst.material == kFuseGpuSceneInvalid) {
        return kFuseMrNoMaterial;
    }
    uint local = 0u;
    if (mesh.submeshes != 0ull && mesh.meshlets != 0ull) {
        GpuSubmesh* subs = (GpuSubmesh*)mesh.submeshes;
        GpuMeshlet* meshlets = (GpuMeshlet*)mesh.meshlets;
        for (uint i = 0u; i < mesh.submeshCount; ++i) {
            const GpuSubmesh sub = subs[i];
            if (sub.meshletCount == 0u || sub.meshletOffset >= mesh.meshletCount) {
                continue;
            }
            const uint first = meshlets[sub.meshletOffset].triangleOffset;
            if (triangle >= first && triangle - first < sub.triangleCount) {
                local = sub.materialIndex;
                break;
            }
        }
    }
    const uint m = inst.material + local;
    return m < scene.counts[kFuseGpuSceneMaterials] ? m : kFuseMrNoMaterial;
}

GpuMaterial fuseMrMaterial(GpuSceneHeader* scene, uint index) {
    return ((GpuMaterial*)scene.addresses[kFuseGpuSceneMaterials])[index];
}

bool fuseMrIdsValid(GpuSceneHeader* scene, uint instance, uint triangle) {
    if (instance >= scene.counts[kFuseGpuSceneInstances]) {
        return false;
    }
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    if ((inst.flags & kFuseInstanceValid) == 0u || inst.mesh >= scene.counts[kFuseGpuSceneMeshes]) {
        return false;
    }
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    return mesh.indexCount != 0u && scene.indexAddress != 0ull && triangle < mesh.indexCount / 3u;
}

uint fuseMrPixelBin(GpuSceneHeader* scene, uint2 vis) {
    if (vis.x == kFuseVisInvalid || !fuseMrIdsValid(scene, vis.x, vis.y)) {
        return kFuseMrBinEmpty;
    }
    const GpuInstance inst = fuseGpuSceneInstances(scene)[vis.x];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const uint m = fuseMrTriangleMaterial(scene, inst, mesh, vis.y);
    return m == kFuseMrNoMaterial ? kFuseMrBinFlat : fuseMrMaterialBin(fuseMrMaterial(scene, m));
}

// --- attribute reconstruction --------------------------------------------------------------------
struct MrAttributes {
    uint flags;
    float depth;
    MrBary bary;
    float2 uv;
    float2 duvdx;
    float2 duvdy;
    float3 normal;
    float3 tangent;
    float tangentSign;
    float2 velocity;
    uint material;
    uint bin;
};

MrAttributes fuseMrAttributes(GpuSceneHeader* scene, MrFrame f, uint2 pixel, uint2 vis) {
    MrAttributes r;
    r.flags = kFuseMrAttrEmpty;
    r.depth = 1.0;
    r.bary.b = float3(0.0);
    r.bary.dbdx = float3(0.0);
    r.bary.dbdy = float3(0.0);
    r.bary.depth = 1.0;
    r.bary.flags = kFuseMrAttrEmpty;
    r.uv = float2(0.0);
    r.duvdx = float2(0.0);
    r.duvdy = float2(0.0);
    r.normal = float3(0.0);
    r.tangent = float3(0.0);
    r.tangentSign = 1.0;
    r.velocity = float2(0.0);
    r.material = kFuseMrNoMaterial;
    r.bin = kFuseMrBinEmpty;
    if (vis.x == kFuseVisInvalid) {
        return r;
    }
    r.flags = kFuseMrAttrBadId;
    if (!fuseMrIdsValid(scene, vis.x, vis.y)) {
        return r;
    }
    const GpuInstance inst = fuseGpuSceneInstances(scene)[vis.x];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const GpuTransform t = fuseGpuSceneTransforms(scene)[vis.x];
    const GpuTransform pt = fuseGpuScenePrevTransforms(scene)[vis.x];
    uint* indices = fuseGpuSceneIndices(scene);
    const uint base = mesh.firstIndex + vis.y * 3u;
    MrVertex v[3];
    float4 c[3];
    float4 pc[3];
    for (uint k = 0u; k < 3u; ++k) {
        const uint vi = uint(int(indices[base + k]) + mesh.vertexOffset);
        if (vi >= mesh.vertexCount) {
            return r;
        }
        v[k] = fuseMrVertex(mesh, vi);
        c[k] = fuseVisClip(t, f.viewProj, v[k].position);
        pc[k] = fuseVisClip(pt, f.prevViewProj, v[k].position);
    }
    r.material = fuseMrTriangleMaterial(scene, inst, mesh, vis.y);
    r.bin = r.material == kFuseMrNoMaterial ? kFuseMrBinFlat : fuseMrMaterialBin(fuseMrMaterial(scene, r.material));
    const float2 ndc = fuseVisPixelNdc(pixel, f.width, f.height);
    const float2 s = float2(2.0 / float(f.width), 2.0 / float(f.height));
    const MrBary b = fuseMrBary(c[0], c[1], c[2], ndc, s);
    r.bary = b;
    r.flags = b.flags;
    if (b.flags != kFuseMrAttrOk) {
        return r;
    }
    r.depth = b.depth;
    r.uv = float2(fuseMrInterp(b.b, v[0].uv.x, v[1].uv.x, v[2].uv.x), fuseMrInterp(b.b, v[0].uv.y, v[1].uv.y, v[2].uv.y));
    r.duvdx = float2(fuseMrInterp(b.dbdx, v[0].uv.x, v[1].uv.x, v[2].uv.x), fuseMrInterp(b.dbdx, v[0].uv.y, v[1].uv.y, v[2].uv.y));
    r.duvdy = float2(fuseMrInterp(b.dbdy, v[0].uv.x, v[1].uv.x, v[2].uv.x), fuseMrInterp(b.dbdy, v[0].uv.y, v[1].uv.y, v[2].uv.y));
    float3 n[3];
    float3 tg[3];
    for (uint k = 0u; k < 3u; ++k) {
        n[k] = fuseMrTransformNormal(t, v[k].normal);
        tg[k] = fuseMrTransformVector(t, v[k].tangent);
    }
    r.normal = float3(fuseMrInterp(b.b, n[0].x, n[1].x, n[2].x), fuseMrInterp(b.b, n[0].y, n[1].y, n[2].y),
                      fuseMrInterp(b.b, n[0].z, n[1].z, n[2].z));
    r.tangent = float3(fuseMrInterp(b.b, tg[0].x, tg[1].x, tg[2].x), fuseMrInterp(b.b, tg[0].y, tg[1].y, tg[2].y),
                       fuseMrInterp(b.b, tg[0].z, tg[1].z, tg[2].z));
    r.tangentSign = v[0].sign_ * fuseMrDetSign(t);
    const float px = fuseMrInterp(b.b, pc[0].x, pc[1].x, pc[2].x);
    const float py = fuseMrInterp(b.b, pc[0].y, pc[1].y, pc[2].y);
    const float pw = fuseMrInterp(b.b, pc[0].w, pc[1].w, pc[2].w);
    if (pw > 0.0) {
        const float prevX = (px / pw * 0.5 + 0.5) * float(f.width);
        const float prevY = (py / pw * 0.5 + 0.5) * float(f.height);
        r.velocity = float2((float(pixel.x) + 0.5) - prevX, (float(pixel.y) + 0.5) - prevY);
    }
    return r;
}

// --- material evaluation + G-buffer packing ------------------------------------------------------
float4 fuseMrSample(uint tex, uint smp, float2 uv, float2 dx, float2 dy) {
    return fuse_mr_textures[NonUniformResourceIndex(tex & 0xFFFFFu)].SampleGrad(
        fuse_mr_samplers[NonUniformResourceIndex(smp & 0xFFFFFu)], uv, dx, dy);
}

struct MrGBuffer {
    float4 rt0;
    float4 rt1;
    float4 rt2;
    float4 rt3;
    float4 rt4;
    float4 rt5;
    uint material;
};

MrGBuffer fuseMrGBufferEmpty() {
    MrGBuffer g;
    g.rt0 = float4(0.0);
    g.rt1 = float4(0.0);
    g.rt2 = float4(0.0);
    g.rt3 = float4(0.0);
    g.rt4 = float4(1.0, 0.0, 0.0, 0.0);
    g.rt5 = float4(0.0);
    g.material = kFuseMrNoMaterial;
    return g;
}

MrGBuffer fuseMrShade(GpuSceneHeader* scene, uint material, uint smp, uint features, float2 uv, float2 dx, float2 dy,
                      float3 nInterp, float3 tInterp, float tSign, float depth, float2 velocity) {
    float3 albedo = float3(1.0);
    float roughness = 0.5;
    float metallic = 0.0;
    float ao = 1.0;
    float3 emissive = float3(0.0);
    uint shading = 0u;
    const float nl = nInterp.x * nInterp.x + nInterp.y * nInterp.y + nInterp.z * nInterp.z;
    float3 N = nl > 0.0 ? nInterp / sqrt(nl) : float3(0.0, 0.0, 1.0);
    if (material != kFuseMrNoMaterial) {
        const GpuMaterial m = fuseMrMaterial(scene, material);
        albedo = m.base_color.rgb;
        roughness = m.roughness_emissive.x;
        metallic = m.base_color.w;
        emissive = fuseMaterialEmissiveRadiance(m);
        shading = m.shading_model;
        if ((features & kFuseMrFeatureTextures) != 0u) {
            if (m.base_color_tex != kFuseInvalidTexture) {
                albedo *= fuseMrSample(m.base_color_tex, smp, uv, dx, dy).rgb;
            }
            if (m.roughness_tex != kFuseInvalidTexture) {
                roughness *= fuseMrSample(m.roughness_tex, smp, uv, dx, dy).r;
            }
            if (m.metallic_tex != kFuseInvalidTexture) {
                metallic *= fuseMrSample(m.metallic_tex, smp, uv, dx, dy).r;
            }
            if (m.ao_tex != kFuseInvalidTexture) {
                ao = fuseMrSample(m.ao_tex, smp, uv, dx, dy).r;
            }
            if (m.emissive_tex != kFuseInvalidTexture) {
                emissive *= fuseMrSample(m.emissive_tex, smp, uv, dx, dy).rgb;
            }
        }
        if ((features & kFuseMrFeatureNormalMap) != 0u && m.normal_tex != kFuseInvalidTexture) {
            const float nt = N.x * tInterp.x + N.y * tInterp.y + N.z * tInterp.z;
            float3 T = tInterp - N * nt;
            const float tl = T.x * T.x + T.y * T.y + T.z * T.z;
            if (tl > 0.0) {
                T = T / sqrt(tl);
                const float3 B = float3(N.y * T.z - N.z * T.y, N.z * T.x - N.x * T.z, N.x * T.y - N.y * T.x) * tSign;
                float3 ts = fuseMrSample(m.normal_tex, smp, uv, dx, dy).xyz * 2.0 - 1.0;
                ts.xy *= m.normal_strength;
                const float3 mapped = T * ts.x + B * ts.y + N * ts.z;
                const float ml = mapped.x * mapped.x + mapped.y * mapped.y + mapped.z * mapped.z;
                if (ml > 0.0) {
                    N = mapped / sqrt(ml);
                }
            }
        }
    }
    const GBufferOut o = fuseWriteGBuffer(N, albedo, roughness, metallic, emissive, ao, velocity, shading);
    MrGBuffer g;
    g.rt0 = o.rt0;
    g.rt1 = o.rt1;
    g.rt2 = o.rt2;
    g.rt3 = o.rt3;
    g.rt4 = float4(depth, 0.0, 0.0, 0.0);
    g.rt5 = o.rt5;
    g.material = material;
    return g;
}
