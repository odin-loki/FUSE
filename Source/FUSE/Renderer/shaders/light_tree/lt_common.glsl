// WP-7.1 light tree: records and the sampler, GLSL twin of lt_common.slang and a line-for-line port of
// include/fuse/renderer/light_tree/light_tree_kernel.hpp (the CPU sampler). The C++ mirror of the records is
// light_tree_types.hpp (fuse_rp_light_tree_layout checks names, order and offsets).
//
// Consumers (WP-7.2 ReSTIR, Relight RL-4.4) include this file and call
//   LtSampleResult s = lt_sample(headerAddress, p, n, u0, u1, u2);
//   float pmf = lt_pmf(headerAddress, p, n, light);
// with headerAddress = LightTreeGpu::headerAddress() (declare LightTreeGraphRefs::tree / range StorageRead).
//
// Every arithmetic result is held in a `precise` variable so no multiply-add is contracted: the sampler keeps
// the CPU kernel's IEEE operations and order and returns the same bits.
#ifndef FUSE_LT_COMMON_GLSL
#define FUSE_LT_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define LT_INVALID 0xFFFFFFFFu
#define LT_MAX_DEPTH 31u
#define LT_ONE_MINUS_EPSILON 0.99999994
#define LT_MIN_DISTANCE2 1e-12

#define LT_KIND_NONE 0u
#define LT_KIND_POINT 1u
#define LT_KIND_SPOT 2u
#define LT_KIND_RECT 3u
#define LT_KIND_DISK 4u
#define LT_KIND_TRIANGLE 5u
#define LT_KIND_DIRECTIONAL 6u

#define LT_FLAG_LEAF 1u
#define LT_FLAG_TWO_SIDED 2u

// LightTreeNode, 64 bytes.
struct LtNode {
    float boundsMin[3];
    float phi;
    float boundsMax[3];
    uint childOrEmitter;
    float axis[3];
    float cosThetaO;
    float cosThetaE;
    float sinThetaO;
    uint flags;
    uint depth;
};

// LightTreeEmitter, 80 bytes.
struct LtEmitter {
    float p0[3];
    uint kind;
    float e1[3];
    float area;
    float e2[3];
    uint source;
    float normal[3];
    uint flags;
    uint leafNode;
    uint bitTrail;
    uint depth;
    float power;
};

// LightTreeHeader, 48 bytes.
struct LtHeader {
    uint64_t nodes;
    uint64_t emitters;
    uint64_t directional;
    uint nodeCount;
    uint emitterCount;
    uint directionalCount;
    uint version;
    uint reserved[2];
};

// LightTreeQuery, 48 bytes.
struct LtQuery {
    float position[3];
    float u0;
    float normal[3];
    float u1;
    float u2;
    uint evalLight;
    uint reserved[2];
};

// LightTreeSample, 32 bytes.
struct LtSampleResult {
    uint light;
    float pmf;
    float pdfArea;
    uint kind;
    float position[3];
    float evalPmf;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LtHeaderRef { LtHeader h; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LtNodesRef { LtNode v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LtEmittersRef { LtEmitter v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer LtU32Ref { uint v[]; };

float lt_min(float a, float b) { return b < a ? b : a; }
float lt_max(float a, float b) { return a < b ? b : a; }
float lt_safe_sqrt(float x) {
    precise float r = sqrt(lt_max(x, 0.0));
    return r;
}

float lt_cos_sub_clamped(float sinA, float cosA, float sinB, float cosB) {
    if (cosA > cosB) {
        return 1.0;
    }
    precise float x = cosA * cosB;
    precise float y = sinA * sinB;
    precise float r = x + y;
    return r;
}

float lt_sin_sub_clamped(float sinA, float cosA, float sinB, float cosB) {
    if (cosA > cosB) {
        return 0.0;
    }
    precise float x = sinA * cosB;
    precise float y = cosA * sinB;
    precise float r = x - y;
    return r;
}

float lt_importance(LtNode node, vec3 p, vec3 n, bool hasNormal) {
    precise float cx = (node.boundsMin[0] + node.boundsMax[0]) * 0.5;
    precise float cy = (node.boundsMin[1] + node.boundsMax[1]) * 0.5;
    precise float cz = (node.boundsMin[2] + node.boundsMax[2]) * 0.5;
    precise float dx = p.x - cx;
    precise float dy = p.y - cy;
    precise float dz = p.z - cz;
    precise float dist2 = dx * dx + dy * dy + dz * dz;
    precise float gx = node.boundsMax[0] - node.boundsMin[0];
    precise float gy = node.boundsMax[1] - node.boundsMin[1];
    precise float gz = node.boundsMax[2] - node.boundsMin[2];
    precise float radius2 = (gx * gx + gy * gy + gz * gz) * 0.25;
    const float d2 = lt_max(lt_max(dist2, radius2), LT_MIN_DISTANCE2);
    precise float wx = 0.0;
    precise float wy = 0.0;
    precise float wz = 0.0;
    if (dist2 > 0.0) {
        precise float len = sqrt(dist2);
        wx = dx / len;
        wy = dy / len;
        wz = dz / len;
    }
    precise float cosW = node.axis[0] * wx + node.axis[1] * wy + node.axis[2] * wz;
    if ((node.flags & LT_FLAG_TWO_SIDED) != 0u) {
        cosW = abs(cosW);
    }
    precise float oneMinusW = 1.0 - cosW * cosW;
    const float sinW = lt_safe_sqrt(oneMinusW);
    precise float cosB = -1.0;
    precise float sinB = 0.0;
    if (dist2 > radius2) {
        precise float sin2 = radius2 / dist2;
        precise float oneMinusB = 1.0 - sin2;
        cosB = lt_safe_sqrt(oneMinusB);
        sinB = sqrt(sin2);
    }
    const float cosX = lt_cos_sub_clamped(sinW, cosW, node.sinThetaO, node.cosThetaO);
    const float sinX = lt_sin_sub_clamped(sinW, cosW, node.sinThetaO, node.cosThetaO);
    const float cosP = lt_cos_sub_clamped(sinX, cosX, sinB, cosB);
    if (cosP <= node.cosThetaE) {
        return 0.0;
    }
    precise float importance = node.phi * cosP / d2;
    if (hasNormal) {
        precise float cosI = abs(wx * n.x + wy * n.y + wz * n.z);
        precise float oneMinusI = 1.0 - cosI * cosI;
        const float sinI = lt_safe_sqrt(oneMinusI);
        const float cosPI = lt_cos_sub_clamped(sinI, cosI, sinB, cosB);
        importance = importance * cosPI;
    }
    return lt_max(importance, 0.0);
}

void lt_branch(float i0, float i1, out float p0, out float p1) {
    precise float sum = i0 + i1;
    if (sum > 0.0) {
        precise float a = i0 / sum;
        precise float b = i1 / sum;
        p0 = a;
        p1 = b;
    } else {
        p0 = 0.5;
        p1 = 0.5;
    }
}

bool lt_has_normal(vec3 n) { return n.x != 0.0 || n.y != 0.0 || n.z != 0.0; }

float lt_directional_probability(LtHeader t) {
    const uint total = t.directionalCount + (t.nodeCount > 0u ? 1u : 0u);
    if (total == 0u) {
        return 0.0;
    }
    precise float r = float(t.directionalCount) / float(total);
    return r;
}

float lt_sin_poly(float t) {
    precise float t2 = t * t;
    precise float a = t2 * 2.7557319e-06;
    precise float b = t2 * (-0.00019841270 + a);
    precise float c = t2 * (0.0083333333 + b);
    precise float d = t2 * (-0.16666667 + c);
    precise float e = t * d;
    precise float r = t + e;
    return r;
}

float lt_cos_poly(float t) {
    precise float t2 = t * t;
    precise float a = t2 * 2.4801587e-05;
    precise float b = t2 * (-0.0013888889 + a);
    precise float c = t2 * (0.041666668 + b);
    precise float d = t2 * (-0.5 + c);
    precise float r = 1.0 + d;
    return r;
}

void lt_concentric(float u1, float u2, out float x, out float y) {
    precise float a = u1 * 2.0 - 1.0;
    precise float b = u2 * 2.0 - 1.0;
    x = 0.0;
    y = 0.0;
    if (a == 0.0 && b == 0.0) {
        return;
    }
    if (abs(a) > abs(b)) {
        precise float t = (b / a) * 0.78539816;
        precise float rx = a * lt_cos_poly(t);
        precise float ry = a * lt_sin_poly(t);
        x = rx;
        y = ry;
    } else {
        precise float t = (a / b) * 0.78539816;
        precise float rx = b * lt_sin_poly(t);
        precise float ry = b * lt_cos_poly(t);
        x = rx;
        y = ry;
    }
}

void lt_sample_point(LtEmitter e, float u1, float u2, out vec3 position, out float pdfArea) {
    precise float a = 0.0;
    precise float b = 0.0;
    bool surface = true;
    if (e.kind == LT_KIND_TRIANGLE) {
        precise float su = sqrt(u1);
        a = su * (1.0 - u2);
        b = su * u2;
    } else if (e.kind == LT_KIND_RECT) {
        a = u1 * 2.0 - 1.0;
        b = u2 * 2.0 - 1.0;
    } else if (e.kind == LT_KIND_DISK) {
        float ca;
        float cb;
        lt_concentric(u1, u2, ca, cb);
        a = ca;
        b = cb;
    } else {
        surface = false;
    }
    if (!surface) {
        const bool directional = e.kind == LT_KIND_DIRECTIONAL;
        position = directional ? vec3(e.normal[0], e.normal[1], e.normal[2]) : vec3(e.p0[0], e.p0[1], e.p0[2]);
        pdfArea = 0.0;
        return;
    }
    precise float px = e.p0[0] + e.e1[0] * a + e.e2[0] * b;
    precise float py = e.p0[1] + e.e1[1] * a + e.e2[1] * b;
    precise float pz = e.p0[2] + e.e1[2] * a + e.e2[2] * b;
    position = vec3(px, py, pz);
    precise float inv = 1.0 / e.area;
    pdfArea = e.area > 0.0 ? inv : 0.0;
}

LtSampleResult lt_sample(uint64_t headerAddress, vec3 p, vec3 n, float u0, float u1, float u2) {
    LtSampleResult s;
    s.light = LT_INVALID;
    s.pmf = 0.0;
    s.pdfArea = 0.0;
    s.kind = LT_KIND_NONE;
    s.position[0] = 0.0;
    s.position[1] = 0.0;
    s.position[2] = 0.0;
    s.evalPmf = 0.0;
    const LtHeader t = LtHeaderRef(headerAddress).h;
    const float pDir = lt_directional_probability(t);
    if (t.directionalCount == 0u && t.nodeCount == 0u) {
        return s;
    }
    const bool hasNormal = lt_has_normal(n);
    LtNodesRef nodes = LtNodesRef(t.nodes);
    precise float u = u0;
    uint light = LT_INVALID;
    precise float pmf = 0.0;
    if (u < pDir) {
        precise float q = u / pDir;
        u = lt_min(q, LT_ONE_MINUS_EPSILON);
        precise float scaled = u * float(t.directionalCount);
        uint k = uint(scaled);
        if (k >= t.directionalCount) {
            k = t.directionalCount - 1u;
        }
        light = LtU32Ref(t.directional).v[k];
        pmf = pDir / float(t.directionalCount);
    } else {
        precise float pTree = 1.0 - pDir;
        precise float q = (u - pDir) / pTree;
        u = lt_min(q, LT_ONE_MINUS_EPSILON);
        pmf = pTree;
        uint node = 0u;
        for (uint level = 0u; level <= LT_MAX_DEPTH; ++level) {
            const LtNode current = nodes.v[node];
            if ((current.flags & LT_FLAG_LEAF) != 0u) {
                light = current.childOrEmitter;
                break;
            }
            const uint c0 = node + 1u;
            const uint c1 = current.childOrEmitter;
            const float i0 = lt_importance(nodes.v[c0], p, n, hasNormal);
            const float i1 = lt_importance(nodes.v[c1], p, n, hasNormal);
            float p0;
            float p1;
            lt_branch(i0, i1, p0, p1);
            if (u < p0) {
                precise float r = u / p0;
                u = lt_min(r, LT_ONE_MINUS_EPSILON);
                pmf = pmf * p0;
                node = c0;
            } else {
                precise float r = (u - p0) / p1;
                u = lt_min(r, LT_ONE_MINUS_EPSILON);
                pmf = pmf * p1;
                node = c1;
            }
        }
    }
    if (light >= t.emitterCount) {
        return s;
    }
    const LtEmitter e = LtEmittersRef(t.emitters).v[light];
    s.light = light;
    s.pmf = pmf;
    s.kind = e.kind;
    vec3 position;
    float pdfArea;
    lt_sample_point(e, u1, u2, position, pdfArea);
    s.position[0] = position.x;
    s.position[1] = position.y;
    s.position[2] = position.z;
    s.pdfArea = pdfArea;
    return s;
}

float lt_pmf(uint64_t headerAddress, vec3 p, vec3 n, uint light) {
    const LtHeader t = LtHeaderRef(headerAddress).h;
    if (light >= t.emitterCount) {
        return 0.0;
    }
    const LtEmitter e = LtEmittersRef(t.emitters).v[light];
    const float pDir = lt_directional_probability(t);
    if (e.kind == LT_KIND_DIRECTIONAL) {
        precise float r = pDir / float(t.directionalCount);
        return t.directionalCount > 0u ? r : 0.0;
    }
    if (e.leafNode == LT_INVALID || t.nodeCount == 0u) {
        return 0.0;
    }
    const bool hasNormal = lt_has_normal(n);
    LtNodesRef nodes = LtNodesRef(t.nodes);
    precise float pmf = 1.0 - pDir;
    uint node = 0u;
    for (uint level = 0u; level < e.depth; ++level) {
        const LtNode current = nodes.v[node];
        const uint c0 = node + 1u;
        const uint c1 = current.childOrEmitter;
        const float i0 = lt_importance(nodes.v[c0], p, n, hasNormal);
        const float i1 = lt_importance(nodes.v[c1], p, n, hasNormal);
        float p0;
        float p1;
        lt_branch(i0, i1, p0, p1);
        if (((e.bitTrail >> level) & 1u) == 0u) {
            pmf = pmf * p0;
            node = c0;
        } else {
            pmf = pmf * p1;
            node = c1;
        }
    }
    return pmf;
}

#endif
