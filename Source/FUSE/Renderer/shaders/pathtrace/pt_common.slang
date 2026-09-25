// WP-7.3 path-tracing mode: records, random numbers, the BSDF and the surface interaction. Slang twin of
// pt_common.glsl (see there; C++ mirrors pt_types.hpp / pt_bsdf.hpp). Pure buffer-device-address code (no
// descriptor). #include after lt_common.slang; pt_integrator.slang follows the stage-specific ray functions.
#pragma once

// gpu_scene.slang only needs the address table for its handle helpers, which the path tracer never calls.
#define FUSE_BINDLESS_ADDRESS_TABLE_DECLARED 1
uint64_t fuse_buffer_address(uint handle) { return 0; }
#include "gpu_scene.slang"

static const uint kPtTile = 8u;
static const uint kPtInvalid = 0xFFFFFFFFu;
static const float kPtPi = 3.14159265358979323846;
static const float kPtShadowShorten = 0.9999;
static const uint kPtMaskAll = 0x7Fu; // rt::kRtMaskAll: bit 7 (kRtMaskDead) is never traced
static const float kPtDefaultAlbedo = 0.8;

static const uint kPtFlagNee = 1u;
static const uint kPtFlagEmitterHits = 2u;
static const uint kPtFlagRussianRoulette = 4u;
static const uint kPtFlagAccumulate = 8u;
static const uint kPtFlagGuides = 16u;
static const uint kPtFlagClamp = 32u;

static const uint kPtRayRadiance = 0u;
static const uint kPtRayShadow = 1u;
static const uint kPtRayTypes = 2u;

// PtFrameConstants, 272 bytes.
struct PtFrame {
    uint64_t tlas;
    uint64_t scene;
    uint64_t lightTree;
    uint64_t lights;
    uint64_t emitterMap;
    uint64_t accum;
    uint64_t accumSq;
    uint64_t mean;
    uint64_t signal;
    uint64_t depth;
    uint64_t normal;
    uint64_t albedo;
    float invViewProj[16];
    float cameraPosition[3];
    uint width;
    float cameraForward[3];
    uint height;
    float sky[3];
    uint flags;
    uint frameIndex;
    uint seed;
    uint samplesPerFrame;
    uint maxBounces;
    uint rrStartBounce;
    uint lightCount;
    uint cullMask;
    uint emitterMapSlots;
    float rayTMin;
    float normalBias;
    float viewBias;
    float clampRadiance;
    float farDistance;
    float minRoughness;
    uint sampleBase;
    uint reserved;
};

// PtPush, 16 bytes.
struct PtPush {
    uint64_t frame;
    uint pass;
    uint reserved;
};

// PtHit, 80 bytes (the RT pipeline's surface record).
struct PtHit {
    float position[3];
    float t;
    float normal[3];
    uint instance;
    float albedo[3];
    uint primitive;
    float emission[3];
    uint emitter;
    float roughness;
    float metallic;
    uint flags;
    uint reserved;
};

// restir::RestirLight, 32 bytes.
struct PtLight {
    float radiance[3];
    uint kind;
    float cosInner;
    float cosOuter;
    uint flags;
    uint reserved;
};

// Material::GPUMaterial, 128 bytes.
struct PtMaterialRow {
    float4 baseColor;
    float4 roughnessEmissive;
    uint textures[6];
    float emissiveIntensity;
    float normalStrength;
    float4 rest[4];
};

// The RT pipeline's ray payload (pt_payload.glsl), both ray types.
struct PtPayload {
    PtHit hit;
    uint originInstance;
    uint originPrimitive;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]] ConstantBuffer<PtPush> pc;

PtFrame ptFrame() { return ((PtFrame*)pc.frame)[0]; }
float3 ptLoad(float v[3]) { return float3(v[0], v[1], v[2]); }

// --- random numbers -------------------------------------------------------------------------------------------
uint ptHash(uint x) {
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint ptSeed(uint pixel, uint sampleIndex, uint seed) { return ptHash(pixel ^ ptHash(sampleIndex ^ ptHash(seed))); }
float ptRand(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word >> 8u) * (1.0 / 16777216.0);
}

// --- vectors -------------------------------------------------------------------------------------------------
float ptLuminance(float3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
float ptMax3(float3 c) { return max(c.x, max(c.y, c.z)); }
void ptBasis(float3 n, out float3 t, out float3 b) {
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// --- BSDF (pt_bsdf.hpp) ---------------------------------------------------------------------------------------
struct PtMaterial {
    float3 albedo;
    float metallic;
    float roughness;
};

float ptAlpha(float roughness, float minRoughness) {
    const float r = min(max(roughness, minRoughness), 1.0);
    return r * r;
}
float ptGgxD(float alpha, float cosH) {
    const float a2 = alpha * alpha;
    const float k = cosH * cosH * (a2 - 1.0) + 1.0;
    return a2 / (kPtPi * k * k);
}
float ptGgxLambda(float alpha, float c) {
    const float c2 = c * c;
    const float t2 = (1.0 - c2) / c2;
    return (sqrt(1.0 + alpha * alpha * t2) - 1.0) * 0.5;
}
float3 ptSchlick(float3 f0, float c) {
    const float x = 1.0 - c;
    const float x2 = x * x;
    const float x5 = x2 * x2 * x;
    return f0 + (float3(1.0, 1.0, 1.0) - f0) * x5;
}
float3 ptBsdfEval(PtMaterial m, float minRoughness, float3 wo, float3 wi) {
    if (!(wo.z > 0.0) || !(wi.z > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    float3 f = m.albedo * ((1.0 - m.metallic) / kPtPi);
    if (m.metallic > 0.0) {
        const float alpha = ptAlpha(m.roughness, minRoughness);
        const float3 h = normalize(wo + wi);
        const float d = ptGgxD(alpha, h.z);
        const float g2 = 1.0 / (1.0 + ptGgxLambda(alpha, wo.z) + ptGgxLambda(alpha, wi.z));
        const float3 fr = ptSchlick(m.albedo, max(dot(wo, h), 0.0));
        f += fr * (m.metallic * d * g2 / (4.0 * wo.z * wi.z));
    }
    return f;
}
float ptBsdfPdf(PtMaterial m, float minRoughness, float3 wo, float3 wi) {
    if (!(wo.z > 0.0) || !(wi.z > 0.0)) {
        return 0.0;
    }
    float pdf = (1.0 - m.metallic) * wi.z / kPtPi;
    if (m.metallic > 0.0) {
        const float alpha = ptAlpha(m.roughness, minRoughness);
        const float3 h = normalize(wo + wi);
        const float g1 = 1.0 / (1.0 + ptGgxLambda(alpha, wo.z));
        pdf += m.metallic * g1 * ptGgxD(alpha, h.z) / (4.0 * wo.z);
    }
    return pdf;
}
float3 ptSampleVndf(float alpha, float3 wo, float u1, float u2) {
    const float3 vh = normalize(float3(alpha * wo.x, alpha * wo.y, wo.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u1);
    const float phi = 2.0 * kPtPi * u2;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    const float3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0));
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
}
float3 ptSampleCosine(float u1, float u2) {
    const float r = sqrt(u1);
    const float phi = 2.0 * kPtPi * u2;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u1, 0.0)));
}
bool ptBsdfSample(PtMaterial m, float minRoughness, float3 wo, float uLobe, float u1, float u2, out float3 wi, out float pdf,
                  out float3 weight) {
    wi = float3(0.0, 0.0, 0.0);
    pdf = 0.0;
    weight = float3(0.0, 0.0, 0.0);
    if (!(wo.z > 0.0)) {
        return false;
    }
    if (uLobe < m.metallic) {
        const float3 h = ptSampleVndf(ptAlpha(m.roughness, minRoughness), wo, u1, u2);
        wi = h * (2.0 * dot(wo, h)) - wo;
    } else {
        wi = ptSampleCosine(u1, u2);
    }
    if (!(wi.z > 0.0)) {
        return false;
    }
    pdf = ptBsdfPdf(m, minRoughness, wo, wi);
    if (!(pdf > 0.0)) {
        return false;
    }
    weight = ptBsdfEval(m, minRoughness, wo, wi) * (wi.z / pdf);
    return true;
}
float ptPowerHeuristic(float a, float b) {
    const float a2 = a * a;
    const float b2 = b * b;
    return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;
}

// --- surface interaction ---------------------------------------------------------------------------------------
float3 ptVertex(GpuSceneHeader* scene, GpuMesh mesh, GpuTransform xf, uint primitive, uint k) {
    const uint vi = ((uint*)scene.indexAddress)[mesh.firstIndex + 3u * primitive + k];
    uint* vpos = (uint*)mesh.positions;
    const uint w0 = vpos[vi * 2u];
    const uint w1 = vpos[vi * 2u + 1u];
    const float3 l = float3(mesh.quantOffset[0] + float(w0 & 0xFFFFu) * mesh.quantStep[0],
                            mesh.quantOffset[1] + float(w0 >> 16u) * mesh.quantStep[1],
                            mesh.quantOffset[2] + float(w1 & 0xFFFFu) * mesh.quantStep[2]);
    return float3(dot(xf.rows[0].xyz, l) + xf.rows[0].w, dot(xf.rows[1].xyz, l) + xf.rows[1].w, dot(xf.rows[2].xyz, l) + xf.rows[2].w);
}

uint ptEmitterOf(PtFrame F, uint instance, uint primitive) {
    if (F.emitterMap == 0 || F.lightTree == 0 || instance >= F.emitterMapSlots) {
        return kPtInvalid;
    }
    uint* map = (uint*)F.emitterMap;
    const uint base = map[instance];
    return base == kPtInvalid ? kPtInvalid : map[base + primitive];
}

PtHit ptSurface(PtFrame F, uint instance, uint primitive, float bu, float bv, float t) {
    PtHit h;
    GpuSceneHeader* scene = (GpuSceneHeader*)F.scene;
    const GpuInstance inst = ((GpuInstance*)scene.addresses[kFuseGpuSceneInstances])[instance];
    const GpuTransform xf = ((GpuTransform*)scene.addresses[kFuseGpuSceneTransforms])[instance];
    const GpuMesh mesh = ((GpuMesh*)scene.addresses[kFuseGpuSceneMeshes])[inst.mesh];
    const float3 w0 = ptVertex(scene, mesh, xf, primitive, 0u);
    const float3 w1 = ptVertex(scene, mesh, xf, primitive, 1u);
    const float3 w2 = ptVertex(scene, mesh, xf, primitive, 2u);
    const float3 e1 = w1 - w0;
    const float3 e2 = w2 - w0;
    const float3 p = w0 + e1 * bu + e2 * bv;
    const float3 n = normalize(cross(e1, e2));
    h.position[0] = p.x;
    h.position[1] = p.y;
    h.position[2] = p.z;
    h.t = t;
    h.normal[0] = n.x;
    h.normal[1] = n.y;
    h.normal[2] = n.z;
    h.instance = instance;
    h.primitive = primitive;
    float3 albedo = float3(kPtDefaultAlbedo, kPtDefaultAlbedo, kPtDefaultAlbedo);
    float metallic = 0.0;
    float roughness = 1.0;
    float3 emission = float3(0.0, 0.0, 0.0);
    if (inst.material < scene.counts[kFuseGpuSceneMaterials]) {
        const PtMaterialRow row = ((PtMaterialRow*)scene.addresses[kFuseGpuSceneMaterials])[inst.material];
        albedo = max(row.baseColor.xyz, float3(0.0, 0.0, 0.0));
        metallic = clamp(row.baseColor.w, 0.0, 1.0);
        roughness = clamp(row.roughnessEmissive.x, 0.0, 1.0);
        emission = max(row.roughnessEmissive.yzw, float3(0.0, 0.0, 0.0)) * max(row.emissiveIntensity, 0.0);
    }
    h.albedo[0] = albedo.x;
    h.albedo[1] = albedo.y;
    h.albedo[2] = albedo.z;
    h.emission[0] = emission.x;
    h.emission[1] = emission.y;
    h.emission[2] = emission.z;
    h.emitter = ptEmitterOf(F, instance, primitive);
    h.roughness = roughness;
    h.metallic = metallic;
    h.flags = 0u;
    h.reserved = 0u;
    return h;
}

PtHit ptMissRecord() {
    PtHit h;
    h.position[0] = 0.0;
    h.position[1] = 0.0;
    h.position[2] = 0.0;
    h.t = -1.0;
    h.normal[0] = 0.0;
    h.normal[1] = 0.0;
    h.normal[2] = 0.0;
    h.instance = kPtInvalid;
    h.albedo[0] = 0.0;
    h.albedo[1] = 0.0;
    h.albedo[2] = 0.0;
    h.primitive = kPtInvalid;
    h.emission[0] = 0.0;
    h.emission[1] = 0.0;
    h.emission[2] = 0.0;
    h.emitter = kPtInvalid;
    h.roughness = 1.0;
    h.metallic = 0.0;
    h.flags = 0u;
    h.reserved = 0u;
    return h;
}
