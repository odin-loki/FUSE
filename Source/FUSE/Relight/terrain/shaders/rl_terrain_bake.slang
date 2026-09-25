// FUSE Relight RL-5.6 "relight.terrain.*": the GPU terrain bake. Slang twin of rl_terrain_bake.comp (see there);
// compile with -fp-mode precise.
struct TerrainPush {
    float4 clearColor; ///< first: 16-byte aligned
    uint64_t layers;
    uint64_t vertices;
    uint64_t indices;
    uint64_t texels;
    uint64_t outputs;
    uint layer;
    uint width;
    uint height;
    uint stage;
    float minX;
    float minZ;
    float sx;
    float sz;
};
[[vk::push_constant]] ConstantBuffer<TerrainPush> pc;

uint tbWrap(int i, uint n) { return uint(((i % int(n)) + int(n)) % int(n)); }

float4 tbSample(uint base, uint w, uint h, float u, float v) {
    float x = u * float(w) - 0.5;
    float y = v * float(h) - 0.5;
    float fx = floor(x);
    float fy = floor(y);
    float ax = x - fx;
    float ay = y - fy;
    uint x0 = tbWrap(int(fx), w), x1 = tbWrap(int(fx) + 1, w);
    uint y0 = tbWrap(int(fy), h), y1 = tbWrap(int(fy) + 1, h);
    float4* t = (float4*)pc.texels;
    float4 a = t[base + y0 * w + x0];
    float4 b = t[base + y0 * w + x1];
    float4 c = t[base + y1 * w + x0];
    float4 d = t[base + y1 * w + x1];
    float4 top = a + (b - a) * ax;
    float4 bottom = c + (d - c) * ax;
    float4 r = top + (bottom - top) * ay;
    return r;
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    uint xi = tid.x;
    uint zi = tid.y;
    if (xi >= pc.width || zi >= pc.height) {
        return;
    }
    uint i = zi * pc.width + xi;
    float4* o = (float4*)pc.outputs;
    if (pc.stage == 0u) {
        o[i] = pc.clearColor;
        return;
    }
    uint4 l0 = ((uint4*)pc.layers)[pc.layer * 4u];
    uint4 l1 = ((uint4*)pc.layers)[pc.layer * 4u + 1u];
    float4 l2 = ((float4*)pc.layers)[pc.layer * 4u + 2u];
    float4 l3 = ((float4*)pc.layers)[pc.layer * 4u + 3u];
    float4* vtx = (float4*)pc.vertices;
    uint* idx = (uint*)pc.indices;
    float px = float(xi) + 0.5;
    float pz = float(zi) + 0.5;
    uint best = 0xFFFFFFFFu;
    float bestY = 0.0;
    float bw1 = 0.0;
    float bw2 = 0.0;
    for (uint t = 0u; t + 2u < l0.z; t += 3u) {
        uint i0 = idx[l0.y + t], i1 = idx[l0.y + t + 1u], i2 = idx[l0.y + t + 2u];
        if (i0 >= l0.w || i1 >= l0.w || i2 >= l0.w) {
            continue;
        }
        float4 p0 = vtx[(l0.x + i0) * 3u];
        float4 p1 = vtx[(l0.x + i1) * 3u];
        float4 p2 = vtx[(l0.x + i2) * 3u];
        float ax = (p0.x - pc.minX) * pc.sx, az = (p0.z - pc.minZ) * pc.sz;
        float bx = (p1.x - pc.minX) * pc.sx, bz = (p1.z - pc.minZ) * pc.sz;
        float cx = (p2.x - pc.minX) * pc.sx, cz = (p2.z - pc.minZ) * pc.sz;
        float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
        if (area == 0.0) {
            continue;
        }
        float inv = 1.0 / area;
        float w1 = ((px - ax) * (cz - az) - (pz - az) * (cx - ax)) * inv;
        float w2 = ((bx - ax) * (pz - az) - (bz - az) * (px - ax)) * inv;
        float w0 = 1.0 - w1 - w2;
        if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
            continue;
        }
        float y = p0.y * w0 + p1.y * w1 + p2.y * w2;
        if (best == 0xFFFFFFFFu || y > bestY) {
            best = t;
            bestY = y;
            bw1 = w1;
            bw2 = w2;
        }
    }
    if (best == 0xFFFFFFFFu) {
        return;
    }
    float w0 = 1.0 - bw1 - bw2;
    uint v0 = l0.x + idx[l0.y + best], v1 = l0.x + idx[l0.y + best + 1u], v2 = l0.x + idx[l0.y + best + 2u];
    uint flags = l1.w & 0xFFu;
    uint blend = l1.w >> 8u;
    float u = 0.0;
    float v = 0.0;
    if ((flags & 2u) != 0u) {
        float uu = vtx[v0 * 3u + 1u].x * w0 + vtx[v1 * 3u + 1u].x * bw1 + vtx[v2 * 3u + 1u].x * bw2;
        float vv = vtx[v0 * 3u + 1u].y * w0 + vtx[v1 * 3u + 1u].y * bw1 + vtx[v2 * 3u + 1u].y * bw2;
        u = uu;
        v = vv;
    }
    float tu = l2.x * u + l2.y * v + l2.z;
    float tv = l2.w * u + l3.x * v + l3.y;
    float4 src = float4(1.0, 1.0, 1.0, 1.0);
    if ((flags & 8u) != 0u) {
        src = tbSample(l1.x, l1.y, l1.z, tu, tv);
    }
    if ((flags & 4u) != 0u) {
        float4 c = vtx[v0 * 3u + 2u] * w0 + vtx[v1 * 3u + 2u] * bw1 + vtx[v2 * 3u + 2u] * bw2;
        float4 s2 = src * c;
        src = s2;
    }
    if ((flags & 1u) != 0u && !(src.w > l3.z)) {
        return;
    }
    float4 dst = o[i];
    float a = src.w;
    if (blend == 0u) {
        dst = src;
    } else if (blend == 1u) {
        dst.xyz = src.xyz * a + dst.xyz * (1.0 - a);
        dst.w = a + dst.w * (1.0 - a);
    } else if (blend == 2u) {
        dst.xyz = dst.xyz + src.xyz * a;
    } else if (blend == 3u) {
        dst.xyz = dst.xyz * src.xyz;
    } else {
        dst.xyz = dst.xyz * src.xyz + src.xyz * dst.xyz;
    }
    o[i] = dst;
}
