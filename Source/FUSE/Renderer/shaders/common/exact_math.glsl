// Correctly rounded f32 division and square root, shared by every renderer kernel that must return a CPU
// reference's bits (light tree, ReSTIR, path tracer, screen-space effects, SSRC, temporal, gsplat, atmosphere,
// visibility-buffer decode, DDGI). Slang twin: exact_math.slang (fuseDivRn / fuseSqrtRn).
//
// Vulkan guarantees correct rounding only for add / sub / mul: OpFDiv may be 2.5 ulp off and sqrt is inherited
// from inversesqrt. NVIDIA uses that slack (RTX 3090: 1-2 ulp, ~250 ulp after cancellation downstream), so a plain
// `/` or `sqrt` differs from the CPU's IEEE result in the last bits and every discrete decision on it (a branch, a
// floor() texel / tile choice, a rejection test) can flip. fuse_div_rn / fuse_sqrt_rn return the IEEE
// round-to-nearest result: the hardware value (a few ulp off at most) is stepped to the neighbour on the right
// side of the rounding midpoints, which are compared exactly in 64-bit integers (a midpoint has 25 significant
// bits, its product with a 24-bit mantissa fits in 50 bits; a quotient or a square root of floats is never
// exactly a midpoint, so there are no ties). Only integer operations decide, so the result does not depend on
// `precise` / -fp-mode (contraction cannot change it). Where IEEE division / sqrt are already exact (Lavapipe)
// this is the identity. Zero, subnormal, infinite and NaN operands or results keep the hardware value.
//
// Include with a path relative to the including file ("../common/exact_math.glsl"), so no extra -I is needed.
#ifndef FUSE_EXACT_MATH_GLSL
#define FUSE_EXACT_MATH_GLSL
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

bool fuse_rn_is_normal(float x) {
    const uint e = (floatBitsToUint(x) >> 23u) & 0xFFu;
    return e != 0u && e != 0xFFu;
}

// |x| = m * 2^e (x normal): m in [2^23, 2^24).
uint fuse_rn_mantissa(float x) { return (floatBitsToUint(x) & 0x7FFFFFu) | 0x800000u; }
int fuse_rn_exponent(float x) { return int((floatBitsToUint(x) >> 23u) & 0xFFu) - 150; }

// sign(ma * 2^ea - p * 2^ep) for ma in [2^23, 2^24), p in [2^46, 2^52).
int fuse_rn_compare_scaled(uint ma, int ea, uint64_t p, int ep) {
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
void fuse_rn_mid_up(float q, out uint64_t m, out int e) {
    m = uint64_t(fuse_rn_mantissa(q)) * 2ul + 1ul;
    e = fuse_rn_exponent(q) - 1;
}
void fuse_rn_mid_down(float q, out uint64_t m, out int e) {
    const uint mq = fuse_rn_mantissa(q);
    if (mq == 0x800000u) { // power of two: the float below is half an ulp closer
        m = uint64_t(mq) * 4ul - 1ul;
        e = fuse_rn_exponent(q) - 2;
    } else {
        m = uint64_t(mq) * 2ul - 1ul;
        e = fuse_rn_exponent(q) - 1;
    }
}

float fuse_div_rn(float a, float b) {
    const float q0 = a / b;
    if (!fuse_rn_is_normal(a) || !fuse_rn_is_normal(b) || !fuse_rn_is_normal(q0)) {
        return q0;
    }
    const uint sign = floatBitsToUint(q0) & 0x80000000u;
    float q = abs(q0);
    const uint ma = fuse_rn_mantissa(a);
    const int ea = fuse_rn_exponent(a);
    const uint64_t mb = uint64_t(fuse_rn_mantissa(b));
    const int eb = fuse_rn_exponent(b);
    for (uint i = 0u; i < 8u; ++i) {
        uint64_t m;
        int e;
        fuse_rn_mid_up(q, m, e);
        if (fuse_rn_compare_scaled(ma, ea, m * mb, e + eb) > 0) { // |a| > mid * |b|: round up
            q = uintBitsToFloat(floatBitsToUint(q) + 1u);
        } else {
            fuse_rn_mid_down(q, m, e);
            if (fuse_rn_compare_scaled(ma, ea, m * mb, e + eb) < 0) {
                q = uintBitsToFloat(floatBitsToUint(q) - 1u);
            } else {
                break;
            }
        }
        if (!fuse_rn_is_normal(q)) {
            return q0;
        }
    }
    return uintBitsToFloat(floatBitsToUint(q) | sign);
}

float fuse_sqrt_rn(float x) {
    const float s0 = sqrt(x);
    if (!(x > 0.0) || !fuse_rn_is_normal(x) || !fuse_rn_is_normal(s0)) {
        return s0;
    }
    float s = s0;
    const uint mx = fuse_rn_mantissa(x);
    const int ex = fuse_rn_exponent(x);
    for (uint i = 0u; i < 8u; ++i) {
        uint64_t m;
        int e;
        fuse_rn_mid_up(s, m, e);
        if (fuse_rn_compare_scaled(mx, ex, m * m, 2 * e) > 0) { // x > mid^2: round up
            s = uintBitsToFloat(floatBitsToUint(s) + 1u);
        } else {
            fuse_rn_mid_down(s, m, e);
            if (fuse_rn_compare_scaled(mx, ex, m * m, 2 * e) < 0) {
                s = uintBitsToFloat(floatBitsToUint(s) - 1u);
            } else {
                break;
            }
        }
    }
    return s;
}

#endif // FUSE_EXACT_MATH_GLSL
