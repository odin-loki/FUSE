// Correctly rounded f32 division and square root: Slang twin of exact_math.glsl (see there). fuseDivRn /
// fuseSqrtRn return the IEEE round-to-nearest quotient / root on any conformant device; only integer operations
// decide, so the result does not depend on -fp-mode. Include with a path relative to the including file
// ("../common/exact_math.slang").
#pragma once

bool fuseRnIsNormal(float x) {
    const uint e = (asuint(x) >> 23u) & 0xFFu;
    return e != 0u && e != 0xFFu;
}

// |x| = m * 2^e (x normal): m in [2^23, 2^24).
uint fuseRnMantissa(float x) { return (asuint(x) & 0x7FFFFFu) | 0x800000u; }
int fuseRnExponent(float x) { return int((asuint(x) >> 23u) & 0xFFu) - 150; }

// sign(ma * 2^ea - p * 2^ep) for ma in [2^23, 2^24), p in [2^46, 2^52).
int fuseRnCompareScaled(uint ma, int ea, uint64_t p, int ep) {
    const int k = ea - ep;
    if (k < 0) {
        return -1; // p * 2^-k >= p >= 2^46 > ma
    }
    if (k > 40) {
        return 1; // ma * 2^k >= 2^64 > p
    }
    const uint64_t lhs = uint64_t(ma) << uint(k);
    return lhs > p ? 1 : (lhs < p ? -1 : 0);
}

// The rounding midpoints next to q (normal, > 0): (m, e) with midpoint = m * 2^e, m < 2^26.
void fuseRnMidUp(float q, out uint64_t m, out int e) {
    m = uint64_t(fuseRnMantissa(q)) * 2ull + 1ull;
    e = fuseRnExponent(q) - 1;
}
void fuseRnMidDown(float q, out uint64_t m, out int e) {
    const uint mq = fuseRnMantissa(q);
    if (mq == 0x800000u) { // power of two: the float below is half an ulp closer
        m = uint64_t(mq) * 4ull - 1ull;
        e = fuseRnExponent(q) - 2;
    } else {
        m = uint64_t(mq) * 2ull - 1ull;
        e = fuseRnExponent(q) - 1;
    }
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
    const uint64_t mb = uint64_t(fuseRnMantissa(b));
    const int eb = fuseRnExponent(b);
    for (uint i = 0u; i < 8u; ++i) {
        uint64_t m;
        int e;
        fuseRnMidUp(q, m, e);
        if (fuseRnCompareScaled(ma, ea, m * mb, e + eb) > 0) { // |a| > mid * |b|: round up
            q = asfloat(asuint(q) + 1u);
        } else {
            fuseRnMidDown(q, m, e);
            if (fuseRnCompareScaled(ma, ea, m * mb, e + eb) < 0) {
                q = asfloat(asuint(q) - 1u);
            } else {
                break;
            }
        }
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
        uint64_t m;
        int e;
        fuseRnMidUp(s, m, e);
        if (fuseRnCompareScaled(mx, ex, m * m, 2 * e) > 0) { // x > mid^2: round up
            s = asfloat(asuint(s) + 1u);
        } else {
            fuseRnMidDown(s, m, e);
            if (fuseRnCompareScaled(mx, ex, m * m, 2 * e) < 0) {
                s = asfloat(asuint(s) - 1u);
            } else {
                break;
            }
        }
    }
    return s;
}
