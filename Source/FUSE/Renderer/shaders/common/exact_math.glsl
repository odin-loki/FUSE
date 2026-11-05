// Correctly rounded f32 division and square root, shared by every renderer kernel that must return a CPU
// reference's bits (light tree, ReSTIR, path tracer, screen-space effects, SSRC, temporal, gsplat, atmosphere,
// visibility-buffer decode, DDGI, G-buffer normal encode). Slang twin: exact_math.slang (fuseDivRn / fuseSqrtRn).
//
// Vulkan guarantees correct rounding only for add / sub / mul: OpFDiv may be 2.5 ulp off and sqrt is inherited
// from inversesqrt. NVIDIA uses that slack (RTX 3090: 1-2 ulp, ~250 ulp after cancellation downstream), so a plain
// `/` or `sqrt` differs from the CPU's IEEE result in the last bits and every discrete decision on it (a branch, a
// floor() texel / tile choice, a rejection test) can flip. fuse_div_rn / fuse_sqrt_rn return the IEEE
// round-to-nearest result: the hardware value (a few ulp off at most) is stepped to the neighbour on the right
// side of the rounding midpoints, which are compared exactly as 64-bit integers held in 32-bit pairs (a midpoint
// has 25 significant bits, its product with a 24-bit mantissa fits in 50 bits; a quotient or a square root of
// floats is never exactly a midpoint, so there are no ties). Only integer operations decide, so the result does
// not depend on `precise` / -fp-mode (contraction cannot change it), and no shaderInt64 is needed (the G-buffer
// raster pass uses it on every tier). Where IEEE division / sqrt are already exact (Lavapipe) this is the
// identity. Zero, subnormal, infinite and NaN operands or results keep the hardware value.
//
// Include with a path relative to the including file ("../common/exact_math.glsl"), so no extra -I is needed.
#ifndef FUSE_EXACT_MATH_GLSL
#define FUSE_EXACT_MATH_GLSL

bool fuse_rn_is_normal(float x) {
    const uint e = (floatBitsToUint(x) >> 23u) & 0xFFu;
    return e != 0u && e != 0xFFu;
}

// |x| = m * 2^e (x normal): m in [2^23, 2^24).
uint fuse_rn_mantissa(float x) { return (floatBitsToUint(x) & 0x7FFFFFu) | 0x800000u; }
int fuse_rn_exponent(float x) { return int((floatBitsToUint(x) >> 23u) & 0xFFu) - 150; }

// a * b as a 64-bit (hi, lo) pair, a < 2^26, b < 2^26, in 32-bit integers (no shaderInt64): 16-bit halves,
// a = ah 2^16 + al, b = bh 2^16 + bl; the cross sum is below 2^28.
uvec2 fuse_rn_mul(uint a, uint b) {
    const uint ah = a >> 16u, al = a & 0xFFFFu;
    const uint bh = b >> 16u, bl = b & 0xFFFFu;
    const uint cross = ah * bl + al * bh;
    const uint low = al * bl;
    const uint lo = low + (cross << 16u);
    const uint carry = lo < low ? 1u : 0u;
    return uvec2(ah * bh + (cross >> 16u) + carry, lo);
}

// sign(ma * 2^ea - p * 2^ep) for ma in [2^23, 2^24), p = (hi, lo) in [2^46, 2^52).
int fuse_rn_compare_scaled(uint ma, int ea, uvec2 p, int ep) {
    const int k = ea - ep;
    if (k < 0) {
        return -1; // p * 2^-k >= p >= 2^46 > ma
    }
    if (k > 40) {
        return 1; // ma * 2^k >= 2^64 > p
    }
    uvec2 lhs; // ma << k
    if (k >= 32) {
        lhs = uvec2(ma << uint(k - 32), 0u);
    } else if (k == 0) {
        lhs = uvec2(0u, ma);
    } else {
        lhs = uvec2(ma >> uint(32 - k), ma << uint(k));
    }
    if (lhs.x != p.x) {
        return lhs.x > p.x ? 1 : -1;
    }
    return lhs.y > p.y ? 1 : (lhs.y < p.y ? -1 : 0);
}

// The rounding midpoints next to q (normal, > 0): (m, e) with midpoint = m * 2^e, m < 2^26.
void fuse_rn_mid_up(float q, out uint m, out int e) {
    m = fuse_rn_mantissa(q) * 2u + 1u;
    e = fuse_rn_exponent(q) - 1;
}
void fuse_rn_mid_down(float q, out uint m, out int e) {
    const uint mq = fuse_rn_mantissa(q);
    if (mq == 0x800000u) { // power of two: the float below is half an ulp closer
        m = mq * 4u - 1u;
        e = fuse_rn_exponent(q) - 2;
    } else {
        m = mq * 2u - 1u;
        e = fuse_rn_exponent(q) - 1;
    }
}

// One stepping decision of fuse_div_rn / fuse_sqrt_rn: +1 = round up, -1 = round down, 0 = q is the IEEE result.
// The exact decisions compare |a| (x) with the midpoints next to q (s) as 64-bit integers (fuse_rn_mul +
// fuse_rn_compare_scaled, ~60 integer operations per step). The fast step decides from the residual
//   div:  D = ma 2^k - 2 mq mb,  k = ea - eq - eb + 1   (up: D - mb > 0; down: D + mb < 0, power of two 2D + mb < 0)
//   sqrt: D = mx 2^k - (2 ms)^2, k = ex - 2 es + 2       (up: D - 4 ms - 1 > 0; down: D + 4 ms - 1 < 0)
// which is the same sign test term for term (the midpoint products expanded), held in 32 bits: D is computed
// modulo 2^32 (wrapping shift / multiply) and is exact whenever |D| < 2^30. That is proven per call by a float
// estimate of D: the only rounded operation is the product (exact value < 2^50, error <= 2^25; a contracted FMA
// is closer), so |estimate| < 2^29 (div) / 2^30 (sqrt) bounds |D| below 2^30 + 2^26 and none of the int32 sums
// overflow. When the estimate is out of range (q many ulp off, or k outside [0, 31]) or q is a power of two
// (sqrt), the step falls back to the 64-bit compare, so every decision is the exact one and the result is
// unchanged bit for bit. At the IEEE result |D| <= mb (div) / 4 ms (sqrt): the fast step decides there always
// (CPU sweep of 8.4e7 decisions at -6..+6 ulp: 0 mismatches with the 64-bit compare, 99.8 % decided fast).
// RTX 3090: light.shade 2.57 -> 0.9 ms, taau.resolve 2.09 -> 0.7 ms (docs/unification/RENDERER-HW-RUN.md).
const int FUSE_RN_UNDECIDED = 2;

float fuse_rn_pow2(int k) { return uintBitsToFloat(uint(k + 127) << 23u); }

int fuse_rn_div_step(uint ma, int ea, uint mb, int eb, float q) {
    const uint mq = fuse_rn_mantissa(q);
    const int k = ea - fuse_rn_exponent(q) - eb + 1;
    if (k >= 0 && k <= 31) {
        const float estimate = float(ma) * fuse_rn_pow2(k) - float(2u * mq) * float(mb);
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
    fuse_rn_mid_up(q, m, e);
    if (fuse_rn_compare_scaled(ma, ea, fuse_rn_mul(m, mb), e + eb) > 0) { // |a| > mid * |b|: round up
        return 1;
    }
    fuse_rn_mid_down(q, m, e);
    return fuse_rn_compare_scaled(ma, ea, fuse_rn_mul(m, mb), e + eb) < 0 ? -1 : 0;
}

int fuse_rn_sqrt_step(uint mx, int ex, float s) {
    const uint ms = fuse_rn_mantissa(s);
    const int k = ex - 2 * fuse_rn_exponent(s) + 2;
    if (ms != 0x800000u && k >= 0 && k <= 31) {
        const float t = float(2u * ms);
        const float estimate = float(mx) * fuse_rn_pow2(k) - t * t;
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
    fuse_rn_mid_up(s, m, e);
    if (fuse_rn_compare_scaled(mx, ex, fuse_rn_mul(m, m), 2 * e) > 0) { // x > mid^2: round up
        return 1;
    }
    fuse_rn_mid_down(s, m, e);
    return fuse_rn_compare_scaled(mx, ex, fuse_rn_mul(m, m), 2 * e) < 0 ? -1 : 0;
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
    const uint mb = fuse_rn_mantissa(b);
    const int eb = fuse_rn_exponent(b);
    for (uint i = 0u; i < 8u; ++i) {
        const int step = fuse_rn_div_step(ma, ea, mb, eb, q);
        if (step == 0) {
            break;
        }
        q = uintBitsToFloat(uint(int(floatBitsToUint(q)) + step));
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
        const int step = fuse_rn_sqrt_step(mx, ex, s);
        if (step == 0) {
            break;
        }
        s = uintBitsToFloat(uint(int(floatBitsToUint(s)) + step));
    }
    return s;
}

#endif // FUSE_EXACT_MATH_GLSL
