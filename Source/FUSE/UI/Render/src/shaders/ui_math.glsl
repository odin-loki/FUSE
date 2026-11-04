// FUSE UI shared shader math (included by ui_effect.comp.in and ui_sdf_quad.frag; ui_draw_list.comp keeps its own copy).
// The CPU reference uses IEEE single precision with correctly rounded division and sqrt; Vulkan only guarantees that
// for add / sub / mul. divRn / sqrtRn round the hardware result to the IEEE one with an exact residual (Dekker
// product; needs only correctly rounded mul / add and no contraction, hence `precise`).
#ifndef FUSE_UI_MATH_GLSL
#define FUSE_UI_MATH_GLSL

float resid(float a, float q, float b) { // a - q * b, accurate enough to compare neighbours
    precise float p = q * b;
    precise float cq = 4097.0 * q;
    precise float qh = cq - (cq - q);
    precise float ql = q - qh;
    precise float cb = 4097.0 * b;
    precise float bh = cb - (cb - b);
    precise float bl = b - bh;
    precise float e = ((qh * bh - p) + qh * bl + ql * bh) + ql * bl;
    precise float r = (a - p) - e;
    return r;
}
float nextToward(float q, bool up) { // q != 0; up: toward +inf
    const uint bits = floatBitsToUint(q);
    return uintBitsToFloat((q > 0.0) == up ? bits + 1u : bits - 1u);
}
float roundRn(float a, float q, float b, bool root) { // walk q to the nearest representable a / b (root: sqrt(a), b = q)
    float r = resid(a, q, root ? q : b);
    for (int i = 0; i < 4 && r != 0.0; ++i) {
        const float qn = nextToward(q, root ? r > 0.0 : (r > 0.0) == (b > 0.0));
        const float rn = resid(a, qn, root ? qn : b);
        if (abs(rn) < abs(r) || (abs(rn) == abs(r) && (floatBitsToUint(q) & 1u) != 0u)) {
            q = qn;
            r = rn;
        } else {
            break;
        }
    }
    return q;
}
float divRn(float a, float b) {
    precise float q = a / b;
    if (q == 0.0 || isinf(q) || isnan(q)) return q;
    return roundRn(a, q, b, false);
}
float sqrtRn(float x) {
    precise float s = sqrt(x);
    if (!(x > 0.0) || isinf(x)) return s;
    return roundRn(x, s, s, true);
}

float sdRoundRect(float px, float py, vec4 r, vec4 rad) {
    precise float cx = r.x + r.z * 0.5;
    precise float cy = r.y + r.w * 0.5;
    precise float hx = r.z * 0.5;
    precise float hy = r.w * 0.5;
    precise float dx = px - cx;
    precise float dy = py - cy;
    float rr = dx < 0.0 ? (dy < 0.0 ? rad.x : rad.w) : (dy < 0.0 ? rad.y : rad.z);
    rr = min(rr, min(hx, hy));
    precise float qx = abs(dx) - hx + rr;
    precise float qy = abs(dy) - hy + rr;
    precise float ox = max(qx, 0.0);
    precise float oy = max(qy, 0.0);
    precise float l2 = ox * ox + oy * oy;
    precise float d = sqrtRn(l2) + min(max(qx, qy), 0.0) - rr;
    return d;
}

float lroundPos(float v) { // std::lround for v >= -0.5 (halves away from zero)
    const float f = floor(v);
    precise float frac = v - f;
    return frac >= 0.5 ? f + 1.0 : f;
}

// fx::sin / fx::cos / fx::hash (Core/include/fuse/ui/effects.hpp): same constants (as bits) and operation order.
#define FX_TWO_PI uintBitsToFloat(0x40c90fdbu)
float fx_sin(float x) {
    precise float t = x * uintBitsToFloat(0x3e22f983u) + 0.5;
    const float k = floor(t);
    precise float r0 = x - k * uintBitsToFloat(0x40c90000u);
    precise float r = r0 - k * uintBitsToFloat(0x3afdaa22u);
    if (r > uintBitsToFloat(0x3fc90fdbu)) {
        precise float f = uintBitsToFloat(0x40490fdbu) - r;
        r = f;
    }
    if (r < -uintBitsToFloat(0x3fc90fdbu)) {
        precise float f = -uintBitsToFloat(0x40490fdbu) - r;
        r = f;
    }
    precise float r2 = r * r;
    precise float p = uintBitsToFloat(0xb2d7322bu);
    p = p * r2 + uintBitsToFloat(0x3638ef1du);
    p = p * r2 + uintBitsToFloat(0xb9500d01u);
    p = p * r2 + uintBitsToFloat(0x3c088889u);
    p = p * r2 + uintBitsToFloat(0xbe2aaaabu);
    precise float s = r + (r * r2) * p;
    return s;
}
float fx_cos(float x) {
    precise float y = x + uintBitsToFloat(0x3fc90fdbu);
    return fx_sin(y);
}
uint fx_hash(uint x, uint y, uint z) {
    uint h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (z * 0xcb1ab31fu);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
float fx_hash01(uint x, uint y, uint z) { return float(fx_hash(x, y, z) >> 8) * 5.9604644775390625e-8; }

#endif
