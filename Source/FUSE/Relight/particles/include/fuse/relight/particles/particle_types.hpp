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
// Ported from dxvk-remix src/dxvk/shaders/rtx/pass/particles/particle_system_{common,enums,binding_indices}.h@0867d3c

// FUSE Relight RL-3.6: the particle system's data layouts, shared by the CPU kernels (particle_kernels.hpp) and the
// GLSL twin (shaders/particle_common.glsl). Every struct is made of 4-byte scalars and scalar arrays only, so the C++
// layout and the GLSL std430 layout are the same byte for byte (the static_asserts below and the GLSL declarations
// list the same offsets).
//
// Modifications (FUSE): the layouts are all-scalar std430 (no half floats, no bitfields, no mat4x3); the particle is
// 64 bytes with f32 rotation / time to live and an explicit u32 state (upstream: a 48-byte particle with half
// rotation / time to live and an infinity sentinel for "dead"); spawn contexts address one shared emitter-geometry
// pool (upstream: bindless buffers); the per-system constants are one struct per system in a storage buffer
// (upstream: a constant buffer per dispatch).
#pragma once

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::relight::particles {

// ---- enums (particle_system_enums.h) ------------------------------------------------------------------------------

enum class BillboardType : u32 {
    FaceCameraSpherical = 0,   ///< classic billboard
    FaceCameraUpAxisLocked = 1, ///< cylindrical billboard (fixed up axis)
    FaceCameraPosition = 2,    ///< camera -> particle vector
    FaceWorldUp = 3,           ///< horizontal plane (faces the up axis)
};

enum class SpriteSheetMode : u32 {
    UseMaterialSpriteSheet = 0,    ///< the material's sprite sheet parameters
    OverrideMaterialLifetime = 1,  ///< frame 0 at birth, the last frame at death
    OverrideMaterialRandom = 2,    ///< one random frame for the particle's life
};

enum class CollisionMode : u32 { Bounce = 0, Stop = 1, Kill = 2 };

enum class RandomFlipAxis : u32 { None = 0, Horizontal = 1, Vertical = 2, Both = 3 };

/// GpuSystemDesc::flags bits (upstream: the uint8_t bitfields of GpuParticleSystemDesc).
enum SystemFlag : u32 {
    kFlagHideEmitter = 1u << 0,
    kFlagEnableMotionTrail = 1u << 1,
    kFlagUseTurbulence = 1u << 2,
    kFlagAlignParticlesToVelocity = 1u << 3,
    kFlagUseSpawnTexcoords = 1u << 4,
    kFlagEnableCollisionDetection = 1u << 5,
    kFlagRestrictVelocityX = 1u << 6,
    kFlagRestrictVelocityY = 1u << 7,
    kFlagRestrictVelocityZ = 1u << 8,
};

/// Rows of the per-system animation table (upstream ParticleAnimationDataRows; a 256 x 7 RGBA16F texture there, a
/// 7 x 256 array of f32 x 4 here, sampled with the same bilinear rule in the kernel).
enum AnimationRow : u32 {
    kRowMinColor = 0,
    kRowMaxColor = 1,
    kRowMinSize = 2,
    kRowMaxSize = 3,
    kRowMinRotationSpeed = 4,
    kRowMaxRotationSpeed = 5,
    kRowMaxVelocity = 6,
    kAnimationRowCount = 7,
};
inline constexpr u32 kAnimationWidth = 256; ///< texels per row (upstream width)
inline constexpr u32 kAnimationTexels = kAnimationRowCount * kAnimationWidth;

/// f32 x 4 (std430 vec4).
struct Float4 {
    f32 v[4];
};
static_assert(sizeof(Float4) == 16);

// ---- GPU structures -----------------------------------------------------------------------------------------------

/// GpuParticleSystemDesc: everything the kernels read of a system's description (128 bytes).
struct GpuSystemDesc {
    f32 attractorPosition[3];
    f32 attractorForce;
    f32 minTimeToLive;
    f32 maxTimeToLive;
    f32 initialVelocityFromNormal;
    f32 initialVelocityConeAngleDegrees;
    f32 turbulenceFrequency;
    f32 turbulenceForce;
    f32 motionTrailMultiplier;
    f32 minSpawnRotationSpeed; ///< kept for layout parity with upstream; the kernels read the animation rows
    f32 initialRotationDeviationDegrees;
    f32 spawnBurstDuration;
    f32 dragCoefficient;
    f32 attractorRadius;
    f32 gravityForce;
    f32 initialVelocityFromMotion;
    u32 maxNumParticles;
    u32 billboardType;   ///< BillboardType
    u32 spriteSheetMode; ///< SpriteSheetMode
    u32 collisionMode;   ///< CollisionMode
    u32 randomFlipAxis;  ///< RandomFlipAxis
    f32 spawnRatePerSecond;
    f32 collisionThickness;
    f32 collisionRestitution;
    u32 spriteSheetRows;
    u32 spriteSheetCols;
    u32 flags; ///< SystemFlag bits
    u32 pad0;
    u32 pad1;
    u32 pad2;
};
static_assert(sizeof(GpuSystemDesc) == 128);
static_assert(offsetof(GpuSystemDesc, maxNumParticles) == 72);
static_assert(offsetof(GpuSystemDesc, flags) == 112);

/// Particle state values (upstream: timeToLive == +inf half means dead).
enum ParticleState : u32 {
    kParticleDead = 0,  ///< never spawned, or retired (counted once by the evolve kernel)
    kParticleAlive = 1, ///< spawned; "sleeping" (invisible, reusable) once timeToLive <= 0
};

/// GpuParticle (64 bytes).
struct GpuParticle {
    f32 position[3];
    u32 color; ///< RGBA8 unorm base colour (R in the low byte)
    f32 velocity[3];
    f32 randSeed;    ///< [0, 1): fixed for the particle's life
    f32 uvMinMax[4]; ///< u min, v min, u max, v max
    f32 rotation;
    f32 timeToLive;
    u32 state; ///< ParticleState
    u32 pad;
};
static_assert(sizeof(GpuParticle) == 64);
static_assert(offsetof(GpuParticle, timeToLive) == 52);
static_assert(offsetof(GpuParticle, state) == 56);

/// Spawn context flags.
enum SpawnFlag : u32 {
    kSpawnHasColors = 1u << 0,
    kSpawnHasTexcoords = 1u << 1,
};

/// GpuSpawnContext (128 bytes): one emitter instance of one frame. Transforms are 3 rows of a 3x4 matrix
/// (world = rows * (p, 1)); the emitter mesh is a range of the geometry pool.
struct GpuSpawnContext {
    f32 objectToWorld[12];
    f32 prevObjectToWorld[12];
    u32 indexOffset;      ///< first index (u32) of the emitter's triangle list in the pool
    u32 triangleCount;    ///< 0: the context spawns nothing
    u32 vertexOffset;     ///< base vertex of the emitter's positions / colours / texcoords (indices are relative)
    u32 prevVertexOffset; ///< base vertex of the previous-frame positions (== vertexOffset for rigid emitters)
    u32 flags;            ///< SpawnFlag bits
    u32 pad0;
    u32 pad1;
    u32 pad2;
};
static_assert(sizeof(GpuSpawnContext) == 128);
static_assert(offsetof(GpuSpawnContext, indexOffset) == 96);

/// ParticleVertex (24 bytes): the billboard vertex.
struct GpuParticleVertex {
    f32 position[3];
    u32 color; ///< B8G8R8A8 unorm (B in the low byte), as upstream's vertex format
    f32 texcoord[2];
};
static_assert(sizeof(GpuParticleVertex) == 24);

/// Per-system, per-frame constants (upstream ParticleSystemConstants + the GpuParticleSystem counters) (416 bytes).
/// Matrices are column-major (GLSL mat4 order): m[col * 4 + row].
struct GpuFrameConstants {
    GpuSystemDesc desc;
    f32 viewToWorld[16];
    f32 prevWorldToProjection[16];
    f32 upDirection[3];
    f32 deltaTimeSecs;
    f32 absoluteTimeSecs;
    f32 invDeltaTimeSecs;
    u32 frameIdx;
    u32 systemSeed;
    u32 renderingWidth;
    u32 renderingHeight;
    f32 resolveTransparencyThreshold;
    f32 minParticleSize; ///< pixels
    f32 sceneScale;
    u32 particleBase;  ///< first particle of the system in the particle pool
    u32 vertexBase;    ///< first vertex of the system in the vertex pool
    u32 animationBase; ///< first Float4 of the system's animation table
    u32 spawnMapBase;  ///< first entry of the system's spawn map
    u32 spawnParticleOffset;
    u32 spawnParticleCount;
    u32 particleTailOffset;
    u32 simulateParticleCount;
    u32 particleCount;
    u32 verticesPerParticle; ///< 4, or 8 with a motion trail
    u32 counterIndex;        ///< death counter of the system (counters[counterIndex])
    f32 vertexOffsets[16];   ///< 8 x (x, y)
};
static_assert(sizeof(GpuFrameConstants) == 416);
static_assert(offsetof(GpuFrameConstants, viewToWorld) == 128);
static_assert(offsetof(GpuFrameConstants, upDirection) == 256);
static_assert(offsetof(GpuFrameConstants, sceneScale) == 304);
static_assert(offsetof(GpuFrameConstants, vertexOffsets) == 352);

/// Storage-buffer bindings of the three compute kernels (set 0; the GLSL twin uses the same numbers).
enum Binding : u32 {
    kBindingConstants = 0,
    kBindingParticles = 1,
    kBindingSpawnContexts = 2,
    kBindingSpawnMap = 3,
    kBindingPositions = 4,
    kBindingColors = 5,
    kBindingTexcoords = 6,
    kBindingIndices = 7,
    kBindingAnimation = 8,
    kBindingVertices = 9,
    kBindingCounters = 10,
    kBindingCount = 11,
};

/// Push constants of the kernels: which GpuFrameConstants entry the dispatch runs.
struct GpuPushConstants {
    u32 systemIndex;
};

inline constexpr u32 kWorkgroupSize = 128; ///< upstream numthreads(128, 1, 1)

} // namespace fuse::relight::particles
