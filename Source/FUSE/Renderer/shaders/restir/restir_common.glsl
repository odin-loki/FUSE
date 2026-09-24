// WP-7.2 ReSTIR DI and GI: records and the kernel, GLSL twin of restir_common.slang and a line-for-line port of
// include/fuse/renderer/restir/restir_kernel.hpp (the CPU kernel). The C++ mirror of the records is
// restir_types.hpp (fuse_rp_restir_layout checks names, order and offsets).
//
// #include after bindless.glsl, gpu_scene.glsl and lt_common.glsl. Every arithmetic result is held in a
// `precise` variable so no multiply-add is contracted: the passes keep the CPU kernel's IEEE operations and
// order and return the same bits.
#ifndef FUSE_RESTIR_COMMON_GLSL
#define FUSE_RESTIR_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : require

#define RS_TILE 8
#define RS_INVALID 0xFFFFFFFFu
#define RS_MAX_NEIGHBORS 8u
#define RS_PI 3.14159265358979323846
#define RS_SHADOW_SHORTEN 0.9999
#define RS_MASK_ALL 0x7Fu // rt::kRtMaskAll: bit 7 (kRtMaskDead) is never traced
#define RS_DEFAULT_ALBEDO 0.8

#define RS_FLAG_UNBIASED 1u
#define RS_FLAG_HISTORY 2u
#define RS_FLAG_VISIBILITY_REUSE 4u
#define RS_FLAG_MOTION 8u
#define RS_FLAG_GI_SHADE_VISIBILITY 16u
#define RS_MODE_TEMPORAL 0u
#define RS_MODE_SPATIAL 1u
#define RS_HIT_TRACED 1u
#define RS_HIT_HIT 2u

#define RS_STREAM_DI_SOBOL 0x100u
#define RS_STREAM_DI_INITIAL 0x101u
#define RS_STREAM_DI_REUSE 0x110u
#define RS_STREAM_GI_DIR 0x200u
#define RS_STREAM_GI_NEE 0x201u
#define RS_STREAM_GI_NEE_SOBOL 0x202u
#define RS_STREAM_GI_REUSE 0x210u

// RestirLight, 32 bytes.
struct RestirLight {
    float radiance[3];
    uint kind;
    float cosInner;
    float cosOuter;
    uint flags;
    uint reserved;
};

// RestirDiReservoir, 32 bytes.
struct RestirDiReservoir {
    uint light;
    float u1;
    float u2;
    float W;
    float M;
    float targetPdf;
    uint reserved[2];
};

// RestirGiReservoir, 48 bytes.
struct RestirGiReservoir {
    float position[3];
    float W;
    float normal[3];
    float M;
    float radiance[3];
    float targetPdf;
};

// RestirGiHitRecord, 32 bytes.
struct RestirGiHitRecord {
    float t;
    uint instance;
    uint primitive;
    uint flags;
    float direction[3];
    float reserved;
};

// RestirFrameConstants, 336 bytes.
struct RestirFrame {
    uint64_t surfPos[2];
    uint64_t surfNormal[2];
    uint64_t surfAlbedo[2];
    uint64_t diHistory[2];
    uint64_t giHistory[2];
    uint64_t diSignal;
    uint64_t giSignal;
    uint64_t depth;
    uint64_t motion;
    uint64_t lightTree;
    uint64_t lights;
    uint64_t tlas;
    uint64_t scene;
    float invViewProj[16];
    float cameraPosition[3];
    uint width;
    float cameraForward[3];
    uint height;
    float invWidth;
    float invHeight;
    uint frameIndex;
    uint seed;
    uint flags;
    uint lightCount;
    uint diCandidates;
    uint diNeighbors;
    uint giNeighbors;
    uint gbufferNormal;
    uint gbufferAlbedo;
    uint gbufferDepth;
    float diRadius;
    float giRadius;
    float diMCap;
    float giMCap;
    float normalThreshold;
    float depthThreshold;
    float normalBias;
    float viewBias;
    float farDistance;
    float giJacobianClamp;
    float giRayTMin;
    uint cullMask;
};

// RestirPush, 48 bytes.
struct RestirPush {
    uint64_t frame;
    uint64_t src;
    uint64_t dst;
    uint64_t aux;
    uint mode;
    uint iteration;
    uint reserved[2];
};

// Material::GPUMaterial (128 bytes): the base colour only.
struct RsMaterialHead {
    vec4 baseColor;
    vec4 rest[7];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RsFrameRef { RestirFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RsDiRef { RestirDiReservoir v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RsGiRef { RestirGiReservoir v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer RsVec4Ref { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer RsVec2Ref { vec2 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer RsFloatRef { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RsLightsRef { RestirLight v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer RsHitRef { RestirGiHitRecord v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RsWordsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RsMaterialsRef { RsMaterialHead v[]; };

layout(push_constant) uniform RestirPushBlock {
    RestirPush p;
} pc;

RestirFrame rs_frame() { return RsFrameRef(pc.p.frame).f; }
vec3 rs_load(float v[3]) { return vec3(v[0], v[1], v[2]); }

// --- vectors (restir_kernel.hpp rsDot / rsLength / ...) --------------------------------------------------
float rs_dot(vec3 a, vec3 b) {
    precise float x = a.x * b.x;
    precise float y = a.y * b.y;
    precise float z = a.z * b.z;
    precise float xy = x + y;
    precise float r = xy + z;
    return r;
}
float rs_length(vec3 a) {
    precise float r = sqrt(rs_dot(a, a));
    return r;
}
vec3 rs_normalize(vec3 a, vec3 fallback) {
    const float l = rs_length(a);
    if (!(l > 1e-20)) {
        return fallback;
    }
    precise vec3 r = vec3(a.x / l, a.y / l, a.z / l);
    return r;
}
vec3 rs_cross(vec3 a, vec3 b) {
    precise float x0 = a.y * b.z;
    precise float x1 = a.z * b.y;
    precise float y0 = a.z * b.x;
    precise float y1 = a.x * b.z;
    precise float z0 = a.x * b.y;
    precise float z1 = a.y * b.x;
    precise vec3 r = vec3(x0 - x1, y0 - y1, z0 - z1);
    return r;
}
float rs_lum(vec3 c) {
    precise float r = c.x * 0.2126;
    precise float g = c.y * 0.7152;
    precise float b = c.z * 0.0722;
    precise float rg = r + g;
    precise float s = rg + b;
    return s;
}
void rs_basis(vec3 n, out vec3 t, out vec3 b) {
    const float sign = n.z >= 0.0 ? 1.0 : -1.0;
    precise float a = -1.0 / (sign + n.z);
    precise float c = (n.x * n.y) * a;
    precise float txx = ((sign * n.x) * n.x) * a;
    precise vec3 tt = vec3(1.0 + txx, sign * c, -(sign * n.x));
    precise float byy = (n.y * n.y) * a;
    precise vec3 bb = vec3(c, sign + byy, -n.y);
    t = tt;
    b = bb;
}

// --- random numbers -------------------------------------------------------------------------------------------
uint rs_hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
uint rs_seed(uint px, uint py, uint frame, uint stream, uint seed) {
    return rs_hash(px + rs_hash(py + rs_hash(frame + rs_hash(stream + rs_hash(seed)))));
}
float rs_unit(uint x) { return float(x >> 8u) * (1.0 / 16777216.0); }
float rs_next(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    w = (w >> 22u) ^ w;
    return rs_unit(w);
}
uint rs_laine_karras(uint x, uint seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}
uint rs_owen(uint x, uint seed) { return bitfieldReverse(rs_laine_karras(bitfieldReverse(x), seed)); }
uint rs_hash_combine(uint seed, uint v) { return seed ^ (v + (seed << 6u) + (seed >> 2u)); }
void rs_sobol(uint n, uint seed, out float u, out float v) {
    const uint index = rs_owen(n, seed);
    const uint x = bitfieldReverse(index);
    uint bitv = 0x80000000u;
    uint y = 0u;
    for (uint bit = 0u; bit < 32u; ++bit) {
        if (((index >> bit) & 1u) != 0u) {
            y ^= bitv;
        }
        bitv ^= bitv >> 1u;
    }
    u = rs_unit(rs_owen(x, rs_hash_combine(seed, 0u)));
    v = rs_unit(rs_owen(y, rs_hash_combine(seed, 1u)));
}

// --- surfaces --------------------------------------------------------------------------------------------------
struct RsSurface {
    vec3 p;
    float depth;
    vec3 n;
};
bool rs_valid(RsSurface s) { return s.depth > 0.0; }

RsSurface rs_surface(RestirFrame F, uint slot, uint pixel) {
    const vec4 pd = RsVec4Ref(F.surfPos[slot]).v[pixel];
    const vec4 nn = RsVec4Ref(F.surfNormal[slot]).v[pixel];
    RsSurface s;
    s.p = pd.xyz;
    s.depth = pd.w;
    s.n = nn.xyz;
    return s;
}

vec3 rs_offset(RestirFrame F, vec3 p, vec3 n) {
    precise vec3 toCamera = p - rs_load(F.cameraPosition);
    precise float vb = F.viewBias * rs_length(toCamera);
    precise float bias = F.normalBias + vb;
    precise vec3 r = p + n * bias;
    return r;
}

vec3 rs_oct_decode(float ox, float oy) {
    const float ax = abs(ox);
    const float ay = abs(oy);
    precise float z0 = 1.0 - ax;
    precise vec3 n = vec3(ox, oy, z0 - ay);
    if (n.z < 0.0) {
        precise float x = (1.0 - ay) * (ox >= 0.0 ? 1.0 : -1.0);
        precise float y = (1.0 - ax) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return rs_normalize(n, vec3(0.0, 0.0, 1.0));
}

bool rs_prepare(RestirFrame F, uint px, uint py, float depth, float ox, float oy, out RsSurface s) {
    s.p = vec3(0.0);
    s.depth = 0.0;
    s.n = vec3(0.0);
    if (!(depth < 1.0) || !(depth >= 0.0)) {
        return false;
    }
    precise float nx = (((float(px) + 0.5) * F.invWidth) * 2.0) - 1.0;
    precise float ny = (((float(py) + 0.5) * F.invHeight) * 2.0) - 1.0;
    precise float x = ((F.invViewProj[0] * nx + F.invViewProj[4] * ny) + F.invViewProj[8] * depth) + F.invViewProj[12];
    precise float y = ((F.invViewProj[1] * nx + F.invViewProj[5] * ny) + F.invViewProj[9] * depth) + F.invViewProj[13];
    precise float z = ((F.invViewProj[2] * nx + F.invViewProj[6] * ny) + F.invViewProj[10] * depth) + F.invViewProj[14];
    precise float w = ((F.invViewProj[3] * nx + F.invViewProj[7] * ny) + F.invViewProj[11] * depth) + F.invViewProj[15];
    precise vec3 p = vec3(x / w, y / w, z / w);
    precise vec3 rel = p - rs_load(F.cameraPosition);
    const float lin = rs_dot(rel, rs_load(F.cameraForward));
    if (!(lin > 0.0)) {
        return false;
    }
    s.p = p;
    s.depth = lin;
    s.n = rs_oct_decode(ox, oy);
    return true;
}

// --- rays ------------------------------------------------------------------------------------------------------
bool rs_occluded(RestirFrame F, vec3 o, vec3 d, float tMax) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(F.tlas), gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          F.cullMask & RS_MASK_ALL, o, 0.0, d, tMax);
    while (rayQueryProceedEXT(q)) {
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}

struct RsHit {
    float t;
    vec3 normal;
    vec3 albedo;
    uint instance;
    uint primitive;
};

// World position of vertex k of triangle `primitive` of scene instance `instance` (restir_test mirror: the WP-6.0
// rtDecodePosition + the row-major 3x4 transform, fixed order).
vec3 rs_vertex(FuseGpuSceneHeaderRef scene, FuseGpuMesh mesh, FuseGpuTransform xf, uint primitive, uint k) {
    const uint vi = FuseGpuSceneIndicesRef(scene.indexAddress).v[mesh.firstIndex + 3u * primitive + k];
    RsWordsRef vpos = RsWordsRef(mesh.positions);
    const uint w0 = vpos.v[vi * 2u];
    const uint w1 = vpos.v[vi * 2u + 1u];
    precise float lx = mesh.quantOffset[0] + float(w0 & 0xFFFFu) * mesh.quantStep[0];
    precise float ly = mesh.quantOffset[1] + float(w0 >> 16u) * mesh.quantStep[1];
    precise float lz = mesh.quantOffset[2] + float(w1 & 0xFFFFu) * mesh.quantStep[2];
    precise float x = ((xf.rows[0].x * lx + xf.rows[0].y * ly) + xf.rows[0].z * lz) + xf.rows[0].w;
    precise float y = ((xf.rows[1].x * lx + xf.rows[1].y * ly) + xf.rows[1].z * lz) + xf.rows[1].w;
    precise float z = ((xf.rows[2].x * lx + xf.rows[2].y * ly) + xf.rows[2].z * lz) + xf.rows[2].w;
    return vec3(x, y, z);
}

bool rs_trace_hit(RestirFrame F, vec3 o, vec3 d, float tMin, float tMax, out RsHit hit) {
    hit.t = -1.0;
    hit.normal = vec3(0.0);
    hit.albedo = vec3(0.0);
    hit.instance = RS_INVALID;
    hit.primitive = RS_INVALID;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(F.tlas), gl_RayFlagsOpaqueEXT, F.cullMask & RS_MASK_ALL, o, tMin, d, tMax);
    while (rayQueryProceedEXT(q)) {
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
        return false;
    }
    hit.t = rayQueryGetIntersectionTEXT(q, true);
    hit.instance = uint(rayQueryGetIntersectionInstanceIdEXT(q, true));
    hit.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    FuseGpuSceneHeaderRef scene = FuseGpuSceneHeaderRef(F.scene);
    const FuseGpuInstance inst = FuseGpuInstancesRef(scene.addresses[FUSE_GPU_SCENE_INSTANCES]).v[hit.instance];
    const FuseGpuTransform xf = FuseGpuTransformsRef(scene.addresses[FUSE_GPU_SCENE_TRANSFORMS]).v[hit.instance];
    const FuseGpuMesh mesh = FuseGpuMeshesRef(scene.addresses[FUSE_GPU_SCENE_MESHES]).v[inst.mesh];
    const vec3 w0 = rs_vertex(scene, mesh, xf, hit.primitive, 0u);
    const vec3 w1 = rs_vertex(scene, mesh, xf, hit.primitive, 1u);
    const vec3 w2 = rs_vertex(scene, mesh, xf, hit.primitive, 2u);
    precise vec3 e1 = w1 - w0;
    precise vec3 e2 = w2 - w0;
    hit.normal = rs_cross(e1, e2);
    hit.albedo = vec3(RS_DEFAULT_ALBEDO);
    if (inst.material < scene.counts[FUSE_GPU_SCENE_MATERIALS]) {
        hit.albedo = RsMaterialsRef(scene.addresses[FUSE_GPU_SCENE_MATERIALS]).v[inst.material].baseColor.xyz;
    }
    return true;
}

// --- DI: light samples -----------------------------------------------------------------------------------------
struct RsLightEval {
    vec3 contrib;
    float phat;
    vec3 origin;
    vec3 dir;
    float tMax;
};

bool rs_area_kind(uint kind) { return kind == LT_KIND_TRIANGLE || kind == LT_KIND_RECT || kind == LT_KIND_DISK; }

float rs_source_pdf(uint kind, float pmf, float pdfArea) {
    precise float r = pmf * pdfArea;
    return rs_area_kind(kind) ? r : pmf;
}

float rs_spot(float cosA, float cosInner, float cosOuter) {
    if (cosInner > cosOuter) {
        precise float range = cosInner - cosOuter;
        precise float t = (cosA - cosOuter) / range;
        t = t < 0.0 ? 0.0 : t;
        t = t > 1.0 ? 1.0 : t;
        precise float tt = t * t;
        precise float k = 3.0 - 2.0 * t;
        precise float r = tt * k;
        return r;
    }
    return cosA >= cosOuter ? 1.0 : 0.0;
}

bool rs_eval_light(RestirFrame F, RsSurface s, uint light, float u1, float u2, out RsLightEval e) {
    e.contrib = vec3(0.0);
    e.phat = 0.0;
    e.origin = vec3(0.0);
    e.dir = vec3(0.0);
    e.tMax = 0.0;
    const LtHeader tree = LtHeaderRef(F.lightTree).h;
    if (light >= tree.emitterCount || light >= F.lightCount) {
        return false;
    }
    const LtEmitter em = LtEmittersRef(tree.emitters).v[light];
    const RestirLight L = RsLightsRef(F.lights).v[light];
    vec3 pos;
    float pdfArea;
    lt_sample_point(em, u1, u2, pos, pdfArea);
    const vec3 radiance = rs_load(L.radiance);
    e.origin = rs_offset(F, s.p, s.n);
    if (em.kind == LT_KIND_DIRECTIONAL) {
        const vec3 wi = vec3(-pos.x, -pos.y, -pos.z);
        const float cosX = rs_dot(s.n, wi);
        if (!(cosX > 0.0)) {
            return false;
        }
        precise vec3 c = radiance * cosX;
        e.contrib = c;
        e.dir = wi;
        e.tMax = F.farDistance;
    } else {
        precise vec3 d = pos - s.p;
        const float dist2 = rs_dot(d, d);
        if (!(dist2 > 1e-12)) {
            return false;
        }
        precise float dist = sqrt(dist2);
        precise vec3 wi = vec3(d.x / dist, d.y / dist, d.z / dist);
        const float cosX = rs_dot(s.n, wi);
        if (!(cosX > 0.0)) {
            return false;
        }
        precise float g = 0.0;
        if (rs_area_kind(em.kind)) {
            precise float cosL = -rs_dot(rs_load(em.normal), wi);
            if ((em.flags & LT_FLAG_TWO_SIDED) != 0u) {
                cosL = abs(cosL);
            }
            if (!(cosL > 0.0)) {
                return false;
            }
            g = (cosX * cosL) / dist2;
        } else {
            g = cosX / dist2;
            if (em.kind == LT_KIND_SPOT) {
                precise float cosA = -rs_dot(rs_load(em.normal), wi);
                g = g * rs_spot(cosA, L.cosInner, L.cosOuter);
            }
        }
        precise vec3 c = radiance * g;
        e.contrib = c;
        precise vec3 toLight = pos - e.origin;
        const float dl = rs_length(toLight);
        if (dl > 1e-6) {
            precise vec3 dir = vec3(toLight.x / dl, toLight.y / dl, toLight.z / dl);
            precise float tm = dl * RS_SHADOW_SHORTEN;
            e.dir = dir;
            e.tMax = tm;
        } else {
            e.dir = s.n;
            e.tMax = 0.0;
        }
    }
    e.phat = rs_lum(e.contrib);
    return e.phat > 0.0;
}

float rs_di_target(RestirFrame F, RsSurface s, RestirDiReservoir r, bool visibility) {
    RsLightEval e;
    if (!rs_valid(s) || r.light == RS_INVALID || !rs_eval_light(F, s, r.light, r.u1, r.u2, e)) {
        return 0.0;
    }
    if (visibility && e.tMax > 0.0 && rs_occluded(F, e.origin, e.dir, e.tMax)) {
        return 0.0;
    }
    return e.phat;
}

RestirDiReservoir rs_di_empty() {
    RestirDiReservoir r;
    r.light = RS_INVALID;
    r.u1 = 0.0;
    r.u2 = 0.0;
    r.W = 0.0;
    r.M = 0.0;
    r.targetPdf = 0.0;
    r.reserved[0] = 0u;
    r.reserved[1] = 0u;
    return r;
}

RestirDiReservoir rs_di_initial(RestirFrame F, uint px, uint py) {
    const uint pixel = py * F.width + px;
    RestirDiReservoir r = rs_di_empty();
    const RsSurface s = rs_surface(F, 0u, pixel);
    if (!rs_valid(s)) {
        return r;
    }
    r.M = 1.0;
    const uint candidates = F.diCandidates;
    if (candidates == 0u) {
        return r;
    }
    const uint sobol = rs_seed(px, py, 0u, RS_STREAM_DI_SOBOL, F.seed);
    uint rng = rs_seed(px, py, F.frameIndex, RS_STREAM_DI_INITIAL, F.seed);
    precise float wSum = 0.0;
    float selPhat = 0.0;
    RsLightEval sel;
    sel.tMax = 0.0;
    for (uint j = 0u; j < candidates; ++j) {
        const float u0 = rs_next(rng);
        float u1;
        float u2;
        rs_sobol(F.frameIndex * candidates + j, sobol, u1, u2);
        const LtSampleResult ls = lt_sample(F.lightTree, s.p, s.n, u0, u1, u2);
        if (ls.light == LT_INVALID) {
            continue;
        }
        const float src = rs_source_pdf(ls.kind, ls.pmf, ls.pdfArea);
        if (!(src > 0.0)) {
            continue;
        }
        RsLightEval e;
        if (!rs_eval_light(F, s, ls.light, u1, u2, e)) {
            continue;
        }
        precise float w = e.phat / src;
        wSum = wSum + w;
        const float u = rs_next(rng);
        precise float uw = u * wSum;
        if (uw < w) {
            r.light = ls.light;
            r.u1 = u1;
            r.u2 = u2;
            selPhat = e.phat;
            sel = e;
        }
    }
    if (r.light != RS_INVALID) {
        precise float mean = wSum / float(candidates);
        precise float W = mean / selPhat;
        r.W = W;
        r.targetPdf = selPhat;
        if ((F.flags & RS_FLAG_VISIBILITY_REUSE) != 0u && sel.tMax > 0.0 && rs_occluded(F, sel.origin, sel.dir, sel.tMax)) {
            r.W = 0.0;
        }
    }
    return r;
}

// --- reuse helpers ---------------------------------------------------------------------------------------------
bool rs_similar(RestirFrame F, RsSurface s, RsSurface q) {
    if (!rs_valid(q)) {
        return false;
    }
    if (!(rs_dot(s.n, q.n) >= F.normalThreshold)) {
        return false;
    }
    precise float dz = abs(q.depth - s.depth);
    precise float lim = F.depthThreshold * s.depth;
    return dz <= lim;
}

bool rs_reproject(RestirFrame F, uint px, uint py, out uint q) {
    q = 0u;
    if ((F.flags & RS_FLAG_HISTORY) == 0u) {
        return false;
    }
    vec2 m = vec2(0.0);
    if ((F.flags & RS_FLAG_MOTION) != 0u) {
        m = RsVec2Ref(F.motion).v[py * F.width + px];
    }
    precise float ux = ((float(px) + 0.5) * F.invWidth) - m.x;
    precise float uy = ((float(py) + 0.5) * F.invHeight) - m.y;
    if (!(ux >= 0.0 && ux < 1.0 && uy >= 0.0 && uy < 1.0)) {
        return false;
    }
    precise float fx = ux * float(F.width);
    precise float fy = uy * float(F.height);
    uint qx = uint(fx);
    uint qy = uint(fy);
    qx = qx < F.width ? qx : F.width - 1u;
    qy = qy < F.height ? qy : F.height - 1u;
    q = qy * F.width + qx;
    return true;
}

bool rs_neighbor(RestirFrame F, uint px, uint py, float radius, inout uint rng, out uint q) {
    q = 0u;
    const float a = rs_next(rng);
    const float b = rs_next(rng);
    float ox;
    float oy;
    lt_concentric(a, b, ox, oy);
    precise float fx = ox * radius;
    precise float fy = oy * radius;
    const int dx = int(fx);
    const int dy = int(fy);
    if (dx == 0 && dy == 0) {
        return false;
    }
    const int qx = int(px) + dx;
    const int qy = int(py) + dy;
    if (qx < 0 || qy < 0 || qx >= int(F.width) || qy >= int(F.height)) {
        return false;
    }
    q = uint(qy) * F.width + uint(qx);
    return true;
}

// --- DI reuse --------------------------------------------------------------------------------------------------
RestirDiReservoir rs_di_reuse(RestirFrame F, uint px, uint py, uint mode, uint iteration) {
    const uint pixel = py * F.width + px;
    const RsSurface s = rs_surface(F, 0u, pixel);
    RsDiRef src = RsDiRef(pc.p.src);
    const RestirDiReservoir canon = src.v[pixel];
    if (!rs_valid(s)) {
        return canon;
    }
    RestirDiReservoir inR[1 + RS_MAX_NEIGHBORS];
    RsSurface sf[1 + RS_MAX_NEIGHBORS];
    uint count = 1u;
    inR[0] = canon;
    sf[0] = s;
    uint rng = rs_seed(px, py, F.frameIndex, RS_STREAM_DI_REUSE + mode * 16u + iteration, F.seed);
    if (mode == RS_MODE_TEMPORAL) {
        uint q;
        if (rs_reproject(F, px, py, q)) {
            const RsSurface sp = rs_surface(F, 1u, q);
            if (rs_similar(F, s, sp)) {
                RestirDiReservoir h = RsDiRef(F.diHistory[1]).v[q];
                precise float cap = F.diMCap * canon.M;
                h.M = h.M < cap ? h.M : cap;
                inR[count] = h;
                sf[count] = sp;
                ++count;
            }
        }
    } else {
        const uint neighbors = F.diNeighbors < RS_MAX_NEIGHBORS ? F.diNeighbors : RS_MAX_NEIGHBORS;
        for (uint k = 0u; k < neighbors; ++k) {
            uint q;
            if (!rs_neighbor(F, px, py, F.diRadius, rng, q)) {
                continue;
            }
            const RsSurface sq = rs_surface(F, 0u, q);
            if (!rs_similar(F, s, sq)) {
                continue;
            }
            inR[count] = src.v[q];
            sf[count] = sq;
            ++count;
        }
    }
    const bool unbiased = (F.flags & RS_FLAG_UNBIASED) != 0u;
    precise float mSum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        mSum = mSum + inR[i].M;
    }
    RestirDiReservoir outR = rs_di_empty();
    outR.M = mSum;
    precise float wSum = 0.0;
    float selPhat = 0.0;
    for (uint i = 0u; i < count; ++i) {
        const RestirDiReservoir ri = inR[i];
        if (!(ri.W > 0.0) || ri.light == RS_INVALID) {
            continue;
        }
        const float pcv = rs_di_target(F, s, ri, unbiased);
        if (!(pcv > 0.0)) {
            continue;
        }
        precise float w = 0.0;
        if (unbiased) {
            precise float num = 0.0;
            precise float den = 0.0;
            for (uint j = 0u; j < count; ++j) {
                const float pj = j == 0u ? pcv : rs_di_target(F, sf[j], ri, true);
                precise float mj = inR[j].M * pj;
                den = den + mj;
                if (j == i) {
                    num = mj;
                }
            }
            precise float m = num / den;
            w = (m * pcv) * ri.W;
        } else {
            w = (pcv * ri.W) * ri.M;
        }
        if (!(w > 0.0)) {
            continue;
        }
        wSum = wSum + w;
        const float u = rs_next(rng);
        precise float uw = u * wSum;
        if (uw < w) {
            outR.light = ri.light;
            outR.u1 = ri.u1;
            outR.u2 = ri.u2;
            selPhat = pcv;
        }
    }
    if (outR.light != RS_INVALID) {
        precise float wu = wSum / selPhat;
        precise float d = selPhat * mSum;
        precise float wb = wSum / d;
        outR.W = unbiased ? wu : wb;
        outR.targetPdf = selPhat;
    }
    return outR;
}

// --- GI --------------------------------------------------------------------------------------------------------
struct RsGiEval {
    float cosV;
    float phat;
    float area;
    vec3 origin;
    vec3 dir;
    float tMax;
};

RestirGiReservoir rs_gi_empty() {
    RestirGiReservoir r;
    for (uint k = 0u; k < 3u; ++k) {
        r.position[k] = 0.0;
        r.normal[k] = 0.0;
        r.radiance[k] = 0.0;
    }
    r.W = 0.0;
    r.M = 0.0;
    r.targetPdf = 0.0;
    return r;
}

bool rs_gi_evaluate(RestirFrame F, RsSurface s, RestirGiReservoir r, out RsGiEval e) {
    e.cosV = 0.0;
    e.phat = 0.0;
    e.area = 0.0;
    e.origin = vec3(0.0);
    e.dir = vec3(0.0);
    e.tMax = 0.0;
    if (!rs_valid(s)) {
        return false;
    }
    const vec3 xs = rs_load(r.position);
    const vec3 ns = rs_load(r.normal);
    precise vec3 d = xs - s.p;
    const float dist2 = rs_dot(d, d);
    if (!(dist2 > 1e-12)) {
        return false;
    }
    precise float dist = sqrt(dist2);
    precise vec3 w = vec3(d.x / dist, d.y / dist, d.z / dist);
    const float cosV = rs_dot(s.n, w);
    if (!(cosV > 0.0)) {
        return false;
    }
    precise float cosS = -rs_dot(ns, w);
    if (!(cosS > 0.0)) {
        return false;
    }
    precise float phat = rs_lum(rs_load(r.radiance)) * cosV;
    if (!(phat > 0.0)) {
        return false;
    }
    e.cosV = cosV;
    e.phat = phat;
    precise float area = cosS / dist2;
    e.area = area;
    e.origin = rs_offset(F, s.p, s.n);
    precise vec3 target = xs + ns * F.normalBias;
    precise vec3 to = target - e.origin;
    const float dl = rs_length(to);
    if (dl > 1e-6) {
        precise vec3 dir = vec3(to.x / dl, to.y / dl, to.z / dl);
        precise float tm = dl * RS_SHADOW_SHORTEN;
        e.dir = dir;
        e.tMax = tm;
    } else {
        e.dir = s.n;
        e.tMax = 0.0;
    }
    return true;
}

vec3 rs_cosine_direction(vec3 n, float u, float v) {
    float dx;
    float dy;
    lt_concentric(u, v, dx, dy);
    precise float xx = dx * dx;
    precise float yy = dy * dy;
    precise float r = 1.0 - xx;
    precise float rr = r - yy;
    const float dz = lt_safe_sqrt(rr);
    vec3 t;
    vec3 b;
    rs_basis(n, t, b);
    precise vec3 tx = t * dx;
    precise vec3 by = b * dy;
    precise vec3 nz = n * dz;
    precise vec3 ab = tx + by;
    precise vec3 d = ab + nz;
    return rs_normalize(d, n);
}

RestirGiReservoir rs_gi_initial(RestirFrame F, uint px, uint py, uint64_t dump) {
    const uint pixel = py * F.width + px;
    RestirGiReservoir r = rs_gi_empty();
    RestirGiHitRecord rec;
    rec.t = -1.0;
    rec.instance = RS_INVALID;
    rec.primitive = RS_INVALID;
    rec.flags = 0u;
    rec.direction[0] = 0.0;
    rec.direction[1] = 0.0;
    rec.direction[2] = 0.0;
    rec.reserved = 0.0;
    const RsSurface s = rs_surface(F, 0u, pixel);
    if (!rs_valid(s)) {
        if (dump != 0ul) {
            RsHitRef(dump).v[pixel] = rec;
        }
        return r;
    }
    r.M = 1.0;
    float u;
    float v;
    rs_sobol(F.frameIndex, rs_seed(px, py, 0u, RS_STREAM_GI_DIR, F.seed), u, v);
    const vec3 dir = rs_cosine_direction(s.n, u, v);
    const float cosV = rs_dot(s.n, dir);
    rec.direction[0] = dir.x;
    rec.direction[1] = dir.y;
    rec.direction[2] = dir.z;
    if (!(cosV > 0.0)) {
        if (dump != 0ul) {
            RsHitRef(dump).v[pixel] = rec;
        }
        return r;
    }
    RsHit hit;
    const bool found = rs_trace_hit(F, s.p, dir, F.giRayTMin, F.farDistance, hit);
    rec.flags = RS_HIT_TRACED | (found ? RS_HIT_HIT : 0u);
    rec.t = found ? hit.t : -1.0;
    rec.instance = found ? hit.instance : RS_INVALID;
    rec.primitive = found ? hit.primitive : RS_INVALID;
    if (dump != 0ul) {
        RsHitRef(dump).v[pixel] = rec;
    }
    if (!found) {
        return r;
    }
    precise vec3 xs = s.p + dir * hit.t;
    vec3 ns = rs_normalize(hit.normal, -dir);
    if (rs_dot(ns, dir) > 0.0) {
        ns = -ns;
    }
    RsSurface hs;
    hs.p = xs;
    hs.depth = 1.0;
    hs.n = ns;
    uint rng = rs_seed(px, py, F.frameIndex, RS_STREAM_GI_NEE, F.seed);
    const float u0 = rs_next(rng);
    float u1;
    float u2;
    rs_sobol(F.frameIndex, rs_seed(px, py, 0u, RS_STREAM_GI_NEE_SOBOL, F.seed), u1, u2);
    const LtSampleResult ls = lt_sample(F.lightTree, xs, ns, u0, u1, u2);
    precise vec3 lo = vec3(0.0);
    if (ls.light != LT_INVALID) {
        const float src = rs_source_pdf(ls.kind, ls.pmf, ls.pdfArea);
        RsLightEval e;
        if (src > 0.0 && rs_eval_light(F, hs, ls.light, u1, u2, e)) {
            if (!(e.tMax > 0.0) || !rs_occluded(F, e.origin, e.dir, e.tMax)) {
                precise float lx = ((hit.albedo.x * e.contrib.x) / RS_PI) / src;
                precise float ly = ((hit.albedo.y * e.contrib.y) / RS_PI) / src;
                precise float lz = ((hit.albedo.z * e.contrib.z) / RS_PI) / src;
                lo = vec3(lx, ly, lz);
            }
        }
    }
    r.position[0] = xs.x;
    r.position[1] = xs.y;
    r.position[2] = xs.z;
    r.normal[0] = ns.x;
    r.normal[1] = ns.y;
    r.normal[2] = ns.z;
    r.radiance[0] = lo.x;
    r.radiance[1] = lo.y;
    r.radiance[2] = lo.z;
    precise float phat = rs_lum(lo) * cosV;
    if (phat > 0.0) {
        precise float W = RS_PI / cosV;
        r.W = W;
        r.targetPdf = phat;
    }
    return r;
}

float rs_gi_area_target(RestirFrame F, RsSurface s, RestirGiReservoir r) {
    RsGiEval e;
    if (!rs_gi_evaluate(F, s, r, e)) {
        return 0.0;
    }
    if (e.tMax > 0.0 && rs_occluded(F, e.origin, e.dir, e.tMax)) {
        return 0.0;
    }
    precise float a = e.phat * e.area;
    return a;
}

RestirGiReservoir rs_gi_reuse(RestirFrame F, uint px, uint py, uint mode, uint iteration) {
    const uint pixel = py * F.width + px;
    const RsSurface s = rs_surface(F, 0u, pixel);
    RsGiRef src = RsGiRef(pc.p.src);
    const RestirGiReservoir canon = src.v[pixel];
    if (!rs_valid(s)) {
        return canon;
    }
    RestirGiReservoir inR[1 + RS_MAX_NEIGHBORS];
    RsSurface sf[1 + RS_MAX_NEIGHBORS];
    uint count = 1u;
    inR[0] = canon;
    sf[0] = s;
    uint rng = rs_seed(px, py, F.frameIndex, RS_STREAM_GI_REUSE + mode * 16u + iteration, F.seed);
    if (mode == RS_MODE_TEMPORAL) {
        uint q;
        if (rs_reproject(F, px, py, q)) {
            const RsSurface sp = rs_surface(F, 1u, q);
            if (rs_similar(F, s, sp)) {
                RestirGiReservoir h = RsGiRef(F.giHistory[1]).v[q];
                precise float cap = F.giMCap * canon.M;
                h.M = h.M < cap ? h.M : cap;
                inR[count] = h;
                sf[count] = sp;
                ++count;
            }
        }
    } else {
        const uint neighbors = F.giNeighbors < RS_MAX_NEIGHBORS ? F.giNeighbors : RS_MAX_NEIGHBORS;
        for (uint k = 0u; k < neighbors; ++k) {
            uint q;
            if (!rs_neighbor(F, px, py, F.giRadius, rng, q)) {
                continue;
            }
            const RsSurface sq = rs_surface(F, 0u, q);
            if (!rs_similar(F, s, sq)) {
                continue;
            }
            inR[count] = src.v[q];
            sf[count] = sq;
            ++count;
        }
    }
    const bool unbiased = (F.flags & RS_FLAG_UNBIASED) != 0u;
    precise float mSum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        mSum = mSum + inR[i].M;
    }
    RestirGiReservoir outR = rs_gi_empty();
    outR.M = mSum;
    precise float wSum = 0.0;
    float selPhat = 0.0;
    bool selected = false;
    for (uint i = 0u; i < count; ++i) {
        const RestirGiReservoir ri = inR[i];
        if (!(ri.W > 0.0)) {
            continue;
        }
        RsGiEval ec;
        if (!rs_gi_evaluate(F, s, ri, ec)) {
            continue;
        }
        if (unbiased && ec.tMax > 0.0 && rs_occluded(F, ec.origin, ec.dir, ec.tMax)) {
            continue;
        }
        const float pcv = ec.phat;
        precise float jacobian = 1.0;
        if (i != 0u) {
            RsGiEval ei;
            if (!rs_gi_evaluate(F, sf[i], ri, ei)) {
                continue;
            }
            jacobian = ec.area / ei.area;
            precise float jc = jacobian * F.giJacobianClamp;
            if (!unbiased && F.giJacobianClamp > 0.0 && (jacobian > F.giJacobianClamp || jc < 1.0)) {
                continue;
            }
        }
        precise float w = 0.0;
        if (unbiased) {
            precise float num = 0.0;
            precise float den = 0.0;
            for (uint j = 0u; j < count; ++j) {
                precise float pa0 = pcv * ec.area;
                const float pj = j == 0u ? pa0 : rs_gi_area_target(F, sf[j], ri);
                precise float mj = inR[j].M * pj;
                den = den + mj;
                if (j == i) {
                    num = mj;
                }
            }
            precise float m = num / den;
            w = ((m * pcv) * ri.W) * jacobian;
        } else {
            w = ((pcv * ri.W) * jacobian) * ri.M;
        }
        if (!(w > 0.0)) {
            continue;
        }
        wSum = wSum + w;
        const float u = rs_next(rng);
        precise float uw = u * wSum;
        if (uw < w) {
            for (uint k = 0u; k < 3u; ++k) {
                outR.position[k] = ri.position[k];
                outR.normal[k] = ri.normal[k];
                outR.radiance[k] = ri.radiance[k];
            }
            selPhat = pcv;
            selected = true;
        }
    }
    if (selected) {
        precise float wu = wSum / selPhat;
        precise float d = selPhat * mSum;
        precise float wb = wSum / d;
        outR.W = unbiased ? wu : wb;
        outR.targetPdf = selPhat;
    }
    return outR;
}

// --- shade -----------------------------------------------------------------------------------------------------
void rs_shade(RestirFrame F, uint px, uint py, bool hasDi, RestirDiReservoir di, bool hasGi, RestirGiReservoir gi,
              out vec4 diOut, out vec4 giOut, out float depth) {
    const uint pixel = py * F.width + px;
    diOut = vec4(0.0);
    giOut = vec4(0.0);
    const RsSurface s = rs_surface(F, 0u, pixel);
    depth = s.depth;
    if (!rs_valid(s)) {
        depth = 0.0;
        return;
    }
    if (hasDi && di.light != RS_INVALID && di.W > 0.0) {
        RsLightEval e;
        if (rs_eval_light(F, s, di.light, di.u1, di.u2, e)) {
            if (!(e.tMax > 0.0) || !rs_occluded(F, e.origin, e.dir, e.tMax)) {
                precise float k = di.W / RS_PI;
                precise vec3 c = e.contrib * k;
                diOut = vec4(c, 0.0);
            }
        }
    }
    if (hasGi && gi.W > 0.0) {
        RsGiEval e;
        if (rs_gi_evaluate(F, s, gi, e)) {
            const bool trace = (F.flags & RS_FLAG_GI_SHADE_VISIBILITY) != 0u && e.tMax > 0.0;
            if (!trace || !rs_occluded(F, e.origin, e.dir, e.tMax)) {
                precise float cw = e.cosV * gi.W;
                precise float k = cw / RS_PI;
                precise vec3 c = rs_load(gi.radiance) * k;
                giOut = vec4(c, 0.0);
            }
        }
    }
}

#endif
