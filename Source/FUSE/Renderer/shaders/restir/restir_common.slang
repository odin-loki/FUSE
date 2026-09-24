// WP-7.2 ReSTIR DI and GI: Slang twin of restir_common.glsl (see there) and a line-for-line port of
// include/fuse/renderer/restir/restir_kernel.hpp. #include after gpu_scene.slang and lt_common.slang. Compile with
// -fp-mode precise (Slang does not honour `precise` per variable), so no multiply-add is contracted.
#pragma once

static const uint kRsTile = 8u;
static const uint kRsInvalid = 0xFFFFFFFFu;
static const uint kRsMaxNeighbors = 8u;
static const float kRsPi = 3.14159265358979323846;
static const float kRsShadowShorten = 0.9999;
static const uint kRsMaskAll = 0x7Fu; // rt::kRtMaskAll: bit 7 (kRtMaskDead) is never traced
static const float kRsDefaultAlbedo = 0.8;

static const uint kRsFlagUnbiased = 1u;
static const uint kRsFlagHistory = 2u;
static const uint kRsFlagVisibilityReuse = 4u;
static const uint kRsFlagMotion = 8u;
static const uint kRsFlagGiShadeVisibility = 16u;
static const uint kRsModeTemporal = 0u;
static const uint kRsModeSpatial = 1u;
static const uint kRsHitTraced = 1u;
static const uint kRsHitHit = 2u;

static const uint kRsStreamDiSobol = 0x100u;
static const uint kRsStreamDiInitial = 0x101u;
static const uint kRsStreamDiReuse = 0x110u;
static const uint kRsStreamGiDir = 0x200u;
static const uint kRsStreamGiNee = 0x201u;
static const uint kRsStreamGiNeeSobol = 0x202u;
static const uint kRsStreamGiReuse = 0x210u;

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
    float4 baseColor;
    float4 rest[7];
};

[[vk::push_constant]] ConstantBuffer<RestirPush> pc;
[[vk::binding(1, 0)]] Texture2D<float4> fuse_restir_textures[];

RestirFrame rsFrame() { return ((RestirFrame*)pc.frame)[0]; }
float3 rsLoad(float v[3]) { return float3(v[0], v[1], v[2]); }
float4 rsFetch(uint handle, int2 p) { return fuse_restir_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

// --- vectors ---------------------------------------------------------------------------------------------------
float rsDot(float3 a, float3 b) {
    const float x = a.x * b.x;
    const float y = a.y * b.y;
    const float z = a.z * b.z;
    const float xy = x + y;
    return xy + z;
}
float rsLength(float3 a) { return sqrt(rsDot(a, a)); }
float3 rsNormalize(float3 a, float3 fallback) {
    const float l = rsLength(a);
    if (!(l > 1e-20)) {
        return fallback;
    }
    return float3(a.x / l, a.y / l, a.z / l);
}
float3 rsCross(float3 a, float3 b) {
    const float x0 = a.y * b.z;
    const float x1 = a.z * b.y;
    const float y0 = a.z * b.x;
    const float y1 = a.x * b.z;
    const float z0 = a.x * b.y;
    const float z1 = a.y * b.x;
    return float3(x0 - x1, y0 - y1, z0 - z1);
}
float rsLum(float3 c) {
    const float r = c.x * 0.2126;
    const float g = c.y * 0.7152;
    const float b = c.z * 0.0722;
    const float rg = r + g;
    return rg + b;
}
void rsBasis(float3 n, out float3 t, out float3 b) {
    const float sign = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sign + n.z);
    const float c = (n.x * n.y) * a;
    const float txx = ((sign * n.x) * n.x) * a;
    t = float3(1.0 + txx, sign * c, -(sign * n.x));
    const float byy = (n.y * n.y) * a;
    b = float3(c, sign + byy, -n.y);
}

// --- random numbers -------------------------------------------------------------------------------------------
uint rsHash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
uint rsSeed(uint px, uint py, uint frame, uint stream, uint seed) {
    return rsHash(px + rsHash(py + rsHash(frame + rsHash(stream + rsHash(seed)))));
}
float rsUnit(uint x) { return float(x >> 8u) * (1.0 / 16777216.0); }
float rsNext(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    w = (w >> 22u) ^ w;
    return rsUnit(w);
}
uint rsLaineKarras(uint x, uint seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}
uint rsOwen(uint x, uint seed) { return reversebits(rsLaineKarras(reversebits(x), seed)); }
uint rsHashCombine(uint seed, uint v) { return seed ^ (v + (seed << 6u) + (seed >> 2u)); }
void rsSobol(uint n, uint seed, out float u, out float v) {
    const uint index = rsOwen(n, seed);
    const uint x = reversebits(index);
    uint bitv = 0x80000000u;
    uint y = 0u;
    for (uint bit = 0u; bit < 32u; ++bit) {
        if (((index >> bit) & 1u) != 0u) {
            y ^= bitv;
        }
        bitv ^= bitv >> 1u;
    }
    u = rsUnit(rsOwen(x, rsHashCombine(seed, 0u)));
    v = rsUnit(rsOwen(y, rsHashCombine(seed, 1u)));
}

// --- surfaces --------------------------------------------------------------------------------------------------
struct RsSurface {
    float3 p;
    float depth;
    float3 n;
};
bool rsValid(RsSurface s) { return s.depth > 0.0; }

RsSurface rsSurface(RestirFrame F, uint slot, uint pixel) {
    const float4 pd = ((float4*)F.surfPos[slot])[pixel];
    const float4 nn = ((float4*)F.surfNormal[slot])[pixel];
    RsSurface s;
    s.p = pd.xyz;
    s.depth = pd.w;
    s.n = nn.xyz;
    return s;
}

float3 rsOffset(RestirFrame F, float3 p, float3 n) {
    const float3 toCamera = p - rsLoad(F.cameraPosition);
    const float vb = F.viewBias * rsLength(toCamera);
    const float bias = F.normalBias + vb;
    return p + n * bias;
}

float3 rsOctDecode(float ox, float oy) {
    const float ax = abs(ox);
    const float ay = abs(oy);
    const float z0 = 1.0 - ax;
    float3 n = float3(ox, oy, z0 - ay);
    if (n.z < 0.0) {
        const float x = (1.0 - ay) * (ox >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - ax) * (oy >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }
    return rsNormalize(n, float3(0.0, 0.0, 1.0));
}

bool rsPrepare(RestirFrame F, uint px, uint py, float depth, float ox, float oy, out RsSurface s) {
    s.p = float3(0.0, 0.0, 0.0);
    s.depth = 0.0;
    s.n = float3(0.0, 0.0, 0.0);
    if (!(depth < 1.0) || !(depth >= 0.0)) {
        return false;
    }
    const float nx = (((float(px) + 0.5) * F.invWidth) * 2.0) - 1.0;
    const float ny = (((float(py) + 0.5) * F.invHeight) * 2.0) - 1.0;
    const float x = ((F.invViewProj[0] * nx + F.invViewProj[4] * ny) + F.invViewProj[8] * depth) + F.invViewProj[12];
    const float y = ((F.invViewProj[1] * nx + F.invViewProj[5] * ny) + F.invViewProj[9] * depth) + F.invViewProj[13];
    const float z = ((F.invViewProj[2] * nx + F.invViewProj[6] * ny) + F.invViewProj[10] * depth) + F.invViewProj[14];
    const float w = ((F.invViewProj[3] * nx + F.invViewProj[7] * ny) + F.invViewProj[11] * depth) + F.invViewProj[15];
    const float3 p = float3(x / w, y / w, z / w);
    const float3 rel = p - rsLoad(F.cameraPosition);
    const float lin = rsDot(rel, rsLoad(F.cameraForward));
    if (!(lin > 0.0)) {
        return false;
    }
    s.p = p;
    s.depth = lin;
    s.n = rsOctDecode(ox, oy);
    return true;
}

// --- rays ------------------------------------------------------------------------------------------------------
bool rsOccluded(RestirFrame F, float3 o, float3 d, float tMax) {
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = 0.0;
    desc.Direction = d;
    desc.TMax = tMax;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(F.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(as, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, F.cullMask & kRsMaskAll, desc);
    while (q.Proceed()) {
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

struct RsHit {
    float t;
    float3 normal;
    float3 albedo;
    uint instance;
    uint primitive;
};

float3 rsVertex(GpuSceneHeader* scene, GpuMesh mesh, GpuTransform xf, uint primitive, uint k) {
    const uint vi = ((uint*)scene.indexAddress)[mesh.firstIndex + 3u * primitive + k];
    uint* vpos = (uint*)mesh.positions;
    const uint w0 = vpos[vi * 2u];
    const uint w1 = vpos[vi * 2u + 1u];
    const float lx = mesh.quantOffset[0] + float(w0 & 0xFFFFu) * mesh.quantStep[0];
    const float ly = mesh.quantOffset[1] + float(w0 >> 16u) * mesh.quantStep[1];
    const float lz = mesh.quantOffset[2] + float(w1 & 0xFFFFu) * mesh.quantStep[2];
    const float x = ((xf.rows[0].x * lx + xf.rows[0].y * ly) + xf.rows[0].z * lz) + xf.rows[0].w;
    const float y = ((xf.rows[1].x * lx + xf.rows[1].y * ly) + xf.rows[1].z * lz) + xf.rows[1].w;
    const float z = ((xf.rows[2].x * lx + xf.rows[2].y * ly) + xf.rows[2].z * lz) + xf.rows[2].w;
    return float3(x, y, z);
}

bool rsTraceHit(RestirFrame F, float3 o, float3 d, float tMin, float tMax, out RsHit hit) {
    hit.t = -1.0;
    hit.normal = float3(0.0, 0.0, 0.0);
    hit.albedo = float3(0.0, 0.0, 0.0);
    hit.instance = kRsInvalid;
    hit.primitive = kRsInvalid;
    RayDesc desc;
    desc.Origin = o;
    desc.TMin = tMin;
    desc.Direction = d;
    desc.TMax = tMax;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(F.tlas);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(as, RAY_FLAG_FORCE_OPAQUE, F.cullMask & kRsMaskAll, desc);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        return false;
    }
    hit.t = q.CommittedRayT();
    hit.instance = q.CommittedInstanceIndex();
    hit.primitive = q.CommittedPrimitiveIndex();
    GpuSceneHeader* scene = (GpuSceneHeader*)F.scene;
    const GpuInstance inst = ((GpuInstance*)scene.addresses[kFuseGpuSceneInstances])[hit.instance];
    const GpuTransform xf = ((GpuTransform*)scene.addresses[kFuseGpuSceneTransforms])[hit.instance];
    const GpuMesh mesh = ((GpuMesh*)scene.addresses[kFuseGpuSceneMeshes])[inst.mesh];
    const float3 w0 = rsVertex(scene, mesh, xf, hit.primitive, 0u);
    const float3 w1 = rsVertex(scene, mesh, xf, hit.primitive, 1u);
    const float3 w2 = rsVertex(scene, mesh, xf, hit.primitive, 2u);
    const float3 e1 = w1 - w0;
    const float3 e2 = w2 - w0;
    hit.normal = rsCross(e1, e2);
    hit.albedo = float3(kRsDefaultAlbedo, kRsDefaultAlbedo, kRsDefaultAlbedo);
    if (inst.material < scene.counts[kFuseGpuSceneMaterials]) {
        hit.albedo = ((RsMaterialHead*)scene.addresses[kFuseGpuSceneMaterials])[inst.material].baseColor.xyz;
    }
    return true;
}

// --- DI: light samples -----------------------------------------------------------------------------------------
struct RsLightEval {
    float3 contrib;
    float phat;
    float3 origin;
    float3 dir;
    float tMax;
};

bool rsAreaKind(uint kind) { return kind == kLtKindTriangle || kind == kLtKindRect || kind == kLtKindDisk; }

float rsSourcePdf(uint kind, float pmf, float pdfArea) {
    const float r = pmf * pdfArea;
    return rsAreaKind(kind) ? r : pmf;
}

float rsSpot(float cosA, float cosInner, float cosOuter) {
    if (cosInner > cosOuter) {
        const float range = cosInner - cosOuter;
        float t = (cosA - cosOuter) / range;
        t = t < 0.0 ? 0.0 : t;
        t = t > 1.0 ? 1.0 : t;
        const float tt = t * t;
        const float k = 3.0 - 2.0 * t;
        return tt * k;
    }
    return cosA >= cosOuter ? 1.0 : 0.0;
}

bool rsEvalLight(RestirFrame F, RsSurface s, uint light, float u1, float u2, out RsLightEval e) {
    e.contrib = float3(0.0, 0.0, 0.0);
    e.phat = 0.0;
    e.origin = float3(0.0, 0.0, 0.0);
    e.dir = float3(0.0, 0.0, 0.0);
    e.tMax = 0.0;
    const LtHeader tree = ((LtHeader*)F.lightTree)[0];
    if (light >= tree.emitterCount || light >= F.lightCount) {
        return false;
    }
    const LtEmitter em = ((LtEmitter*)tree.emitters)[light];
    const RestirLight L = ((RestirLight*)F.lights)[light];
    float3 pos;
    float pdfArea;
    ltSamplePoint(em, u1, u2, pos, pdfArea);
    const float3 radiance = rsLoad(L.radiance);
    e.origin = rsOffset(F, s.p, s.n);
    if (em.kind == kLtKindDirectional) {
        const float3 wi = float3(-pos.x, -pos.y, -pos.z);
        const float cosX = rsDot(s.n, wi);
        if (!(cosX > 0.0)) {
            return false;
        }
        e.contrib = radiance * cosX;
        e.dir = wi;
        e.tMax = F.farDistance;
    } else {
        const float3 d = pos - s.p;
        const float dist2 = rsDot(d, d);
        if (!(dist2 > 1e-12)) {
            return false;
        }
        const float dist = sqrt(dist2);
        const float3 wi = float3(d.x / dist, d.y / dist, d.z / dist);
        const float cosX = rsDot(s.n, wi);
        if (!(cosX > 0.0)) {
            return false;
        }
        float g = 0.0;
        if (rsAreaKind(em.kind)) {
            float cosL = -rsDot(rsLoad(em.normal), wi);
            if ((em.flags & kLtFlagTwoSided) != 0u) {
                cosL = abs(cosL);
            }
            if (!(cosL > 0.0)) {
                return false;
            }
            g = (cosX * cosL) / dist2;
        } else {
            g = cosX / dist2;
            if (em.kind == kLtKindSpot) {
                const float cosA = -rsDot(rsLoad(em.normal), wi);
                g = g * rsSpot(cosA, L.cosInner, L.cosOuter);
            }
        }
        e.contrib = radiance * g;
        const float3 toLight = pos - e.origin;
        const float dl = rsLength(toLight);
        if (dl > 1e-6) {
            e.dir = float3(toLight.x / dl, toLight.y / dl, toLight.z / dl);
            e.tMax = dl * kRsShadowShorten;
        } else {
            e.dir = s.n;
            e.tMax = 0.0;
        }
    }
    e.phat = rsLum(e.contrib);
    return e.phat > 0.0;
}

float rsDiTarget(RestirFrame F, RsSurface s, RestirDiReservoir r, bool visibility) {
    RsLightEval e;
    if (!rsValid(s) || r.light == kRsInvalid || !rsEvalLight(F, s, r.light, r.u1, r.u2, e)) {
        return 0.0;
    }
    if (visibility && e.tMax > 0.0 && rsOccluded(F, e.origin, e.dir, e.tMax)) {
        return 0.0;
    }
    return e.phat;
}

RestirDiReservoir rsDiEmpty() {
    RestirDiReservoir r;
    r.light = kRsInvalid;
    r.u1 = 0.0;
    r.u2 = 0.0;
    r.W = 0.0;
    r.M = 0.0;
    r.targetPdf = 0.0;
    r.reserved[0] = 0u;
    r.reserved[1] = 0u;
    return r;
}

RestirDiReservoir rsDiInitial(RestirFrame F, uint px, uint py) {
    const uint pixel = py * F.width + px;
    RestirDiReservoir r = rsDiEmpty();
    const RsSurface s = rsSurface(F, 0u, pixel);
    if (!rsValid(s)) {
        return r;
    }
    r.M = 1.0;
    const uint candidates = F.diCandidates;
    if (candidates == 0u) {
        return r;
    }
    const uint sobol = rsSeed(px, py, 0u, kRsStreamDiSobol, F.seed);
    uint rng = rsSeed(px, py, F.frameIndex, kRsStreamDiInitial, F.seed);
    float wSum = 0.0;
    float selPhat = 0.0;
    RsLightEval sel;
    sel.contrib = float3(0.0, 0.0, 0.0);
    sel.phat = 0.0;
    sel.origin = float3(0.0, 0.0, 0.0);
    sel.dir = float3(0.0, 0.0, 0.0);
    sel.tMax = 0.0;
    for (uint j = 0u; j < candidates; ++j) {
        const float u0 = rsNext(rng);
        float u1;
        float u2;
        rsSobol(F.frameIndex * candidates + j, sobol, u1, u2);
        const LtSampleResult ls = ltSample(F.lightTree, s.p, s.n, u0, u1, u2);
        if (ls.light == kLtInvalid) {
            continue;
        }
        const float src = rsSourcePdf(ls.kind, ls.pmf, ls.pdfArea);
        if (!(src > 0.0)) {
            continue;
        }
        RsLightEval e;
        if (!rsEvalLight(F, s, ls.light, u1, u2, e)) {
            continue;
        }
        const float w = e.phat / src;
        wSum = wSum + w;
        const float u = rsNext(rng);
        const float uw = u * wSum;
        if (uw < w) {
            r.light = ls.light;
            r.u1 = u1;
            r.u2 = u2;
            selPhat = e.phat;
            sel = e;
        }
    }
    if (r.light != kRsInvalid) {
        const float mean = wSum / float(candidates);
        r.W = mean / selPhat;
        r.targetPdf = selPhat;
        if ((F.flags & kRsFlagVisibilityReuse) != 0u && sel.tMax > 0.0 && rsOccluded(F, sel.origin, sel.dir, sel.tMax)) {
            r.W = 0.0;
        }
    }
    return r;
}

// --- reuse helpers ---------------------------------------------------------------------------------------------
bool rsSimilar(RestirFrame F, RsSurface s, RsSurface q) {
    if (!rsValid(q)) {
        return false;
    }
    if (!(rsDot(s.n, q.n) >= F.normalThreshold)) {
        return false;
    }
    const float dz = abs(q.depth - s.depth);
    const float lim = F.depthThreshold * s.depth;
    return dz <= lim;
}

bool rsReproject(RestirFrame F, uint px, uint py, out uint q) {
    q = 0u;
    if ((F.flags & kRsFlagHistory) == 0u) {
        return false;
    }
    float2 m = float2(0.0, 0.0);
    if ((F.flags & kRsFlagMotion) != 0u) {
        m = ((float2*)F.motion)[py * F.width + px];
    }
    const float ux = ((float(px) + 0.5) * F.invWidth) - m.x;
    const float uy = ((float(py) + 0.5) * F.invHeight) - m.y;
    if (!(ux >= 0.0 && ux < 1.0 && uy >= 0.0 && uy < 1.0)) {
        return false;
    }
    const float fx = ux * float(F.width);
    const float fy = uy * float(F.height);
    uint qx = uint(fx);
    uint qy = uint(fy);
    qx = qx < F.width ? qx : F.width - 1u;
    qy = qy < F.height ? qy : F.height - 1u;
    q = qy * F.width + qx;
    return true;
}

bool rsNeighbor(RestirFrame F, uint px, uint py, float radius, inout uint rng, out uint q) {
    q = 0u;
    const float a = rsNext(rng);
    const float b = rsNext(rng);
    float ox;
    float oy;
    ltConcentric(a, b, ox, oy);
    const float fx = ox * radius;
    const float fy = oy * radius;
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
RestirDiReservoir rsDiReuse(RestirFrame F, uint px, uint py, uint mode, uint iteration) {
    const uint pixel = py * F.width + px;
    const RsSurface s = rsSurface(F, 0u, pixel);
    RestirDiReservoir* src = (RestirDiReservoir*)pc.src;
    const RestirDiReservoir canon = src[pixel];
    if (!rsValid(s)) {
        return canon;
    }
    RestirDiReservoir inR[9];
    RsSurface sf[9];
    uint count = 1u;
    inR[0] = canon;
    sf[0] = s;
    uint rng = rsSeed(px, py, F.frameIndex, kRsStreamDiReuse + mode * 16u + iteration, F.seed);
    if (mode == kRsModeTemporal) {
        uint q;
        if (rsReproject(F, px, py, q)) {
            const RsSurface sp = rsSurface(F, 1u, q);
            if (rsSimilar(F, s, sp)) {
                RestirDiReservoir h = ((RestirDiReservoir*)F.diHistory[1])[q];
                const float cap = F.diMCap * canon.M;
                h.M = h.M < cap ? h.M : cap;
                inR[count] = h;
                sf[count] = sp;
                ++count;
            }
        }
    } else {
        const uint neighbors = F.diNeighbors < kRsMaxNeighbors ? F.diNeighbors : kRsMaxNeighbors;
        for (uint k = 0u; k < neighbors; ++k) {
            uint q;
            if (!rsNeighbor(F, px, py, F.diRadius, rng, q)) {
                continue;
            }
            const RsSurface sq = rsSurface(F, 0u, q);
            if (!rsSimilar(F, s, sq)) {
                continue;
            }
            inR[count] = src[q];
            sf[count] = sq;
            ++count;
        }
    }
    const bool unbiased = (F.flags & kRsFlagUnbiased) != 0u;
    float mSum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        mSum = mSum + inR[i].M;
    }
    RestirDiReservoir outR = rsDiEmpty();
    outR.M = mSum;
    float wSum = 0.0;
    float selPhat = 0.0;
    for (uint i = 0u; i < count; ++i) {
        const RestirDiReservoir ri = inR[i];
        if (!(ri.W > 0.0) || ri.light == kRsInvalid) {
            continue;
        }
        const float pcv = rsDiTarget(F, s, ri, unbiased);
        if (!(pcv > 0.0)) {
            continue;
        }
        float w = 0.0;
        if (unbiased) {
            float num = 0.0;
            float den = 0.0;
            for (uint j = 0u; j < count; ++j) {
                const float pj = j == 0u ? pcv : rsDiTarget(F, sf[j], ri, true);
                const float mj = inR[j].M * pj;
                den = den + mj;
                if (j == i) {
                    num = mj;
                }
            }
            const float m = num / den;
            w = (m * pcv) * ri.W;
        } else {
            w = (pcv * ri.W) * ri.M;
        }
        if (!(w > 0.0)) {
            continue;
        }
        wSum = wSum + w;
        const float u = rsNext(rng);
        const float uw = u * wSum;
        if (uw < w) {
            outR.light = ri.light;
            outR.u1 = ri.u1;
            outR.u2 = ri.u2;
            selPhat = pcv;
        }
    }
    if (outR.light != kRsInvalid) {
        const float wu = wSum / selPhat;
        const float d = selPhat * mSum;
        const float wb = wSum / d;
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
    float3 origin;
    float3 dir;
    float tMax;
};

RestirGiReservoir rsGiEmpty() {
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

bool rsGiEvaluate(RestirFrame F, RsSurface s, RestirGiReservoir r, out RsGiEval e) {
    e.cosV = 0.0;
    e.phat = 0.0;
    e.area = 0.0;
    e.origin = float3(0.0, 0.0, 0.0);
    e.dir = float3(0.0, 0.0, 0.0);
    e.tMax = 0.0;
    if (!rsValid(s)) {
        return false;
    }
    const float3 xs = rsLoad(r.position);
    const float3 ns = rsLoad(r.normal);
    const float3 d = xs - s.p;
    const float dist2 = rsDot(d, d);
    if (!(dist2 > 1e-12)) {
        return false;
    }
    const float dist = sqrt(dist2);
    const float3 w = float3(d.x / dist, d.y / dist, d.z / dist);
    const float cosV = rsDot(s.n, w);
    if (!(cosV > 0.0)) {
        return false;
    }
    const float cosS = -rsDot(ns, w);
    if (!(cosS > 0.0)) {
        return false;
    }
    const float phat = rsLum(rsLoad(r.radiance)) * cosV;
    if (!(phat > 0.0)) {
        return false;
    }
    e.cosV = cosV;
    e.phat = phat;
    e.area = cosS / dist2;
    e.origin = rsOffset(F, s.p, s.n);
    const float3 target = xs + ns * F.normalBias;
    const float3 to = target - e.origin;
    const float dl = rsLength(to);
    if (dl > 1e-6) {
        e.dir = float3(to.x / dl, to.y / dl, to.z / dl);
        e.tMax = dl * kRsShadowShorten;
    } else {
        e.dir = s.n;
        e.tMax = 0.0;
    }
    return true;
}

float3 rsCosineDirection(float3 n, float u, float v) {
    float dx;
    float dy;
    ltConcentric(u, v, dx, dy);
    const float xx = dx * dx;
    const float yy = dy * dy;
    const float r = 1.0 - xx;
    const float dz = ltSafeSqrt(r - yy);
    float3 t;
    float3 b;
    rsBasis(n, t, b);
    const float3 tx = t * dx;
    const float3 by = b * dy;
    const float3 nz = n * dz;
    const float3 ab = tx + by;
    const float3 d = ab + nz;
    return rsNormalize(d, n);
}

RestirGiReservoir rsGiInitial(RestirFrame F, uint px, uint py, uint64_t dump) {
    const uint pixel = py * F.width + px;
    RestirGiReservoir r = rsGiEmpty();
    RestirGiHitRecord rec;
    rec.t = -1.0;
    rec.instance = kRsInvalid;
    rec.primitive = kRsInvalid;
    rec.flags = 0u;
    rec.direction[0] = 0.0;
    rec.direction[1] = 0.0;
    rec.direction[2] = 0.0;
    rec.reserved = 0.0;
    const RsSurface s = rsSurface(F, 0u, pixel);
    if (!rsValid(s)) {
        if (dump != 0ull) {
            ((RestirGiHitRecord*)dump)[pixel] = rec;
        }
        return r;
    }
    r.M = 1.0;
    float u;
    float v;
    rsSobol(F.frameIndex, rsSeed(px, py, 0u, kRsStreamGiDir, F.seed), u, v);
    const float3 dir = rsCosineDirection(s.n, u, v);
    const float cosV = rsDot(s.n, dir);
    rec.direction[0] = dir.x;
    rec.direction[1] = dir.y;
    rec.direction[2] = dir.z;
    if (!(cosV > 0.0)) {
        if (dump != 0ull) {
            ((RestirGiHitRecord*)dump)[pixel] = rec;
        }
        return r;
    }
    RsHit hit;
    const bool found = rsTraceHit(F, s.p, dir, F.giRayTMin, F.farDistance, hit);
    rec.flags = kRsHitTraced | (found ? kRsHitHit : 0u);
    rec.t = found ? hit.t : -1.0;
    rec.instance = found ? hit.instance : kRsInvalid;
    rec.primitive = found ? hit.primitive : kRsInvalid;
    if (dump != 0ull) {
        ((RestirGiHitRecord*)dump)[pixel] = rec;
    }
    if (!found) {
        return r;
    }
    const float3 xs = s.p + dir * hit.t;
    float3 ns = rsNormalize(hit.normal, -dir);
    if (rsDot(ns, dir) > 0.0) {
        ns = -ns;
    }
    RsSurface hs;
    hs.p = xs;
    hs.depth = 1.0;
    hs.n = ns;
    uint rng = rsSeed(px, py, F.frameIndex, kRsStreamGiNee, F.seed);
    const float u0 = rsNext(rng);
    float u1;
    float u2;
    rsSobol(F.frameIndex, rsSeed(px, py, 0u, kRsStreamGiNeeSobol, F.seed), u1, u2);
    const LtSampleResult ls = ltSample(F.lightTree, xs, ns, u0, u1, u2);
    float3 lo = float3(0.0, 0.0, 0.0);
    if (ls.light != kLtInvalid) {
        const float src = rsSourcePdf(ls.kind, ls.pmf, ls.pdfArea);
        RsLightEval e;
        if (src > 0.0 && rsEvalLight(F, hs, ls.light, u1, u2, e)) {
            if (!(e.tMax > 0.0) || !rsOccluded(F, e.origin, e.dir, e.tMax)) {
                const float lx = ((hit.albedo.x * e.contrib.x) / kRsPi) / src;
                const float ly = ((hit.albedo.y * e.contrib.y) / kRsPi) / src;
                const float lz = ((hit.albedo.z * e.contrib.z) / kRsPi) / src;
                lo = float3(lx, ly, lz);
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
    const float phat = rsLum(lo) * cosV;
    if (phat > 0.0) {
        r.W = kRsPi / cosV;
        r.targetPdf = phat;
    }
    return r;
}

float rsGiAreaTarget(RestirFrame F, RsSurface s, RestirGiReservoir r) {
    RsGiEval e;
    if (!rsGiEvaluate(F, s, r, e)) {
        return 0.0;
    }
    if (e.tMax > 0.0 && rsOccluded(F, e.origin, e.dir, e.tMax)) {
        return 0.0;
    }
    return e.phat * e.area;
}

RestirGiReservoir rsGiReuse(RestirFrame F, uint px, uint py, uint mode, uint iteration) {
    const uint pixel = py * F.width + px;
    const RsSurface s = rsSurface(F, 0u, pixel);
    RestirGiReservoir* src = (RestirGiReservoir*)pc.src;
    const RestirGiReservoir canon = src[pixel];
    if (!rsValid(s)) {
        return canon;
    }
    RestirGiReservoir inR[9];
    RsSurface sf[9];
    uint count = 1u;
    inR[0] = canon;
    sf[0] = s;
    uint rng = rsSeed(px, py, F.frameIndex, kRsStreamGiReuse + mode * 16u + iteration, F.seed);
    if (mode == kRsModeTemporal) {
        uint q;
        if (rsReproject(F, px, py, q)) {
            const RsSurface sp = rsSurface(F, 1u, q);
            if (rsSimilar(F, s, sp)) {
                RestirGiReservoir h = ((RestirGiReservoir*)F.giHistory[1])[q];
                const float cap = F.giMCap * canon.M;
                h.M = h.M < cap ? h.M : cap;
                inR[count] = h;
                sf[count] = sp;
                ++count;
            }
        }
    } else {
        const uint neighbors = F.giNeighbors < kRsMaxNeighbors ? F.giNeighbors : kRsMaxNeighbors;
        for (uint k = 0u; k < neighbors; ++k) {
            uint q;
            if (!rsNeighbor(F, px, py, F.giRadius, rng, q)) {
                continue;
            }
            const RsSurface sq = rsSurface(F, 0u, q);
            if (!rsSimilar(F, s, sq)) {
                continue;
            }
            inR[count] = src[q];
            sf[count] = sq;
            ++count;
        }
    }
    const bool unbiased = (F.flags & kRsFlagUnbiased) != 0u;
    float mSum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        mSum = mSum + inR[i].M;
    }
    RestirGiReservoir outR = rsGiEmpty();
    outR.M = mSum;
    float wSum = 0.0;
    float selPhat = 0.0;
    bool selected = false;
    for (uint i = 0u; i < count; ++i) {
        const RestirGiReservoir ri = inR[i];
        if (!(ri.W > 0.0)) {
            continue;
        }
        RsGiEval ec;
        if (!rsGiEvaluate(F, s, ri, ec)) {
            continue;
        }
        if (unbiased && ec.tMax > 0.0 && rsOccluded(F, ec.origin, ec.dir, ec.tMax)) {
            continue;
        }
        const float pcv = ec.phat;
        float jacobian = 1.0;
        if (i != 0u) {
            RsGiEval ei;
            if (!rsGiEvaluate(F, sf[i], ri, ei)) {
                continue;
            }
            jacobian = ec.area / ei.area;
            const float jc = jacobian * F.giJacobianClamp;
            if (!unbiased && F.giJacobianClamp > 0.0 && (jacobian > F.giJacobianClamp || jc < 1.0)) {
                continue;
            }
        }
        float w = 0.0;
        if (unbiased) {
            float num = 0.0;
            float den = 0.0;
            for (uint j = 0u; j < count; ++j) {
                const float pa0 = pcv * ec.area;
                const float pj = j == 0u ? pa0 : rsGiAreaTarget(F, sf[j], ri);
                const float mj = inR[j].M * pj;
                den = den + mj;
                if (j == i) {
                    num = mj;
                }
            }
            const float m = num / den;
            w = ((m * pcv) * ri.W) * jacobian;
        } else {
            w = ((pcv * ri.W) * jacobian) * ri.M;
        }
        if (!(w > 0.0)) {
            continue;
        }
        wSum = wSum + w;
        const float u = rsNext(rng);
        const float uw = u * wSum;
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
        const float wu = wSum / selPhat;
        const float d = selPhat * mSum;
        const float wb = wSum / d;
        outR.W = unbiased ? wu : wb;
        outR.targetPdf = selPhat;
    }
    return outR;
}

// --- shade -----------------------------------------------------------------------------------------------------
void rsShade(RestirFrame F, uint px, uint py, bool hasDi, RestirDiReservoir di, bool hasGi, RestirGiReservoir gi, out float4 diOut,
             out float4 giOut, out float depth) {
    const uint pixel = py * F.width + px;
    diOut = float4(0.0, 0.0, 0.0, 0.0);
    giOut = float4(0.0, 0.0, 0.0, 0.0);
    const RsSurface s = rsSurface(F, 0u, pixel);
    depth = s.depth;
    if (!rsValid(s)) {
        depth = 0.0;
        return;
    }
    if (hasDi && di.light != kRsInvalid && di.W > 0.0) {
        RsLightEval e;
        if (rsEvalLight(F, s, di.light, di.u1, di.u2, e)) {
            if (!(e.tMax > 0.0) || !rsOccluded(F, e.origin, e.dir, e.tMax)) {
                const float k = di.W / kRsPi;
                diOut = float4(e.contrib * k, 0.0);
            }
        }
    }
    if (hasGi && gi.W > 0.0) {
        RsGiEval e;
        if (rsGiEvaluate(F, s, gi, e)) {
            const bool trace = (F.flags & kRsFlagGiShadeVisibility) != 0u && e.tMax > 0.0;
            if (!trace || !rsOccluded(F, e.origin, e.dir, e.tMax)) {
                const float cw = e.cosV * gi.W;
                const float k = cw / kRsPi;
                giOut = float4(rsLoad(gi.radiance) * k, 0.0);
            }
        }
    }
}
