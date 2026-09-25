// "ssrc.cascade". Slang twin of src_cascade.comp (see there).
#include "src_common.slang"
#include "ddgi_sample.slang"

struct SrcRay {
    bool valid;
    float kO;
    float kE;
    float3 qO;
    float3 qE;
    float len;
};

SrcRay srcRaySetup(float3 o, float3 d) {
    SrcRay r;
    r.valid = false;
    r.kO = 0.0;
    r.kE = 0.0;
    r.qO = float3(0.0);
    r.qE = float3(0.0);
    r.len = 0.0;
    const float nearLimit = F.nearZ * 1.01;
    if (!(o.z >= nearLimit)) {
        return r;
    }
    float len = F.maxDistance;
    if (d.z < 0.0) {
        len = min(len, (o.z - nearLimit) / (-d.z));
    }
    if (!(len > 1e-4)) {
        return r;
    }
    const float3 e = srcAdd(o, srcScale(d, len));
    r.kO = 1.0 / o.z;
    r.kE = 1.0 / e.z;
    r.qO = srcScale(o, r.kO);
    r.qE = srcScale(e, r.kE);
    const float ox = F.fx * r.qO.x + F.cx;
    const float oy = F.fy * r.qO.y + F.cy;
    const float ex = F.fx * r.qE.x + F.cx;
    const float ey = F.fy * r.qE.y + F.cy;
    const float dx = ex - ox;
    const float dy = ey - oy;
    r.len = sqrt(dx * dx + dy * dy);
    r.valid = true;
    return r;
}

float3 srcPointAt(SrcRay r, float f) {
    const float k = r.kO + (r.kE - r.kO) * f;
    const float inv = 1.0 / k;
    const float qx = r.qO.x + (r.qE.x - r.qO.x) * f;
    const float qy = r.qO.y + (r.qE.y - r.qO.y) * f;
    const float qz = r.qO.z + (r.qE.z - r.qO.z) * f;
    return float3(qx * inv, qy * inv, qz * inv);
}

bool srcInterval(SrcRay r, uint ci, out float fA, out float fB, out bool ended) {
    const float tS = F.tStart[ci];
    const float tE = F.tEnd[ci];
    fA = 0.0;
    fB = 1.0;
    ended = false;
    if (r.len < 1e-3) {
        if (ci != 0u) {
            return false;
        }
        ended = true;
        return true;
    }
    if (tS > 0.0 && !(r.len > tS)) {
        return false;
    }
    fA = tS / r.len;
    ended = !(tE < r.len);
    if (!ended) {
        fB = tE / r.len;
    }
    return true;
}

float3 srcToWorld(float3 e, bool point) {
    const float x = F.viewToWorld[0] * e.x + F.viewToWorld[1] * e.y + F.viewToWorld[2] * e.z;
    const float y = F.viewToWorld[4] * e.x + F.viewToWorld[5] * e.y + F.viewToWorld[6] * e.z;
    const float z = F.viewToWorld[8] * e.x + F.viewToWorld[9] * e.y + F.viewToWorld[10] * e.z;
    if (point) {
        return float3(x + F.cameraWorld[0], y + F.cameraWorld[1], z + F.cameraWorld[2]);
    }
    return float3(x, y, z);
}

float3 srcFarField(float3 o, float3 d) {
    float r = F.sky[0];
    float g = F.sky[1];
    float b = F.sky[2];
    if ((F.flags & kSrcFlagDdgi) != 0u && F.ddgiVolume != 0) {
        const float3 wo = srcToWorld(float3(o.x, -o.y, -o.z), true);
        const float3 wd = srcToWorld(float3(d.x, -d.y, -d.z), false);
        const float3 e = fuseDdgiSampleIrradiance(F.ddgiVolume, wo, wd);
        r = r + e.x * F.ddgiScale;
        g = g + e.y * F.ddgiScale;
        b = b + e.z * F.ddgiScale;
    }
    return float3(r, g, b);
}

// xyz = hit radiance, w = state (0 hit, 1 continue, 2 escaped).
float srcPatchDepth(float zc, float nd, float3 n, float x, float y) {
    const float rx = (x - F.cx) / F.fx;
    const float ry = (y - F.cy) / F.fy;
    const float nr = srcDot(n, float3(rx, ry, 1.0));
    if (!(abs(nr) >= 1e-6)) {
        return zc;
    }
    float q = nd / nr;
    if (!(q > 0.5)) {
        q = 0.5;
    }
    if (q > 2.0) {
        q = 2.0;
    }
    return zc * q;
}

float4 srcTrace(float3 a, float3 b, uint startPixel) {
    const float ka = 1.0 / a.z;
    const float kb = 1.0 / b.z;
    const float ax = F.fx * (a.x * ka) + F.cx;
    const float ay = F.fy * (a.y * ka) + F.cy;
    const float bx = F.fx * (b.x * kb) + F.cx;
    const float by = F.fy * (b.y * kb) + F.cy;
    const float ddx = bx - ax;
    const float ddy = by - ay;
    const float len = max(abs(ddx), abs(ddy));
    if (!(len >= 1e-3)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float n = min(ceil(len / F.stride), 1048576.0);
    const uint steps = uint(n);
    const float fStep = F.stride / len;
    const float fw = float(F.width);
    const float fh = float(F.height);
    float prevZ = a.z;
    float prevX = ax;
    float prevY = ay;
    const float3 dir = float3(b.x - a.x, b.y - a.y, b.z - a.z);
    for (uint i = 1u; i <= steps; ++i) {
        const float f = min(1.0, float(i) * fStep);
        const float sx = ax + ddx * f;
        const float sy = ay + ddy * f;
        if (!(sx >= 0.0 && sy >= 0.0 && sx < fw && sy < fh)) {
            return float4(0.0, 0.0, 0.0, 2.0);
        }
        const float rayZ = 1.0 / (ka + (kb - ka) * f);
        const uint hx = uint(sx);
        const uint hy = uint(sy);
        const uint pixel = hy * F.width + hx;
        if (pixel != startPixel) {
            const float4 g = srcTexels(F.geo)[pixel];
            const float zc = g.x;
            if (zc > 0.0) {
                const float3 rd = float3(srcRayCoord(hx, F.cx, F.fx), srcRayCoord(hy, F.cy, F.fy), 1.0);
                const float nd = srcDot(g.yzw, rd);
                const float d0 = prevZ - srcPatchDepth(zc, nd, g.yzw, prevX, prevY);
                const float d1 = rayZ - srcPatchDepth(zc, nd, g.yzw, sx, sy);
                const float th = F.thickness + F.thicknessSlope * zc;
                if ((d1 >= 0.0 && d1 <= th) || (d0 < 0.0 && d1 > th) || (d0 > th && d1 < 0.0)) {
                    const float alongRay = srcDot(g.yzw, dir);
                    const bool front = nd > 0.0 ? alongRay > 0.0 : alongRay < 0.0;
                    if (front) {
                        return float4(srcTexels(F.lit)[pixel].xyz, 0.0);
                    }
                    return float4(0.0, 0.0, 0.0, 0.0);
                }
            }
        }
        prevZ = rayZ;
        prevX = sx;
        prevY = sy;
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

float4 srcChildAverage(uint ci, uint ux, uint uy, uint k, float3 n) {
    const uint res = F.dirRes[ci];
    const uint res2 = F.dirRes[ci + 1u];
    const uint ku = k % res;
    const uint kv = k / res;
    const uint base = (uy * F.probesX[ci + 1u] + ux) * (res2 * res2);
    const uint dirBase = F.dirOffset[ci + 1u];
    float sr = 0.0;
    float sg = 0.0;
    float sb = 0.0;
    float sv = 0.0;
    float sw = 0.0;
    float ar = 0.0;
    float ag = 0.0;
    float ab = 0.0;
    float av = 0.0;
    float aw = 0.0;
    for (uint j = 0u; j < 4u; ++j) {
        const uint ck = (2u * kv + (j >> 1u)) * res2 + 2u * ku + (j & 1u);
        const float4 rec = srcDecode(srcRecords(pc.upper)[base + ck]);
        const float4 dt = srcTexels(F.dirs)[dirBase + ck];
        const float w = dt.w;
        ar = ar + w * rec.x;
        ag = ag + w * rec.y;
        ab = ab + w * rec.z;
        av = av + w * rec.w;
        aw = aw + w;
        const float cosine = srcDot(n, dt.xyz);
        if (cosine > 0.0) {
            const float wm = ci == 0u ? w * cosine : w;
            sr = sr + wm * rec.x;
            sg = sg + wm * rec.y;
            sb = sb + wm * rec.z;
            sv = sv + wm * rec.w;
            sw = sw + wm;
        }
    }
    if (sw > 0.0) {
        return float4(sr / sw, sg / sw, sb / sw, sv / sw);
    }
    return float4(ar / aw, ag / aw, ab / aw, av / aw);
}

[numthreads(64, 1, 1)]
void main(uint3 gid: SV_GroupID, uint3 lid: SV_GroupThreadID) {
    srcBegin();
    const uint index = (gid.y * pc.groupsX + gid.x) * 64u + lid.x;
    if (index >= pc.count) {
        return;
    }
    const uint ci = pc.cascade;
    const uint res = F.dirRes[ci];
    const uint count = res * res;
    const uint k = index % count;
    const uint probe = index / count;
    const uint probeX = probe % F.probesX[ci];
    const uint probeY = probe / F.probesX[ci];
    const float3 d = srcTexels(F.dirs)[F.dirOffset[ci] + k].xyz;
    const bool hasUpper = ci + 1u < F.cascades;
    float r = 0.0;
    float g = 0.0;
    float b = 0.0;
    float vis = 1.0;
    const SrcProbe p = srcProbeAt(ci, probeX, probeY);
    float fA = 0.0;
    float fB = 1.0;
    bool ended = false;
    SrcRay ray;
    ray.valid = false;
    if (p.valid) {
        ray = srcRaySetup(p.o, d);
    }
    bool inside = p.valid && ray.valid;
    if (inside) {
        inside = srcInterval(ray, ci, fA, fB, ended);
    }
    if (!inside) {
        const float3 farRgb = srcFarField(p.valid ? p.o : float3(0.0), d);
        r = farRgb.x;
        g = farRgb.y;
        b = farRgb.z;
        vis = 1.0;
    } else {
        const float3 A = srcPointAt(ray, fA);
        const float3 B = srcPointAt(ray, fB);
        bool fallback = !hasUpper;
        const bool below = ci == 0u && !(srcDot(p.n, d) > 0.0);
        if (hasUpper) {
            float4 w;
            const uint4 q = srcBilinear(srcGrid(probeX, F.spacing[ci]), srcGrid(probeY, F.spacing[ci]),
                                        F.spacing[ci + 1u], F.probesX[ci + 1u], F.probesY[ci + 1u], w);
            SrcProbe qs[4];
            bool use[4] = { false, false, false, false };
            bool anyPlane = false;
            for (uint j = 0u; j < 4u; ++j) {
                qs[j].valid = false;
                qs[j].pixel = 0u;
                qs[j].o = float3(0.0);
                qs[j].n = float3(0.0);
                if (!(w[j] > 0.0)) {
                    continue;
                }
                qs[j] = srcProbeAt(ci + 1u, (j & 1u) != 0u ? q.y : q.x, (j >> 1u) != 0u ? q.w : q.z);
                if (!qs[j].valid) {
                    continue;
                }
                const float dist = abs(srcDot(qs[j].n, srcSub(p.o, qs[j].o)));
                use[j] = !(F.planeTolerance > 0.0) || dist <= F.planeTolerance * p.o.z;
                anyPlane = anyPlane || use[j];
            }
            if (!anyPlane) {
                for (uint j = 0u; j < 4u; ++j) {
                    use[j] = w[j] > 0.0 && qs[j].valid;
                }
            }
            float sr = 0.0;
            float sg = 0.0;
            float sb = 0.0;
            float sv = 0.0;
            float sw = 0.0;
            if ((F.flags & kSrcFlagBilinearFix) != 0u && !below) {
                for (uint j = 0u; j < 4u; ++j) {
                    if (!use[j]) {
                        continue;
                    }
                    const float wj = w[j];
                    const uint ux = (j & 1u) != 0u ? q.y : q.x;
                    const uint uy = (j >> 1u) != 0u ? q.w : q.z;
                    float3 target = B;
                    bool toParent = false;
                    const SrcRay qr = srcRaySetup(qs[j].o, d);
                    if (qr.valid && qr.len >= 1e-3 && F.tEnd[ci] < qr.len) {
                        target = srcPointAt(qr, F.tEnd[ci] / qr.len);
                        toParent = true;
                    }
                    const float4 s = srcTrace(A, target, p.pixel);
                    float4 val;
                    if (s.w == 0.0) {
                        val = float4(s.xyz, 0.0);
                    } else if (s.w == 2.0 || (!toParent && ended)) {
                        val = float4(srcFarField(p.o, d), 1.0);
                    } else {
                        val = srcChildAverage(ci, ux, uy, k, p.n);
                    }
                    sr = sr + wj * val.x;
                    sg = sg + wj * val.y;
                    sb = sb + wj * val.z;
                    sv = sv + wj * val.w;
                    sw = sw + wj;
                }
                if (sw > 0.0) {
                    r = sr / sw;
                    g = sg / sw;
                    b = sb / sw;
                    vis = sv / sw;
                } else {
                    fallback = true;
                }
            } else {
                float4 s = float4(0.0, 0.0, 0.0, 1.0);
                if (!below) {
                    s = srcTrace(A, B, p.pixel);
                }
                if (s.w == 0.0) {
                    r = s.x;
                    g = s.y;
                    b = s.z;
                    vis = 0.0;
                } else if (s.w == 2.0 || (ended && !below)) {
                    const float3 farRgb = srcFarField(p.o, d);
                    r = farRgb.x;
                    g = farRgb.y;
                    b = farRgb.z;
                    vis = 1.0;
                } else {
                    for (uint j = 0u; j < 4u; ++j) {
                        if (!use[j]) {
                            continue;
                        }
                        const float wj = w[j];
                        const uint ux = (j & 1u) != 0u ? q.y : q.x;
                        const uint uy = (j >> 1u) != 0u ? q.w : q.z;
                        const float4 val = srcChildAverage(ci, ux, uy, k, p.n);
                        sr = sr + wj * val.x;
                        sg = sg + wj * val.y;
                        sb = sb + wj * val.z;
                        sv = sv + wj * val.w;
                        sw = sw + wj;
                    }
                    if (sw > 0.0) {
                        r = sr / sw;
                        g = sg / sw;
                        b = sb / sw;
                        vis = sv / sw;
                    } else {
                        const float3 farRgb = srcFarField(p.o, d);
                        r = farRgb.x;
                        g = farRgb.y;
                        b = farRgb.z;
                        vis = 1.0;
                    }
                }
            }
        }
        if (fallback) {
            const float4 s = srcTrace(A, B, p.pixel);
            if (s.w == 0.0) {
                r = s.x;
                g = s.y;
                b = s.z;
                vis = 0.0;
            } else {
                const float3 farRgb = srcFarField(p.o, d);
                r = farRgb.x;
                g = farRgb.y;
                b = farRgb.z;
                vis = 1.0;
            }
        }
    }
    if (ci >= F.aoCascades) {
        vis = 1.0;
    }
    srcRecords(pc.dst)[index] =
        uint2(srcF32ToF16(r) | (srcF32ToF16(g) << 16), srcF32ToF16(b) | (srcF32ToF16(vis) << 16));
}
