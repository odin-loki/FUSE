// WP-6.3: the screen-space ray march shared by ssfx.ssr and ssfx.ssgi. Slang twin of sx_trace.glsl (see there).
#pragma once
#include "sx_common.slang"

struct SxTrace {
    uint maxSteps;
    float stride;
    float thickness;
    float maxDistance;
    float fadeEdge;
    uint refineSteps;
};

struct SxHit {
    bool hit;
    float px;
    float py;
    float distance;
    float3 color;
    float confidence;
};

SxHit sxNoHit() {
    SxHit r;
    r.hit = false;
    r.px = 0.0;
    r.py = 0.0;
    r.distance = 0.0;
    r.color = float3(0.0);
    r.confidence = 0.0;
    return r;
}

float sxDepthNearest(float px, float py) {
    if (!sxInside(px, py)) {
        return 0.0;
    }
    return sxDepthAt(uint(px), uint(py));
}

float sxRayZAt(float k0, float k1, float f) { return 1.0 / (k0 + (k1 - k0) * f); }

float3 sxReflect(float3 incident, float3 n) { return sxSub(incident, sxScale(n, 2.0 * sxDot(incident, n))); }

SxHit sxTraceRay(uint64_t sceneColor, SxTrace P, uint x, uint y, float3 direction) {
    SxHit result = sxNoHit();
    if (x >= F.width || y >= F.height || sxDepthAt(x, y) <= 0.0 || sxLength(direction) <= 0.0) {
        return result;
    }
    const float3 p = sxPositionAt(x, y);
    const float3 r = sxNormalize(direction);

    float rayLength = P.maxDistance;
    if (r.z < 0.0) {
        const float maxLen = (p.z - F.nearZ * 1.01) / -r.z;
        rayLength = min(rayLength, maxLen);
    }
    if (rayLength <= 1e-4) {
        return result;
    }
    const float3 e = sxAdd(p, sxScale(r, rayLength));

    float p0x, p0y, p1x, p1y;
    if (!sxProject(p, p0x, p0y) || !sxProject(e, p1x, p1y)) {
        return result;
    }
    const float k0 = 1.0 / p.z;
    const float k1 = 1.0 / e.z;
    const float3 q0 = sxScale(p, k0);
    const float3 q1 = sxScale(e, k1);
    const float ddx = p1x - p0x;
    const float ddy = p1y - p0y;
    const float pixelLength = max(abs(ddx), abs(ddy));
    if (pixelLength < 1e-3) {
        return result;
    }
    const float stride = max(P.stride, pixelLength / float(P.maxSteps));
    const uint stepCount = uint(ceil(pixelLength / stride));
    const float fStep = stride / pixelLength;

    float prevF = 0.0;
    float prevZ = p.z;
    bool hit = false;
    float hitF = 0.0;
    for (uint i = 1u; i <= stepCount; ++i) {
        const float f = min(1.0, float(i) * fStep);
        const float sx = p0x + ddx * f;
        const float sy = p0y + ddy * f;
        if (!sxInside(sx, sy)) {
            break;
        }
        const float rayZ = sxRayZAt(k0, k1, f);
        const float sceneZ = sxDepthNearest(sx, sy);
        if (sceneZ > 0.0 && !(uint(sx) == x && uint(sy) == y)) {
            const float zMin = min(prevZ, rayZ);
            const float zMax = max(prevZ, rayZ);
            if (zMax >= sceneZ && zMin <= sceneZ + P.thickness) {
                float lo = prevF;
                float hi = f;
                for (uint k = 0u; k < P.refineSteps; ++k) {
                    const float mid = 0.5 * (lo + hi);
                    const float midSceneZ = sxDepthNearest(p0x + ddx * mid, p0y + ddy * mid);
                    if (midSceneZ > 0.0 && sxRayZAt(k0, k1, mid) >= midSceneZ) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                hit = true;
                hitF = hi;
                break;
            }
        }
        prevF = f;
        prevZ = rayZ;
    }
    if (!hit) {
        return result;
    }

    result.hit = true;
    result.px = p0x + ddx * hitF;
    result.py = p0y + ddy * hitF;
    const uint hx = min(F.width - 1u, uint(max(0.0, result.px)));
    const uint hy = min(F.height - 1u, uint(max(0.0, result.py)));
    result.color = sxColorAt(sceneColor, sxIndex(hx, hy));

    const float k = k0 + (k1 - k0) * hitF;
    const float3 hitPos = sxScale(sxAdd(q0, sxScale(sxSub(q1, q0), hitF)), 1.0 / k);
    result.distance = sxLength(sxSub(hitPos, p));

    float edgeFade = 1.0;
    if (P.fadeEdge > 0.0) {
        const float u = result.px / float(F.width);
        const float vv = result.py / float(F.height);
        const float edge = min(min(u, 1.0 - u), min(vv, 1.0 - vv));
        edgeFade = sxSaturate(edge / P.fadeEdge);
    }
    const float distanceFade = sxSaturate(4.0 * (1.0 - result.distance / P.maxDistance));
    result.confidence = edgeFade * distanceFade;
    return result;
}

SxHit sxTracePixel(uint64_t sceneColor, SxTrace P, uint x, uint y) {
    if (x >= F.width || y >= F.height || sxDepthAt(x, y) <= 0.0) {
        return sxNoHit();
    }
    const float3 p = sxPositionAt(x, y);
    float3 n = sxNormalize(sxNormalAt(x, y));
    const float3 v = sxNormalize(p);
    if (sxDot(n, v) > 0.0) {
        n = sxNeg(n);
    }
    return sxTraceRay(sceneColor, P, x, y, sxReflect(v, n));
}
