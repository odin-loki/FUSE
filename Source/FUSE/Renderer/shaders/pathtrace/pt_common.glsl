// WP-7.3 path-tracing mode: records, random numbers, the BSDF and the surface interaction. GLSL twin of
// pt_common.slang. The C++ mirrors are include/fuse/renderer/pathtrace/pt_types.hpp (records; fuse_rp_pathtrace_layout
// checks names, order and offsets) and pt_bsdf.hpp (the BSDF the CPU reference evaluates in double precision).
//
// Pure buffer-device-address code (no descriptor): the frame constants come from the push constant, every table
// from an address in them. Include after lt_common.glsl; the integrator (pt_integrator.glsl) follows the
// stage-specific ray functions.
#ifndef FUSE_PT_COMMON_GLSL
#define FUSE_PT_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// gpu_scene.glsl only needs fuse_buffer_address for its handle helpers, which the path tracer never calls.
uint64_t fuse_buffer_address(uint handle) { return uint64_t(0); }
#include "gpu_scene.glsl"

#define PT_TILE 8
#define PT_INVALID 0xFFFFFFFFu
#define PT_PI 3.14159265358979323846
#define PT_SHADOW_SHORTEN 0.9999
#define PT_MASK_ALL 0x7Fu // rt::kRtMaskAll: bit 7 (kRtMaskDead) is never traced
#define PT_DEFAULT_ALBEDO 0.8

#define PT_FLAG_NEE 1u
#define PT_FLAG_EMITTER_HITS 2u
#define PT_FLAG_RUSSIAN_ROULETTE 4u
#define PT_FLAG_ACCUMULATE 8u
#define PT_FLAG_GUIDES 16u
#define PT_FLAG_CLAMP 32u

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

// restir::RestirLight, 32 bytes (the RGB light table shared with WP-7.2).
struct PtLight {
    float radiance[3];
    uint kind;
    float cosInner;
    float cosOuter;
    uint flags;
    uint reserved;
};

// Material::GPUMaterial, 128 bytes: base colour + metallic, roughness + emissive colour, emissive intensity.
struct PtMaterialRow {
    vec4 baseColor;
    vec4 roughnessEmissive;
    uint textures[6];
    float emissiveIntensity;
    float normalStrength;
    vec4 rest[4];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PtFrameRef { PtFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer PtVec4Ref { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer PtFloatRef { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PtLightsRef { PtLight v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PtWordsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PtMaterialsRef { PtMaterialRow v[]; };

layout(push_constant) uniform PtPushBlock {
    PtPush p;
} pc;

PtFrame pt_frame() { return PtFrameRef(pc.p.frame).f; }
vec3 pt_load(float v[3]) { return vec3(v[0], v[1], v[2]); }

// --- random numbers (PCG, O'Neill 2014; hash of Jarzynski and Olano 2020) --------------------------------------
uint pt_hash(uint x) {
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint pt_seed(uint pixel, uint sampleIndex, uint seed) { return pt_hash(pixel ^ pt_hash(sampleIndex ^ pt_hash(seed))); }
float pt_rand(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word >> 8u) * (1.0 / 16777216.0);
}

// --- vectors -------------------------------------------------------------------------------------------------
float pt_luminance(vec3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
float pt_max3(vec3 c) { return max(c.x, max(c.y, c.z)); }
void pt_basis(vec3 n, out vec3 t, out vec3 b) {
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// --- BSDF (pt_bsdf.hpp) ---------------------------------------------------------------------------------------
struct PtMaterial {
    vec3 albedo;
    float metallic;
    float roughness;
};

float pt_alpha(float roughness, float minRoughness) {
    const float r = min(max(roughness, minRoughness), 1.0);
    return r * r;
}
float pt_ggx_d(float alpha, float cosH) {
    const float a2 = alpha * alpha;
    const float k = cosH * cosH * (a2 - 1.0) + 1.0;
    return a2 / (PT_PI * k * k);
}
float pt_ggx_lambda(float alpha, float c) {
    const float c2 = c * c;
    const float t2 = (1.0 - c2) / c2;
    return (sqrt(1.0 + alpha * alpha * t2) - 1.0) * 0.5;
}
vec3 pt_schlick(vec3 f0, float c) {
    const float x = 1.0 - c;
    const float x2 = x * x;
    const float x5 = x2 * x2 * x;
    return f0 + (vec3(1.0) - f0) * x5;
}
vec3 pt_bsdf_eval(PtMaterial m, float minRoughness, vec3 wo, vec3 wi) {
    if (!(wo.z > 0.0) || !(wi.z > 0.0)) {
        return vec3(0.0);
    }
    vec3 f = m.albedo * ((1.0 - m.metallic) / PT_PI);
    if (m.metallic > 0.0) {
        const float alpha = pt_alpha(m.roughness, minRoughness);
        const vec3 h = normalize(wo + wi);
        const float d = pt_ggx_d(alpha, h.z);
        const float g2 = 1.0 / (1.0 + pt_ggx_lambda(alpha, wo.z) + pt_ggx_lambda(alpha, wi.z));
        const vec3 fr = pt_schlick(m.albedo, max(dot(wo, h), 0.0));
        f += fr * (m.metallic * d * g2 / (4.0 * wo.z * wi.z));
    }
    return f;
}
float pt_bsdf_pdf(PtMaterial m, float minRoughness, vec3 wo, vec3 wi) {
    if (!(wo.z > 0.0) || !(wi.z > 0.0)) {
        return 0.0;
    }
    float pdf = (1.0 - m.metallic) * wi.z / PT_PI;
    if (m.metallic > 0.0) {
        const float alpha = pt_alpha(m.roughness, minRoughness);
        const vec3 h = normalize(wo + wi);
        const float g1 = 1.0 / (1.0 + pt_ggx_lambda(alpha, wo.z));
        pdf += m.metallic * g1 * pt_ggx_d(alpha, h.z) / (4.0 * wo.z);
    }
    return pdf;
}
vec3 pt_sample_vndf(float alpha, vec3 wo, float u1, float u2) {
    const vec3 vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
    const vec3 t2 = cross(vh, t1);
    const float r = sqrt(u1);
    const float phi = 2.0 * PT_PI * u2;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    const vec3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0));
    return normalize(vec3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
}
vec3 pt_sample_cosine(float u1, float u2) {
    const float r = sqrt(u1);
    const float phi = 2.0 * PT_PI * u2;
    return vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u1, 0.0)));
}
// Direction (local frame), mixture pdf and weight f cos / pdf; false when the sample is below the surface.
bool pt_bsdf_sample(PtMaterial m, float minRoughness, vec3 wo, float uLobe, float u1, float u2, out vec3 wi, out float pdf,
                    out vec3 weight) {
    wi = vec3(0.0);
    pdf = 0.0;
    weight = vec3(0.0);
    if (!(wo.z > 0.0)) {
        return false;
    }
    if (uLobe < m.metallic) {
        const vec3 h = pt_sample_vndf(pt_alpha(m.roughness, minRoughness), wo, u1, u2);
        wi = h * (2.0 * dot(wo, h)) - wo;
    } else {
        wi = pt_sample_cosine(u1, u2);
    }
    if (!(wi.z > 0.0)) {
        return false;
    }
    pdf = pt_bsdf_pdf(m, minRoughness, wo, wi);
    if (!(pdf > 0.0)) {
        return false;
    }
    weight = pt_bsdf_eval(m, minRoughness, wo, wi) * (wi.z / pdf);
    return true;
}
float pt_power_heuristic(float a, float b) {
    const float a2 = a * a;
    const float b2 = b * b;
    return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;
}

// --- surface interaction ---------------------------------------------------------------------------------------
// World position of vertex k of triangle `primitive` of scene instance `instance` (the WP-6.0 rtDecodePosition +
// the row-major 3x4 transform).
vec3 pt_vertex(FuseGpuSceneHeaderRef scene, FuseGpuMesh mesh, FuseGpuTransform xf, uint primitive, uint k) {
    const uint vi = FuseGpuSceneIndicesRef(scene.indexAddress).v[mesh.firstIndex + 3u * primitive + k];
    PtWordsRef vpos = PtWordsRef(mesh.positions);
    const uint w0 = vpos.v[vi * 2u];
    const uint w1 = vpos.v[vi * 2u + 1u];
    const vec3 l = vec3(mesh.quantOffset[0] + float(w0 & 0xFFFFu) * mesh.quantStep[0],
                        mesh.quantOffset[1] + float(w0 >> 16u) * mesh.quantStep[1],
                        mesh.quantOffset[2] + float(w1 & 0xFFFFu) * mesh.quantStep[2]);
    return vec3(dot(xf.rows[0].xyz, l) + xf.rows[0].w, dot(xf.rows[1].xyz, l) + xf.rows[1].w, dot(xf.rows[2].xyz, l) + xf.rows[2].w);
}

// Light-tree emitter of a traced triangle (pathtrace.hpp ptEmitterLookup), PT_INVALID when none.
uint pt_emitter_of(PtFrame F, uint instance, uint primitive) {
    if (F.emitterMap == uint64_t(0) || F.lightTree == uint64_t(0) || instance >= F.emitterMapSlots) {
        return PT_INVALID;
    }
    PtWordsRef map = PtWordsRef(F.emitterMap);
    const uint base = map.v[instance];
    return base == PT_INVALID ? PT_INVALID : map.v[base + primitive];
}

// The surface record of a hit (closest-hit shader / ray-query kernel): position from the barycentrics, geometric
// normal, the instance's material row, the emitter.
PtHit pt_surface(PtFrame F, uint instance, uint primitive, float bu, float bv, float t) {
    PtHit h;
    FuseGpuSceneHeaderRef scene = FuseGpuSceneHeaderRef(F.scene);
    const FuseGpuInstance inst = FuseGpuInstancesRef(scene.addresses[FUSE_GPU_SCENE_INSTANCES]).v[instance];
    const FuseGpuTransform xf = FuseGpuTransformsRef(scene.addresses[FUSE_GPU_SCENE_TRANSFORMS]).v[instance];
    const FuseGpuMesh mesh = FuseGpuMeshesRef(scene.addresses[FUSE_GPU_SCENE_MESHES]).v[inst.mesh];
    const vec3 w0 = pt_vertex(scene, mesh, xf, primitive, 0u);
    const vec3 w1 = pt_vertex(scene, mesh, xf, primitive, 1u);
    const vec3 w2 = pt_vertex(scene, mesh, xf, primitive, 2u);
    const vec3 e1 = w1 - w0;
    const vec3 e2 = w2 - w0;
    const vec3 p = w0 + e1 * bu + e2 * bv;
    const vec3 n = normalize(cross(e1, e2));
    h.position[0] = p.x;
    h.position[1] = p.y;
    h.position[2] = p.z;
    h.t = t;
    h.normal[0] = n.x;
    h.normal[1] = n.y;
    h.normal[2] = n.z;
    h.instance = instance;
    h.primitive = primitive;
    vec3 albedo = vec3(PT_DEFAULT_ALBEDO);
    float metallic = 0.0;
    float roughness = 1.0;
    vec3 emission = vec3(0.0);
    if (inst.material < scene.counts[FUSE_GPU_SCENE_MATERIALS]) {
        const PtMaterialRow row = PtMaterialsRef(scene.addresses[FUSE_GPU_SCENE_MATERIALS]).v[inst.material];
        albedo = max(row.baseColor.xyz, vec3(0.0));
        metallic = clamp(row.baseColor.w, 0.0, 1.0);
        roughness = clamp(row.roughnessEmissive.x, 0.0, 1.0);
        emission = max(row.roughnessEmissive.yzw, vec3(0.0)) * max(row.emissiveIntensity, 0.0);
    }
    h.albedo[0] = albedo.x;
    h.albedo[1] = albedo.y;
    h.albedo[2] = albedo.z;
    h.emission[0] = emission.x;
    h.emission[1] = emission.y;
    h.emission[2] = emission.z;
    h.emitter = pt_emitter_of(F, instance, primitive);
    h.roughness = roughness;
    h.metallic = metallic;
    h.flags = 0u;
    h.reserved = 0u;
    return h;
}

PtHit pt_miss_record() {
    PtHit h;
    h.position[0] = 0.0;
    h.position[1] = 0.0;
    h.position[2] = 0.0;
    h.t = -1.0;
    h.normal[0] = 0.0;
    h.normal[1] = 0.0;
    h.normal[2] = 0.0;
    h.instance = PT_INVALID;
    h.albedo[0] = 0.0;
    h.albedo[1] = 0.0;
    h.albedo[2] = 0.0;
    h.primitive = PT_INVALID;
    h.emission[0] = 0.0;
    h.emission[1] = 0.0;
    h.emission[2] = 0.0;
    h.emitter = PT_INVALID;
    h.roughness = 1.0;
    h.metallic = 0.0;
    h.flags = 0u;
    h.reserved = 0u;
    return h;
}

#endif
