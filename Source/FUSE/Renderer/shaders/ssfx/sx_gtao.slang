// WP-6.3 "ssfx.gtao". Slang twin of sx_gtao.comp (see there).
#include "sx_common.slang"

static const float kSxPi = 3.14159274;
static const float kSxHalfPi = 1.57079637;
static const float kSxTwoPi = 6.28318548;

float sxAcosPoly(float x) {
    const float c = max(-1.0, min(1.0, x));
    const float a = abs(c);
    float p = -0.0012624911;
    p = p * a + 0.0066700899;
    p = p * a - 0.0170881264;
    p = p * a + 0.0308918804;
    p = p * a - 0.0501743034;
    p = p * a + 0.0889789909;
    p = p * a - 0.214598805;
    p = p * a + 1.57079625;
    const float r = sqrt(1.0 - a) * p;
    return c < 0.0 ? kSxPi - r : r;
}

float sxCosPoly(float x) {
    float a = abs(x);
    if (a > kSxPi) {
        a = kSxTwoPi - a;
    }
    float sign_ = 1.0;
    if (a > kSxHalfPi) {
        a = kSxPi - a;
        sign_ = -1.0;
    }
    const float a2 = a * a;
    float p = 2.08767559e-09;
    p = p * a2 - 2.755732e-07;
    p = p * a2 + 2.48015876e-05;
    p = p * a2 - 0.00138888892;
    p = p * a2 + 0.0416666679;
    p = p * a2 - 0.5;
    p = p * a2 + 1.0;
    return sign_ * p;
}

float sxArc(float h, float gamma, float cosN, float sinG) {
    const float twoH = 2.0 * h;
    return 0.25 * (cosN + twoH * sinG - sxCosPoly(twoH - gamma));
}

float sxGtao(uint x, uint y) {
    if (x >= F.width || y >= F.height || sxDepthAt(x, y) <= 0.0) {
        return 1.0;
    }
    const float3 p = sxPositionAt(x, y);
    float3 n = sxNormalize(sxNormalAt(x, y));
    if (sxDot(n, p) > 0.0) {
        n = sxNeg(n);
    }
    const float3 viewVec = sxNormalize(sxNeg(p));

    const float radiusPx = min(F.aoMaxRadiusPx, F.aoRadius * F.fx / p.z);
    if (radiusPx < 1.0) {
        return 1.0;
    }
    const uint steps = max(1u, min(F.aoSteps, uint(radiusPx)));
    const float cxp = float(x) + 0.5;
    const float cyp = float(y) + 0.5;

    float3 viewT, viewB;
    sxTangentBasis(viewVec, viewT, viewB);

    float ra = 1.0;
    float rb = 0.0;
    float offset = 1.0;
    if ((F.flags & kSxFlagAoJitter) != 0u) {
        const uint seed = (x * 73856093u) ^ (y * 19349663u) ^ (F.aoFrame * 83492791u);
        const float u = sxHashUnit(seed) - 0.5;
        const float v = sxHashUnit(seed ^ 0x9e3779b9u) - 0.5;
        const float len = sqrt(u * u + v * v);
        if (len > 1e-4) {
            ra = u / len;
            rb = v / len;
        }
        offset = 1.0 - sxHashUnit(seed ^ 0x85ebca6bu);
    }

    float numerator = 0.0;
    float denominator = 0.0;
    for (uint k = 0u; k < F.aoSlices; ++k) {
        const float c = F.aoSliceCos[k] * ra - F.aoSliceSin[k] * rb;
        const float s = F.aoSliceSin[k] * ra + F.aoSliceCos[k] * rb;
        const float3 d = sxAdd(sxScale(viewT, c), sxScale(viewB, s));
        float sdx = F.fx * (d.x * p.z - p.x * d.z);
        float sdy = F.fy * (d.y * p.z - p.y * d.z);
        const float slen = sqrt(sdx * sdx + sdy * sdy);
        if (slen < 1e-20) {
            continue;
        }
        sdx = sdx / slen;
        sdy = sdy / slen;

        const float3 axis = sxNormalize(sxCross(d, viewVec));
        const float3 projN = sxSub(n, sxScale(axis, sxDot(n, axis)));
        const float projNLen = sxLength(projN);
        if (projNLen < 1e-6) {
            continue;
        }
        const float signN = sxDot(d, projN) >= 0.0 ? 1.0 : -1.0;
        const float cosN = sxSaturate(sxDot(projN, viewVec) / projNLen);
        const float gamma = signN * sxAcosPoly(cosN);
        const float sinG = signN * sqrt(max(0.0, 1.0 - cosN * cosN));

        float lowCos[2] = { -sinG, sinG };
        float horizonCos[2] = { lowCos[0], lowCos[1] };
        for (uint side = 0u; side < 2u; ++side) {
            const float dirX = side == 0u ? sdx : -sdx;
            const float dirY = side == 0u ? sdy : -sdy;
            int lastX = int(x);
            int lastY = int(y);
            for (uint st = 1u; st <= steps; ++st) {
                const float t = radiusPx * (float(st - 1u) + offset) / float(steps);
                const float sx = cxp + dirX * t;
                const float sy = cyp + dirY * t;
                if (!sxInside(sx, sy)) {
                    break;
                }
                const int ix = int(sx);
                const int iy = int(sy);
                if (ix == lastX && iy == lastY) {
                    continue;
                }
                lastX = ix;
                lastY = iy;
                const uint ux = uint(ix);
                const uint uy = uint(iy);
                if (sxDepthAt(ux, uy) <= 0.0) {
                    continue;
                }
                const float3 h = sxSub(sxPositionAt(ux, uy), p);
                const float dist = sxLength(h);
                if (dist < 1e-6 || dist > F.aoRadius) {
                    continue;
                }
                const float3 hn = sxScale(h, 1.0 / dist);
                if (sxDot(hn, n) <= F.aoBias) {
                    continue;
                }
                float hc = sxDot(hn, viewVec);
                if (F.aoFalloff > 0.0) {
                    const float w = sxSaturate((F.aoRadius - dist) / (F.aoFalloff * F.aoRadius));
                    hc = lowCos[side] + (hc - lowCos[side]) * w;
                }
                horizonCos[side] = max(horizonCos[side], hc);
            }
        }

        float h1 = gamma + kSxHalfPi;
        float h0 = gamma - kSxHalfPi;
        if (horizonCos[0] > lowCos[0]) {
            h1 = gamma + max(-kSxHalfPi, min(kSxHalfPi, sxAcosPoly(horizonCos[0]) - gamma));
        }
        if (horizonCos[1] > lowCos[1]) {
            h0 = gamma + max(-kSxHalfPi, min(kSxHalfPi, -sxAcosPoly(horizonCos[1]) - gamma));
        }
        numerator = numerator + projNLen * (sxArc(h0, gamma, cosN, sinG) + sxArc(h1, gamma, cosN, sinG));
        denominator = denominator +
                      projNLen * (sxArc(gamma - kSxHalfPi, gamma, cosN, sinG) + sxArc(gamma + kSxHalfPi, gamma, cosN, sinG));
    }
    if (!(denominator > 0.0)) {
        return 1.0;
    }
    const float visibility = sxSaturate(numerator / denominator);
    return F.aoStrength == 1.0 ? visibility : pow(visibility, F.aoStrength);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    sxFloats(F.ao)[sxIndex(p.x, p.y)] = sxGtao(p.x, p.y);
}
