// WP-3.2: the include chain of every WP-3.2 Slang kernel. Twin of vsm_raster_common.glsl (see there).
#pragma once
#include "gpu_scene.slang"
#include "vsm_shadow.slang"

struct VsmrPush {
    uint64_t shadow; // BDA of VsmShadowConstants
    uint64_t in_;    // vsm.probe: VsmProbeInput[count]
    uint64_t out_;   // vsm.probe: VsmProbeOutput[count]
    uint count;
    uint arg;
};
[[vk::push_constant]] ConstantBuffer<VsmrPush> pc;

[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_vsmr_words[];
[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> fuse_vsmr_images[];

static const uint kFuseVsmrThreads = 64u;
static const int kFuseVsmrBigTexels = 256;
static const uint kFuseVsmrWordLocalX = 0u;
static const uint kFuseVsmrWordDirPages = 3u;
static const uint kFuseVsmrWordLocalPages = 4u;
static const uint kFuseVsmrWordDirty = 5u;
static const uint kFuseVsmrWordDirTriangles = 6u;
static const uint kFuseVsmrWordDirBig = 7u;
static const uint kFuseVsmrWordLocalTriangles = 8u;
static const uint kFuseVsmrWordLocalList = 16u;
static const uint kFuseVsmrMaxLocalPages = 256u;
static const float kFuseVsmrMaxArea = 3.0e38;

VsmShadow* fuseVsmrShadow() { return (VsmShadow*)pc.shadow; }
uint fuseVsmrIndex(uint handle) { return handle & 0xFFFFFu; }

// --- vertices -------------------------------------------------------------------------------------
float3 fuseVsmrMeshPosition(GpuMesh mesh, uint vertex) {
    uint* words = (uint*)mesh.positions;
    const uint xy = words[vertex * 2u];
    const uint zw = words[vertex * 2u + 1u];
    float3 p;
    p.x = mesh.quantOffset[0] + float(xy & 0xFFFFu) * mesh.quantStep[0];
    p.y = mesh.quantOffset[1] + float(xy >> 16u) * mesh.quantStep[1];
    p.z = mesh.quantOffset[2] + float(zw & 0xFFFFu) * mesh.quantStep[2];
    return p;
}

float3 fuseVsmrWorldPoint(GpuTransform t, float3 p) {
    float3 w;
    w.x = ((t.rows[0].x * p.x + t.rows[0].y * p.y) + t.rows[0].z * p.z) + t.rows[0].w;
    w.y = ((t.rows[1].x * p.x + t.rows[1].y * p.y) + t.rows[1].z * p.z) + t.rows[1].w;
    w.z = ((t.rows[2].x * p.x + t.rows[2].y * p.y) + t.rows[2].z * p.z) + t.rows[2].w;
    return w;
}

bool fuseVsmrTriangleWorld(GpuSceneHeader* scene, GpuMesh mesh, GpuTransform xf, uint tri, out float3 w[3]) {
    w[0] = float3(0.0, 0.0, 0.0);
    w[1] = float3(0.0, 0.0, 0.0);
    w[2] = float3(0.0, 0.0, 0.0);
    uint* indices = fuseGpuSceneIndices(scene);
    const uint base = mesh.firstIndex + tri * 3u;
    for (uint k = 0u; k < 3u; ++k) {
        const uint v = uint(int(indices[base + k]) + mesh.vertexOffset);
        if (v >= mesh.vertexCount) {
            return false;
        }
        w[k] = fuseVsmrWorldPoint(xf, fuseVsmrMeshPosition(mesh, v));
    }
    return true;
}

bool fuseVsmrCasterBounds(GpuInstance inst, GpuMesh mesh, GpuTransform t, out float3 c, out float3 e) {
    c = float3(0.0, 0.0, 0.0);
    e = float3(0.0, 0.0, 0.0);
    const uint need = kFuseInstanceValid | kFuseInstanceCastShadow;
    if ((inst.flags & need) != need || mesh.indexCount == 0u || mesh.positions == 0ull) {
        return false;
    }
    const float rad = mesh.boundsRadius + ((abs(mesh.quantStep[0]) + abs(mesh.quantStep[1])) + abs(mesh.quantStep[2]));
    for (uint k = 0u; k < 3u; ++k) {
        const float4 row = t.rows[k];
        c[k] = ((row.x * mesh.boundsCenter[0] + row.y * mesh.boundsCenter[1]) + row.z * mesh.boundsCenter[2]) + row.w;
        e[k] = rad * ((abs(row.x) + abs(row.y)) + abs(row.z));
    }
    return true;
}

bool fuseVsmrBoxSphere(float3 c, float3 e, float3 p, float radius) {
    const float dx = max(abs(c.x - p.x) - e.x, 0.0);
    const float dy = max(abs(c.y - p.y) - e.y, 0.0);
    const float dz = max(abs(c.z - p.z) - e.z, 0.0);
    return (dx * dx + dy * dy) + dz * dz <= radius * radius;
}

void fuseVsmrDirPageOf(VsmConstants* C, uint vp, out uint level, out int ax, out int ay) {
    level = vp / kFuseVsmPagesPerLevel;
    const uint inLevel = vp % kFuseVsmPagesPerLevel;
    const uint sx = inLevel % kFuseVsmPagesPerAxis;
    const uint sy = inLevel / kFuseVsmPagesPerAxis;
    const VsmLevel L = C.level[level < 16u ? level : 0u];
    const int half_ = int(kFuseVsmPagesPerAxis / 2u);
    const int bx = L.originX - half_;
    const int by = L.originY - half_;
    ax = bx + int(uint(int(sx) - bx) & (kFuseVsmPagesPerAxis - 1u));
    ay = by + int(uint(int(sy) - by) & (kFuseVsmPagesPerAxis - 1u));
}

float3 fuseVsmrDirPageVertex(VsmLevel L, int ax, int ay, float3 l) {
    const float x = (l.x * L.invPageWorld - float(ax)) * float(kFuseVsmPageTexels);
    const float y = (l.y * L.invPageWorld - float(ay)) * float(kFuseVsmPageTexels);
    return float3(x, y, fuseVsmrDirDepth(L, l.z));
}

// --- triangle rasteriser --------------------------------------------------------------------------
// x, y texel coordinates; directional: z = vertex depths; local: z = view-plane normal, h = offset.
struct VsmrTri {
    float3 x;
    float3 y;
    float3 z;
    float h;
};

struct VsmrSetup {
    float3 ex;
    float3 ey;
    float invArea;
    float dz1;
    float dz2;
    int4 box; // x0, y0, x1, y1 (inclusive)
};

bool fuseVsmrSetup(VsmrTri t, out VsmrSetup s) {
    s.ex = float3(t.x[2] - t.x[1], t.x[0] - t.x[2], t.x[1] - t.x[0]);
    s.ey = float3(t.y[2] - t.y[1], t.y[0] - t.y[2], t.y[1] - t.y[0]);
    s.invArea = 0.0;
    s.dz1 = 0.0;
    s.dz2 = 0.0;
    s.box = int4(0, 0, -1, -1);
    const float area = s.ex[2] * (t.y[2] - t.y[0]) - s.ey[2] * (t.x[2] - t.x[0]);
    if (!(abs(area) > 0.0) || !(abs(area) < kFuseVsmrMaxArea)) {
        return false;
    }
    s.invArea = 1.0 / area;
    s.dz1 = t.z[1] - t.z[0];
    s.dz2 = t.z[2] - t.z[0];
    const float minX = min(min(t.x[0], t.x[1]), t.x[2]);
    const float maxX = max(max(t.x[0], t.x[1]), t.x[2]);
    const float minY = min(min(t.y[0], t.y[1]), t.y[2]);
    const float maxY = max(max(t.y[0], t.y[1]), t.y[2]);
    const float fx0 = max(ceil(minX - 0.5), 0.0);
    const float fx1 = min(floor(maxX - 0.5), 127.0);
    const float fy0 = max(ceil(minY - 0.5), 0.0);
    const float fy1 = min(floor(maxY - 0.5), 127.0);
    if (!(fx0 <= fx1) || !(fy0 <= fy1)) {
        return false;
    }
    s.box = int4(int(fx0), int(fy0), int(fx1), int(fy1));
    return true;
}

bool fuseVsmrTexel(VsmrTri t, VsmrSetup s, int px, int py, out float z) {
    z = 0.0;
    const float cx = float(px) + 0.5;
    const float cy = float(py) + 0.5;
    const float w0 = s.ex[0] * (cy - t.y[1]) - s.ey[0] * (cx - t.x[1]);
    const float w1 = s.ex[1] * (cy - t.y[2]) - s.ey[1] * (cx - t.x[2]);
    const float w2 = s.ex[2] * (cy - t.y[0]) - s.ey[2] * (cx - t.x[0]);
    const float b0 = w0 * s.invArea;
    const float b1 = w1 * s.invArea;
    const float b2 = w2 * s.invArea;
    if (!(b0 >= 0.0) || !(b1 >= 0.0) || !(b2 >= 0.0)) {
        return false;
    }
    z = (t.z[0] + b1 * s.dz1) + b2 * s.dz2;
    return true;
}

float fuseVsmrClampDepth(float d) {
    const float lo = d > 0.0 ? d : 0.0;
    return lo < 1.0 ? lo : 1.0;
}

float fuseVsmrPerspDepth(VsmrTri t, VsmLocal e, int px, int py) {
    const float qx = ((float(px) + 0.5 - 64.0) * (1.0 / 64.0)) / e.invTanHalf;
    const float qy = ((float(py) + 0.5 - 64.0) * (1.0 / 64.0)) / e.invTanHalf;
    const float den = (t.z[0] * qx + t.z[1] * qy) + t.z[2];
    const float z = t.h / den;
    return z > 0.0 ? fuseVsmrClampDepth(z * e.invRange) : 1.0;
}

void fuseVsmrWriteDir(uint image, int2 origin, VsmrTri t, VsmrSetup s, int px, int py) {
    float z;
    if (fuseVsmrTexel(t, s, px, py, z)) {
        uint previous;
        InterlockedMin(fuse_vsmr_images[image][uint2(origin + int2(px, py))], asuint(fuseVsmrClampDepth(z)), previous);
    }
}

void fuseVsmrWriteLocal(uint image, int2 origin, VsmrTri t, VsmrSetup s, int px, int py, VsmLocal e) {
    float z;
    if (fuseVsmrTexel(t, s, px, py, z)) {
        uint previous;
        InterlockedMin(fuse_vsmr_images[image][uint2(origin + int2(px, py))], asuint(fuseVsmrPerspDepth(t, e, px, py)), previous);
    }
}

float4 fuseVsmrViewPlane(float3 v[3]) {
    const float3 e1 = v[1] - v[0];
    const float3 e2 = v[2] - v[0];
    const float3 n = float3(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
    return float4(n, fuseVsmrDot(n, v[0]));
}

uint fuseVsmrClipProject(VsmLocal e, float3 v[3], out VsmrTri outTri[2]) {
    float3 poly[4];
    uint n = 0u;
    const float zn = e.nearPlane;
    for (uint i = 0u; i < 3u; ++i) {
        const float3 a = v[i];
        const float3 b = v[(i + 1u) % 3u];
        const bool ain = a.z >= zn;
        const bool bin = b.z >= zn;
        if (ain) {
            poly[n] = a;
            ++n;
        }
        if (ain != bin) {
            const float t = (zn - a.z) / (b.z - a.z);
            poly[n] = float3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, zn);
            ++n;
        }
    }
    outTri[0].x = float3(0.0, 0.0, 0.0);
    outTri[0].y = float3(0.0, 0.0, 0.0);
    outTri[0].z = float3(0.0, 0.0, 0.0);
    outTri[0].h = 0.0;
    outTri[1] = outTri[0];
    if (n < 3u) {
        return 0u;
    }
    float3 sp[4];
    for (uint i = 0u; i < n; ++i) {
        sp[i] = fuseVsmrLocalProject(e, poly[i]);
    }
    const float4 plane = fuseVsmrViewPlane(v);
    const uint count = n - 2u;
    for (uint k = 0u; k < count; ++k) {
        const uint3 idx = uint3(0u, k + 1u, k + 2u);
        for (uint j = 0u; j < 3u; ++j) {
            outTri[k].x[j] = sp[idx[j]].x;
            outTri[k].y[j] = sp[idx[j]].y;
            outTri[k].z[j] = plane[j];
        }
        outTri[k].h = plane.w;
    }
    return count;
}
