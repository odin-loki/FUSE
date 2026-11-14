// Correctly rounded f32 division and square root: Slang twin of exact_math.glsl (see there). fuseDivRn /
// fuseSqrtRn return the IEEE round-to-nearest quotient / root on any conformant device; only integer operations
// decide (64-bit products in 32-bit pairs, no Int64 capability), so the result does not depend on -fp-mode.
// Include with a path relative to the including file ("../common/exact_math.slang").
#pragma once

bool fuseRnIsNormal(float x) {
    const uint e = (asuint(x) >> 23u) & 0xFFu;
    return e != 0u && e != 0xFFu;
}

// |x| = m * 2^e (x normal): m in [2^23, 2^24).
uint fuseRnMantissa(float x) { return (asuint(x) & 0x7FFFFFu) | 0x800000u; }
int fuseRnExponent(float x) { return int((asuint(x) >> 23u) & 0xFFu) - 150; }

// a * b as a 64-bit (hi, lo) pair, a < 2^26, b < 2^26, in 32-bit integers (no Int64 capability): 16-bit halves.
uint2 fuseRnMul(uint a, uint b) {
    const uint ah = a >> 16u, al = a & 0xFFFFu;
    const uint bh = b >> 16u, bl = b & 0xFFFFu;
    const uint cross = ah * bl + al * bh;
    const uint low = al * bl;
    const uint lo = low + (cross << 16u);
    const uint carry = lo < low ? 1u : 0u;
    return uint2(ah * bh + (cross >> 16u) + carry, lo);
}

// sign(ma * 2^ea - p * 2^ep) for ma in [2^23, 2^24), p = (hi, lo) in [2^46, 2^52).
int fuseRnCompareScaled(uint ma, int ea, uint2 p, int ep) {
    const int k = ea - ep;
    if (k < 0) {
        return -1; // p * 2^-k >= p >= 2^46 > ma
    }
    if (k > 40) {
        return 1; // ma * 2^k >= 2^64 > p
    }
    uint2 lhs; // ma << k
    if (k >= 32) {
        lhs = uint2(ma << uint(k - 32), 0u);
    } else if (k == 0) {
        lhs = uint2(0u, ma);
    } else {
        lhs = uint2(ma >> uint(32 - k), ma << uint(k));
    }
    if (lhs.x != p.x) {
        return lhs.x > p.x ? 1 : -1;
    }
    return lhs.y > p.y ? 1 : (lhs.y < p.y ? -1 : 0);
}

// The rounding midpoints next to q (normal, > 0): (m, e) with midpoint = m * 2^e, m < 2^26.
void fuseRnMidUp(float q, out uint m, out int e) {
    m = fuseRnMantissa(q) * 2u + 1u;
    e = fuseRnExponent(q) - 1;
}
void fuseRnMidDown(float q, out uint m, out int e) {
    const uint mq = fuseRnMantissa(q);
    if (mq == 0x800000u) { // power of two: the float below is half an ulp closer
        m = mq * 4u - 1u;
        e = fuseRnExponent(q) - 2;
    } else {
        m = mq * 2u - 1u;
        e = fuseRnExponent(q) - 1;
    }
}

// One stepping decision (+1 up, -1 down, 0 = q is the IEEE result): the 32-bit residual fast step, proven exact
// per call by a float estimate, else the 64-bit midpoint compare. Twin of fuse_rn_div_step / fuse_rn_sqrt_step
// (exact_math.glsl, where the derivation and the bounds are written out).
float fuseRnPow2(int k) { return asfloat(uint(k + 127) << 23u); }

int fuseRnDivStep(uint ma, int ea, uint mb, int eb, float q) {
    const uint mq = fuseRnMantissa(q);
    const int k = ea - fuseRnExponent(q) - eb + 1;
    if (k >= 0 && k <= 31) {
        const float estimate = float(ma) * fuseRnPow2(k) - float(2u * mq) * float(mb);
        if (abs(estimate) < 536870912.0) { // 2^29
            const int d = int((ma << uint(k)) - (2u * mq) * mb);
            const int b = int(mb);
            if (d - b > 0) {
                return 1;
            }
            return (mq == 0x800000u ? (2 * d + b < 0) : (d + b < 0)) ? -1 : 0;
        }
    }
    uint m;
    int e;
    fuseRnMidUp(q, m, e);
    if (fuseRnCompareScaled(ma, ea, fuseRnMul(m, mb), e + eb) > 0) { // |a| > mid * |b|: round up
        return 1;
    }
    fuseRnMidDown(q, m, e);
    return fuseRnCompareScaled(ma, ea, fuseRnMul(m, mb), e + eb) < 0 ? -1 : 0;
}

int fuseRnSqrtStep(uint mx, int ex, float s) {
    const uint ms = fuseRnMantissa(s);
    const int k = ex - 2 * fuseRnExponent(s) + 2;
    if (ms != 0x800000u && k >= 0 && k <= 31) {
        const float t = float(2u * ms);
        const float estimate = float(mx) * fuseRnPow2(k) - t * t;
        if (abs(estimate) < 1073741824.0) { // 2^30
            const int d = int((mx << uint(k)) - (2u * ms) * (2u * ms));
            const int m4 = int(4u * ms);
            if (d - m4 - 1 > 0) {
                return 1;
            }
            return d + m4 - 1 < 0 ? -1 : 0;
        }
    }
    uint m;
    int e;
    fuseRnMidUp(s, m, e);
    if (fuseRnCompareScaled(mx, ex, fuseRnMul(m, m), 2 * e) > 0) { // x > mid^2: round up
        return 1;
    }
    fuseRnMidDown(s, m, e);
    return fuseRnCompareScaled(mx, ex, fuseRnMul(m, m), 2 * e) < 0 ? -1 : 0;
}

float fuseDivRn(float a, float b) {
    const float q0 = a / b;
    if (!fuseRnIsNormal(a) || !fuseRnIsNormal(b) || !fuseRnIsNormal(q0)) {
        return q0;
    }
    const uint sign = asuint(q0) & 0x80000000u;
    float q = abs(q0);
    const uint ma = fuseRnMantissa(a);
    const int ea = fuseRnExponent(a);
    const uint mb = fuseRnMantissa(b);
    const int eb = fuseRnExponent(b);
    for (uint i = 0u; i < 8u; ++i) {
        const int step = fuseRnDivStep(ma, ea, mb, eb, q);
        if (step == 0) {
            break;
        }
        q = asfloat(uint(int(asuint(q)) + step));
        if (!fuseRnIsNormal(q)) {
            return q0;
        }
    }
    return asfloat(asuint(q) | sign);
}

float fuseSqrtRn(float x) {
    const float s0 = sqrt(x);
    if (!(x > 0.0) || !fuseRnIsNormal(x) || !fuseRnIsNormal(s0)) {
        return s0;
    }
    float s = s0;
    const uint mx = fuseRnMantissa(x);
    const int ex = fuseRnExponent(x);
    for (uint i = 0u; i < 8u; ++i) {
        const int step = fuseRnSqrtStep(mx, ex, s);
        if (step == 0) {
            break;
        }
        s = asfloat(uint(int(asuint(s)) + step));
    }
    return s;
}
