/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/shaders/rtx/pass/particles/*.slang@0867d3c and
// src/dxvk/shaders/rtx/utility/{noise,procedural_noise,
// sampling,math,packing}.slangh@0867d3c
// Portions Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
// (src/dxvk/shaders/rtx/utility/noise.slangh@0867d3c),
// same MIT licence.

// FUSE Relight RL-3.6: the GLSL twin of include/fuse/relight/particles/particle_kernels.hpp (layouts:
// particle_types.hpp). Every function mirrors its C++ namesake expression for expression; the ones that decide life
// and death (lerpp, initialTimeToLive, the time-to-live update) and the chains whose errors are amplified (the portable
// sine, value noise and its curl: finite differences over 2 e = 0.02) are `precise`, so no contraction changes their
// rounding: the integer state matches the CPU reference bit for bit (the floats too on Lavapipe).
//
// Modifications (FUSE): GLSL 4.60 instead of Slang; std430 all-scalar layouts; see particle_kernels.hpp for the list.

#ifndef FUSE_RELIGHT_PARTICLE_COMMON_GLSL
#define FUSE_RELIGHT_PARTICLE_COMMON_GLSL

// ---- layouts (particle_types.hpp) -----------------------------------------------------------------------------------

struct SystemDesc {
    float attractorPosition[3];
    float attractorForce;
    float minTimeToLive;
    float maxTimeToLive;
    float initialVelocityFromNormal;
    float initialVelocityConeAngleDegrees;
    float turbulenceFrequency;
    float turbulenceForce;
    float motionTrailMultiplier;
    float minSpawnRotationSpeed;
    float initialRotationDeviationDegrees;
    float spawnBurstDuration;
    float dragCoefficient;
    float attractorRadius;
    float gravityForce;
    float initialVelocityFromMotion;
    uint maxNumParticles;
    uint billboardType;
    uint spriteSheetMode;
    uint collisionMode;
    uint randomFlipAxis;
    float spawnRatePerSecond;
    float collisionThickness;
    float collisionRestitution;
    uint spriteSheetRows;
    uint spriteSheetCols;
    uint flags;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct FrameConstants {
    SystemDesc desc;
    float viewToWorld[16];
    float prevWorldToProjection[16];
    float upDirection[3];
    float deltaTimeSecs;
    float absoluteTimeSecs;
    float invDeltaTimeSecs;
    uint frameIdx;
    uint systemSeed;
    uint renderingWidth;
    uint renderingHeight;
    float resolveTransparencyThreshold;
    float minParticleSize;
    float sceneScale;
    uint particleBase;
    uint vertexBase;
    uint animationBase;
    uint spawnMapBase;
    uint spawnParticleOffset;
    uint spawnParticleCount;
    uint particleTailOffset;
    uint simulateParticleCount;
    uint particleCount;
    uint verticesPerParticle;
    uint counterIndex;
    float vertexOffsets[16];
};

struct Particle {
    float position[3];
    uint color;
    float velocity[3];
    float randSeed;
    float uvMinMax[4];
    float rotation;
    float timeToLive;
    uint state;
    uint pad;
};

struct SpawnContext {
    float objectToWorld[12];
    float prevObjectToWorld[12];
    uint indexOffset;
    uint triangleCount;
    uint vertexOffset;
    uint prevVertexOffset;
    uint flags;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct ParticleVertex {
    float position[3];
    uint color;
    float texcoord[2];
};

const uint kFlagHideEmitter = 1u << 0;
const uint kFlagEnableMotionTrail = 1u << 1;
const uint kFlagUseTurbulence = 1u << 2;
const uint kFlagAlignParticlesToVelocity = 1u << 3;
const uint kFlagUseSpawnTexcoords = 1u << 4;
const uint kFlagEnableCollisionDetection = 1u << 5;
const uint kFlagRestrictVelocityX = 1u << 6;
const uint kFlagRestrictVelocityY = 1u << 7;
const uint kFlagRestrictVelocityZ = 1u << 8;

const uint kSpawnHasColors = 1u << 0;
const uint kSpawnHasTexcoords = 1u << 1;

const uint kParticleDead = 0u;
const uint kParticleAlive = 1u;

const uint kRowMinColor = 0u;
const uint kRowMinSize = 2u;
const uint kRowMinRotationSpeed = 4u;
const uint kRowMaxVelocity = 6u;
const uint kAnimationWidth = 256u;

const uint kBillboardFaceCameraUpAxisLocked = 1u;
const uint kBillboardFaceCameraPosition = 2u;
const uint kBillboardFaceWorldUp = 3u;
const uint kSpriteSheetLifetime = 1u;
const uint kSpriteSheetRandom = 2u;
const uint kFlipHorizontal = 1u;
const uint kFlipVertical = 2u;
const uint kFlipBoth = 3u;

// Exact f32 constants (the C++ values' bits).
#define kTwoPi uintBitsToFloat(0x40C90FDBu)
#define kDegToRad uintBitsToFloat(0x3C8EFA35u)
#define kInv255 uintBitsToFloat(0x3B808081u)
#define kMinimumParticleLife uintBitsToFloat(0x3D088889u)

// ---- bindings (Binding in particle_types.hpp) -----------------------------------------------------------------------

layout(std430, set = 0, binding = 0) readonly buffer ConstantsBuffer { FrameConstants constants[]; };
layout(std430, set = 0, binding = 1) buffer ParticlesBuffer { Particle particles[]; };
layout(std430, set = 0, binding = 2) readonly buffer SpawnContextsBuffer { SpawnContext spawnContexts[]; };
layout(std430, set = 0, binding = 3) readonly buffer SpawnMapBuffer { uint spawnMap[]; };
layout(std430, set = 0, binding = 4) readonly buffer PositionsBuffer { float positions[]; };
layout(std430, set = 0, binding = 5) readonly buffer ColorsBuffer { uint colors[]; };
layout(std430, set = 0, binding = 6) readonly buffer TexcoordsBuffer { float texcoords[]; };
layout(std430, set = 0, binding = 7) readonly buffer IndicesBuffer { uint indices[]; };
layout(std430, set = 0, binding = 8) readonly buffer AnimationBuffer { vec4 animation[]; };
layout(std430, set = 0, binding = 9) writeonly buffer VerticesBuffer { ParticleVertex vertices[]; };
layout(std430, set = 0, binding = 10) buffer CountersBuffer { uint counters[]; };

layout(push_constant) uniform Push { uint systemIndex; } push;

// ---- scalar helpers -----------------------------------------------------------------------------------------------

float minf(float a, float b) { return b < a ? b : a; }
float maxf(float a, float b) { return a < b ? b : a; }
float clampf(float x, float lo, float hi) { return minf(maxf(x, lo), hi); }
float saturatef(float x) { return clampf(x, 0.0, 1.0); }
float lerpp(float a, float b, float t) {
    precise float r = a + (b - a) * t;
    return r;
}
vec3 lerp3(vec3 a, vec3 b, float t) { return vec3(lerpp(a.x, b.x, t), lerpp(a.y, b.y, t), lerpp(a.z, b.z, t)); }
vec4 lerp4(vec4 a, vec4 b, float t) {
    return vec4(lerpp(a.x, b.x, t), lerpp(a.y, b.y, t), lerpp(a.z, b.z, t), lerpp(a.w, b.w, t));
}
float dot3(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 cross3(vec3 a, vec3 b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float length3(vec3 a) { return sqrt(dot3(a, a)); }
vec3 safeNormalize3(vec3 a, vec3 fallback) {
    float l2 = dot3(a, a);
    return l2 > 0.0 ? a * (1.0 / sqrt(l2)) : fallback;
}
vec3 transform34(float rows[12], vec3 p, float w) {
    return vec3(rows[0] * p.x + rows[1] * p.y + rows[2] * p.z + rows[3] * w,
                rows[4] * p.x + rows[5] * p.y + rows[6] * p.z + rows[7] * w,
                rows[8] * p.x + rows[9] * p.y + rows[10] * p.z + rows[11] * w);
}
vec4 transform44(float m[16], vec3 p) {
    return vec4(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14], m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15]);
}

// ---- portable sine / cosine (psin / pcos of particle_kernels.hpp) ------------------------------------------------

float psin(float x) {
    precise float k = floor(x * uintBitsToFloat(0x3E22F983u) + 0.5);
    precise float r = x - k * uintBitsToFloat(0x40C90FDBu);
    r = r - k * uintBitsToFloat(0xB43BBD2Eu);
    float halfPi = uintBitsToFloat(0x3FC90FDBu);
    float pi = uintBitsToFloat(0x40490FDBu);
    if (r > halfPi) {
        r = pi - r;
    } else if (r < -halfPi) {
        r = -pi - r;
    }
    precise float r2 = r * r;
    float p = uintBitsToFloat(0xB2D7322Bu);
    precise float p3 = uintBitsToFloat(0x3638EF1Du) + r2 * p;
    precise float p2 = uintBitsToFloat(0xB9500D01u) + r2 * p3;
    precise float p1 = uintBitsToFloat(0x3C088889u) + r2 * p2;
    precise float p0 = uintBitsToFloat(0xBE2AAAABu) + r2 * p1;
    precise float result = r * (1.0 + r2 * p0);
    return result;
}
float pcos(float x) {
    precise float y = x + uintBitsToFloat(0x3FC90FDBu);
    return psin(y);
}

// ---- packing ------------------------------------------------------------------------------------------------------

uint f32ToUnorm8(float f) { return uint(floor(saturatef(f) * 255.0 + 0.5)); }
uint packUnorm4x8f(vec4 c) {
    return f32ToUnorm8(c.x) | (f32ToUnorm8(c.y) << 8u) | (f32ToUnorm8(c.z) << 16u) | (f32ToUnorm8(c.w) << 24u);
}
vec4 unpackUnorm4x8f(uint u) {
    return vec4(float(u & 0xFFu) * kInv255, float((u >> 8u) & 0xFFu) * kInv255, float((u >> 16u) & 0xFFu) * kInv255,
                float(u >> 24u) * kInv255);
}

// ---- random numbers -----------------------------------------------------------------------------------------------

uint uintHash(uint x) {
    x ^= x >> 16u;
    x *= 0x21f0aaadu;
    x ^= x >> 15u;
    x *= 0x735a2d97u;
    x ^= x >> 15u;
    return x;
}
uint uintHash3(uint x, uint y, uint z) { return uintHash(x ^ uintHash(y) ^ uintHash(z)); }
float unorm23ToFloat(uint x) {
    precise float r = uintBitsToFloat((x & 0x007FFFFFu) | 0x3F800000u) - 1.0;
    return r;
}

// ---- value noise --------------------------------------------------------------------------------------------------

vec4 valueNoiseTexel(int x, int y, int z) {
    uint h = uintHash3(uint(x), uint(y) + 0x9E3779B9u, uint(z) + 0x7F4A7C15u);
    vec4 u = unpackUnorm4x8f(h);
    precise vec4 r = vec4(u.x * 2.0 - 1.0, u.y * 2.0 - 1.0, u.z * 2.0 - 1.0, u.w * 2.0 - 1.0);
    return r;
}
int wrapNoise(int i) { return int(uint(i) & 63u); }
vec4 valueNoiseLutSample(vec3 p) {
    precise vec3 t = vec3(p.x - 0.5, p.y - 0.5, p.z - 0.5);
    float fx = floor(t.x), fy = floor(t.y), fz = floor(t.z);
    precise vec3 f = vec3(t.x - fx, t.y - fy, t.z - fz);
    int x0 = wrapNoise(int(fx)), y0 = wrapNoise(int(fy)), z0 = wrapNoise(int(fz));
    int x1 = wrapNoise(x0 + 1), y1 = wrapNoise(y0 + 1), z1 = wrapNoise(z0 + 1);
    vec4 c00 = lerp4(valueNoiseTexel(x0, y0, z0), valueNoiseTexel(x1, y0, z0), f.x);
    vec4 c10 = lerp4(valueNoiseTexel(x0, y1, z0), valueNoiseTexel(x1, y1, z0), f.x);
    vec4 c01 = lerp4(valueNoiseTexel(x0, y0, z1), valueNoiseTexel(x1, y0, z1), f.x);
    vec4 c11 = lerp4(valueNoiseTexel(x0, y1, z1), valueNoiseTexel(x1, y1, z1), f.x);
    return lerp4(lerp4(c00, c10, f.y), lerp4(c01, c11, f.y), f.z);
}
float valueNoise4D(vec3 pos, float w) {
    precise float angle = (w - floor(w)) * kTwoPi;
    precise float ux = pcos(angle) * 0.5 + 0.5;
    precise float uy = psin(angle) * 0.5 + 0.5;
    vec4 n = valueNoiseLutSample(pos);
    float bottom = lerpp(n.x, n.y, ux);
    float top = lerpp(n.z, n.w, ux);
    return lerpp(bottom, top, uy);
}
vec3 noiseField(vec3 p, float w) {
    precise vec3 p0 = p + vec3(37.1, 17.2, 19.3);
    precise vec3 p1 = p + vec3(29.9, 11.4, 5.7);
    precise vec3 p2 = p + vec3(11.0, 59.5, 47.8);
    precise float w0 = w + 101.1;
    precise float w1 = w + 45.6;
    precise float w2 = w + 13.2;
    return vec3(valueNoise4D(p0, w0), valueNoise4D(p1, w1), valueNoise4D(p2, w2));
}
vec3 curlOfValueNoise(vec3 pos, float time) {
    const float e = 0.01;
    precise vec3 ppx = pos + vec3(e, 0.0, 0.0);
    precise vec3 pmx = pos - vec3(e, 0.0, 0.0);
    precise vec3 ppy = pos + vec3(0.0, e, 0.0);
    precise vec3 pmy = pos - vec3(0.0, e, 0.0);
    precise vec3 ppz = pos + vec3(0.0, 0.0, e);
    precise vec3 pmz = pos - vec3(0.0, 0.0, e);
    vec3 fpx = noiseField(ppx, time);
    vec3 fmx = noiseField(pmx, time);
    vec3 fpy = noiseField(ppy, time);
    vec3 fmy = noiseField(pmy, time);
    vec3 fpz = noiseField(ppz, time);
    vec3 fmz = noiseField(pmz, time);
    precise float k = 0.5 / e;
    precise vec3 dFdx = (fpx - fmx) * k;
    precise vec3 dFdy = (fpy - fmy) * k;
    precise vec3 dFdz = (fpz - fmz) * k;
    precise vec3 curl = vec3(dFdz.y - dFdy.z, dFdx.z - dFdz.x, dFdy.x - dFdx.y);
    return curl;
}

// ---- sampling -----------------------------------------------------------------------------------------------------

float signNotZero(float v) { return v >= 0.0 ? 1.0 : -1.0; }
void orthonormalBasis(vec3 n, out vec3 tangent, out vec3 bitangent) {
    float sgn = signNotZero(n.z);
    float a = -1.0 / (sgn + n.z);
    float b = n.x * n.y * a;
    tangent = vec3(1.0 + sgn * n.x * n.x * a, sgn * b, -sgn * n.x);
    bitangent = vec3(b, sgn + n.y * n.y * a, -n.y);
}
vec3 sampleDirectionInCone(vec3 axis, float cosConeHalfAngle, float u0, float u1) {
    float phi = kTwoPi * u0;
    float cosTheta = lerpp(cosConeHalfAngle, 1.0, u1);
    float sinTheta = sqrt(maxf(0.0, 1.0 - cosTheta * cosTheta));
    vec3 local = vec3(sinTheta * pcos(phi), sinTheta * psin(phi), cosTheta);
    vec3 t, b;
    orthonormalBasis(axis, t, b);
    return t * local.x + b * local.y + axis * local.z;
}

// ---- the particle -------------------------------------------------------------------------------------------------

bool isDead(Particle p) { return p.state == kParticleDead; }
bool isSleeping(Particle p) { return p.timeToLive <= 0.0; }
float initialTimeToLive(SystemDesc d, float randSeed) {
    precise float r = maxf(kMinimumParticleLife, lerpp(d.minTimeToLive, d.maxTimeToLive, randSeed));
    return r;
}
float normalizedLife(SystemDesc d, Particle p) { return p.timeToLive / initialTimeToLive(d, p.randSeed); }

vec4 animationTexel(uint base, uint row, uint x) { return animation[base + row * kAnimationWidth + x]; }
vec4 sampleAnimation(uint base, uint row, float u, bool randomize, float seed) {
    float tx = clampf(u * float(kAnimationWidth) - 0.5, 0.0, float(kAnimationWidth - 1u));
    float fx = floor(tx);
    uint x0 = uint(fx);
    uint x1 = x0 + 1u < kAnimationWidth ? x0 + 1u : kAnimationWidth - 1u;
    float f = tx - fx;
    vec4 a = lerp4(animationTexel(base, row, x0), animationTexel(base, row, x1), f);
    if (!randomize) {
        return a;
    }
    vec4 b = lerp4(animationTexel(base, row + 1u, x0), animationTexel(base, row + 1u, x1), f);
    return lerp4(a, b, seed);
}
vec4 particleColor(SystemDesc d, uint base, Particle q) {
    return unpackUnorm4x8f(q.color) * sampleAnimation(base, kRowMinColor, normalizedLife(d, q), true, q.randSeed);
}
vec2 particleSize(SystemDesc d, uint base, Particle q) {
    return sampleAnimation(base, kRowMinSize, normalizedLife(d, q), true, q.randSeed).xy;
}
float particleRotationSpeed(SystemDesc d, uint base, Particle q) {
    return sampleAnimation(base, kRowMinRotationSpeed, normalizedLife(d, q), true, q.randSeed).x;
}
vec3 particleMaxVelocity(SystemDesc d, uint base, Particle q) {
    return sampleAnimation(base, kRowMaxVelocity, normalizedLife(d, q), false, 0.0).xyz;
}

vec3 loadVec3(float a[3]) { return vec3(a[0], a[1], a[2]); }

#endif
