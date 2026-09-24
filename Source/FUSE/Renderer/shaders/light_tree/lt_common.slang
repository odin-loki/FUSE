// WP-7.1 light tree: Slang twin of lt_common.glsl (see there) and a line-for-line port of
// include/fuse/renderer/light_tree/light_tree_kernel.hpp. Compile with -fp-mode precise (Slang does not honour
// `precise` per variable), so no multiply-add is contracted and the sampler returns the CPU kernel's bits.
#pragma once

static const uint kLtInvalid = 0xFFFFFFFFu;
static const uint kLtMaxDepth = 31u;
static const float kLtOneMinusEpsilon = 0.99999994;
static const float kLtMinDistance2 = 1e-12;

static const uint kLtKindNone = 0u;
static const uint kLtKindPoint = 1u;
static const uint kLtKindSpot = 2u;
static const uint kLtKindRect = 3u;
static const uint kLtKindDisk = 4u;
static const uint kLtKindTriangle = 5u;
static const uint kLtKindDirectional = 6u;

static const uint kLtFlagLeaf = 1u;
static const uint kLtFlagTwoSided = 2u;

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

float ltMin(float a, float b) { return b < a ? b : a; }
float ltMax(float a, float b) { return a < b ? b : a; }
float ltSafeSqrt(float x) { return sqrt(ltMax(x, 0.0)); }

float ltCosSubClamped(float sinA, float cosA, float sinB, float cosB) {
    if (cosA > cosB) {
        return 1.0;
    }
    const float x = cosA * cosB;
    const float y = sinA * sinB;
    return x + y;
}

float ltSinSubClamped(float sinA, float cosA, float sinB, float cosB) {
    if (cosA > cosB) {
        return 0.0;
    }
    const float x = sinA * cosB;
    const float y = cosA * sinB;
    return x - y;
}

float ltImportance(LtNode node, float3 p, float3 n, bool hasNormal) {
    const float cx = (node.boundsMin[0] + node.boundsMax[0]) * 0.5;
    const float cy = (node.boundsMin[1] + node.boundsMax[1]) * 0.5;
    const float cz = (node.boundsMin[2] + node.boundsMax[2]) * 0.5;
    const float dx = p.x - cx;
    const float dy = p.y - cy;
    const float dz = p.z - cz;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    const float gx = node.boundsMax[0] - node.boundsMin[0];
    const float gy = node.boundsMax[1] - node.boundsMin[1];
    const float gz = node.boundsMax[2] - node.boundsMin[2];
    const float radius2 = (gx * gx + gy * gy + gz * gz) * 0.25;
    const float d2 = ltMax(ltMax(dist2, radius2), kLtMinDistance2);
    float wx = 0.0;
    float wy = 0.0;
    float wz = 0.0;
    if (dist2 > 0.0) {
        const float len = sqrt(dist2);
        wx = dx / len;
        wy = dy / len;
        wz = dz / len;
    }
    float cosW = node.axis[0] * wx + node.axis[1] * wy + node.axis[2] * wz;
    if ((node.flags & kLtFlagTwoSided) != 0u) {
        cosW = abs(cosW);
    }
    const float sinW = ltSafeSqrt(1.0 - cosW * cosW);
    float cosB = -1.0;
    float sinB = 0.0;
    if (dist2 > radius2) {
        const float sin2 = radius2 / dist2;
        cosB = ltSafeSqrt(1.0 - sin2);
        sinB = sqrt(sin2);
    }
    const float cosX = ltCosSubClamped(sinW, cosW, node.sinThetaO, node.cosThetaO);
    const float sinX = ltSinSubClamped(sinW, cosW, node.sinThetaO, node.cosThetaO);
    const float cosP = ltCosSubClamped(sinX, cosX, sinB, cosB);
    if (cosP <= node.cosThetaE) {
        return 0.0;
    }
    float importance = node.phi * cosP / d2;
    if (hasNormal) {
        const float cosI = abs(wx * n.x + wy * n.y + wz * n.z);
        const float sinI = ltSafeSqrt(1.0 - cosI * cosI);
        const float cosPI = ltCosSubClamped(sinI, cosI, sinB, cosB);
        importance = importance * cosPI;
    }
    return ltMax(importance, 0.0);
}

void ltBranch(float i0, float i1, out float p0, out float p1) {
    const float sum = i0 + i1;
    if (sum > 0.0) {
        p0 = i0 / sum;
        p1 = i1 / sum;
    } else {
        p0 = 0.5;
        p1 = 0.5;
    }
}

bool ltHasNormal(float3 n) { return n.x != 0.0 || n.y != 0.0 || n.z != 0.0; }

float ltDirectionalProbability(LtHeader t) {
    const uint total = t.directionalCount + (t.nodeCount > 0u ? 1u : 0u);
    if (total == 0u) {
        return 0.0;
    }
    return float(t.directionalCount) / float(total);
}

float ltSinPoly(float t) {
    const float t2 = t * t;
    const float a = t2 * 2.7557319e-06;
    const float b = t2 * (-0.00019841270 + a);
    const float c = t2 * (0.0083333333 + b);
    const float d = t2 * (-0.16666667 + c);
    const float e = t * d;
    return t + e;
}

float ltCosPoly(float t) {
    const float t2 = t * t;
    const float a = t2 * 2.4801587e-05;
    const float b = t2 * (-0.0013888889 + a);
    const float c = t2 * (0.041666668 + b);
    const float d = t2 * (-0.5 + c);
    return 1.0 + d;
}

void ltConcentric(float u1, float u2, out float x, out float y) {
    const float a = u1 * 2.0 - 1.0;
    const float b = u2 * 2.0 - 1.0;
    x = 0.0;
    y = 0.0;
    if (a == 0.0 && b == 0.0) {
        return;
    }
    if (abs(a) > abs(b)) {
        const float t = (b / a) * 0.78539816;
        x = a * ltCosPoly(t);
        y = a * ltSinPoly(t);
    } else {
        const float t = (a / b) * 0.78539816;
        x = b * ltSinPoly(t);
        y = b * ltCosPoly(t);
    }
}

void ltSamplePoint(LtEmitter e, float u1, float u2, out float3 position, out float pdfArea) {
    float a = 0.0;
    float b = 0.0;
    bool surface = true;
    if (e.kind == kLtKindTriangle) {
        const float su = sqrt(u1);
        a = su * (1.0 - u2);
        b = su * u2;
    } else if (e.kind == kLtKindRect) {
        a = u1 * 2.0 - 1.0;
        b = u2 * 2.0 - 1.0;
    } else if (e.kind == kLtKindDisk) {
        ltConcentric(u1, u2, a, b);
    } else {
        surface = false;
    }
    if (!surface) {
        const bool directional = e.kind == kLtKindDirectional;
        position = directional ? float3(e.normal[0], e.normal[1], e.normal[2]) : float3(e.p0[0], e.p0[1], e.p0[2]);
        pdfArea = 0.0;
        return;
    }
    const float px = e.p0[0] + e.e1[0] * a + e.e2[0] * b;
    const float py = e.p0[1] + e.e1[1] * a + e.e2[1] * b;
    const float pz = e.p0[2] + e.e1[2] * a + e.e2[2] * b;
    position = float3(px, py, pz);
    pdfArea = e.area > 0.0 ? 1.0 / e.area : 0.0;
}

LtSampleResult ltSample(uint64_t headerAddress, float3 p, float3 n, float u0, float u1, float u2) {
    LtSampleResult s;
    s.light = kLtInvalid;
    s.pmf = 0.0;
    s.pdfArea = 0.0;
    s.kind = kLtKindNone;
    s.position[0] = 0.0;
    s.position[1] = 0.0;
    s.position[2] = 0.0;
    s.evalPmf = 0.0;
    const LtHeader t = ((LtHeader*)headerAddress)[0];
    const float pDir = ltDirectionalProbability(t);
    if (t.directionalCount == 0u && t.nodeCount == 0u) {
        return s;
    }
    const bool hasNormal = ltHasNormal(n);
    LtNode* nodes = (LtNode*)t.nodes;
    float u = u0;
    uint light = kLtInvalid;
    float pmf = 0.0;
    if (u < pDir) {
        u = ltMin(u / pDir, kLtOneMinusEpsilon);
        uint k = uint(u * float(t.directionalCount));
        if (k >= t.directionalCount) {
            k = t.directionalCount - 1u;
        }
        light = ((uint*)t.directional)[k];
        pmf = pDir / float(t.directionalCount);
    } else {
        const float pTree = 1.0 - pDir;
        u = ltMin((u - pDir) / pTree, kLtOneMinusEpsilon);
        pmf = pTree;
        uint node = 0u;
        for (uint level = 0u; level <= kLtMaxDepth; ++level) {
            const LtNode current = nodes[node];
            if ((current.flags & kLtFlagLeaf) != 0u) {
                light = current.childOrEmitter;
                break;
            }
            const uint c0 = node + 1u;
            const uint c1 = current.childOrEmitter;
            const float i0 = ltImportance(nodes[c0], p, n, hasNormal);
            const float i1 = ltImportance(nodes[c1], p, n, hasNormal);
            float p0;
            float p1;
            ltBranch(i0, i1, p0, p1);
            if (u < p0) {
                u = ltMin(u / p0, kLtOneMinusEpsilon);
                pmf = pmf * p0;
                node = c0;
            } else {
                u = ltMin((u - p0) / p1, kLtOneMinusEpsilon);
                pmf = pmf * p1;
                node = c1;
            }
        }
    }
    if (light >= t.emitterCount) {
        return s;
    }
    const LtEmitter e = ((LtEmitter*)t.emitters)[light];
    s.light = light;
    s.pmf = pmf;
    s.kind = e.kind;
    float3 position;
    float pdfArea;
    ltSamplePoint(e, u1, u2, position, pdfArea);
    s.position[0] = position.x;
    s.position[1] = position.y;
    s.position[2] = position.z;
    s.pdfArea = pdfArea;
    return s;
}

float ltPmf(uint64_t headerAddress, float3 p, float3 n, uint light) {
    const LtHeader t = ((LtHeader*)headerAddress)[0];
    if (light >= t.emitterCount) {
        return 0.0;
    }
    const LtEmitter e = ((LtEmitter*)t.emitters)[light];
    const float pDir = ltDirectionalProbability(t);
    if (e.kind == kLtKindDirectional) {
        return t.directionalCount > 0u ? pDir / float(t.directionalCount) : 0.0;
    }
    if (e.leafNode == kLtInvalid || t.nodeCount == 0u) {
        return 0.0;
    }
    const bool hasNormal = ltHasNormal(n);
    LtNode* nodes = (LtNode*)t.nodes;
    float pmf = 1.0 - pDir;
    uint node = 0u;
    for (uint level = 0u; level < e.depth; ++level) {
        const LtNode current = nodes[node];
        const uint c0 = node + 1u;
        const uint c1 = current.childOrEmitter;
        const float i0 = ltImportance(nodes[c0], p, n, hasNormal);
        const float i1 = ltImportance(nodes[c1], p, n, hasNormal);
        float p0;
        float p1;
        ltBranch(i0, i1, p0, p1);
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
