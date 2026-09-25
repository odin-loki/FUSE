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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_particle_system.h@0867d3c (the rtx.particles.* option declarations)

// FUSE Relight RL-3.6: the particle system's options (RL-0.6 registry; each also answers to its relight.* twin).
//
// Upstream names, defaults and meanings (RtxParticleSystemManager, dxvk-remix rtx_particle_system.h @0867d3c, MIT):
//   rtx.particles.enable / enableSpawning / timeScale
//   rtx.particles.enableDiscontinuityGuard / discontinuityFactor / discontinuityFloor
//   rtx.particles.globalPreset.*   the description of systems created for draws tagged ParticleEmitter by texture
//                                  (rtx.particleEmitterTextures) that carry no USD particle system.
// Borrowed by name (owned elsewhere): rtx.sceneScale (scene/instances), rtx.resolveTransparencyThreshold (the
// renderer; 1/255 when no package declares it), rtx.zUp (scene up axis; Y up when absent).
// Enum-valued options are ints, as in rtx.conf (BillboardType, SpriteSheetMode, CollisionMode, RandomFlipAxis).
// The deprecated rtx.particles.globalPreset.maxSpeed (a NoSave migration onto maxSpawnVelocity / maxTargetVelocity
// upstream) is not declared.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

namespace fuse::relight::particles {

struct ParticleOptions {
    using Vec2f = options::Vec2f;
    using Vec3f = options::Vec3f;
    using Vec4f = options::Vec4f;

    FUSE_RELIGHT_OPTION("rtx.particles", bool, enable, true, "Enables particle simulation and rendering.");
    FUSE_RELIGHT_OPTION("rtx.particles", bool, enableSpawning, true,
                        "Controls whether or not any particle system can currently spawn new particles.");
    FUSE_RELIGHT_OPTION("rtx.particles", float, timeScale, 1.f, "Time modifier, can be used to slow/speed up time.");
    FUSE_RELIGHT_OPTION("rtx.particles", bool, enableDiscontinuityGuard, false,
                        "ON: when an emitter's one-frame motion deviates from a moving average of its recent velocity "
                        "(swap / teleport / loop restart), collapse the spawn prev transform to the current one.");
    FUSE_RELIGHT_OPTION("rtx.particles", float, discontinuityFactor, 3.f,
                        "Multiplier on an emitter's recent velocity (plus the floor) above which a one-frame deviation "
                        "is treated as a discontinuity.");
    FUSE_RELIGHT_OPTION("rtx.particles", float, discontinuityFloor, 1.f,
                        "Minimum per-frame motion added to the emitter's recent speed when forming the discontinuity "
                        "threshold (multiplied by rtx.sceneScale).");

    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", int, spawnRatePerSecond, 100,
                        "Number of particles (per system) to spawn per second on average.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, spawnBurstDuration, 0.f,
                        "Number of seconds between particle spawning bursts (0: continuous spawning).");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", int, numberOfParticlesPerMaterial, 10000,
                        "Maximum number of particles to simulate per material simultaneously.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, minParticleLife, 1.f,
                        "Minimum lifetime (in seconds) to give to a particle when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, maxParticleLife, 1.f,
                        "Maximum lifetime (in seconds) to give to a particle when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec2f, minSpawnSize, Vec2f(10.f),
                        "Minimum size (in centimeters) to give to a particle when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec2f, maxSpawnSize, Vec2f(10.f),
                        "Maximum size (in centimeters) to give to a particle when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, minSpawnRotationSpeed, 0.f,
                        "Minimum rotation speed to give to a particle when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, maxSpawnRotationSpeed, 0.f,
                        "Maximum rotation speed to give to a particle when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec4f, minSpawnColor, Vec4f(1.f),
                        "Minimum range of the color to tint a particle with when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec4f, maxSpawnColor, Vec4f(1.f),
                        "Maximum range of the color to tint a particle with when spawned.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec2f, minTargetSize, Vec2f(0.f),
                        "Minimum size (in centimeters) at the end of the particle's life.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec2f, maxTargetSize, Vec2f(0.f),
                        "Maximum size (in centimeters) at the end of the particle's life.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, minTargetRotationSpeed, 0.f,
                        "Minimum rotation speed at the end of the particle's life.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, maxTargetRotationSpeed, 0.f,
                        "Maximum rotation speed at the end of the particle's life.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec4f, minTargetColor, Vec4f(1.f, 1.f, 1.f, 0.f),
                        "Minimum RGBA color at the end of the particle's life.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec4f, maxTargetColor, Vec4f(1.f, 1.f, 1.f, 0.f),
                        "Maximum RGBA color at the end of the particle's life.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, initialVelocityFromMotion, 0.f,
                        "Multiplier for initial velocity applied at spawn time, based on the spawning object's velocity.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, initialVelocityFromNormal, 0.f,
                        "Initial speed to apply on spawn (centimeters per sec) along the normal of the spawning triangle.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, initialVelocityConeAngleDegrees, 0.f,
                        "Half angle, in degrees, of the random emission cone around the triangle's surface normal.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, gravityForce, 0.f,
                        "Net influence of gravity acting on each particle (centimeters per second squared).");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec3f, maxSpawnVelocity, Vec3f(-1.f),
                        "Maximum velocity of a particle (centimeters per second) at spawn time; negative: unlimited.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec3f, maxTargetVelocity, Vec3f(-1.f),
                        "Maximum velocity of a particle (centimeters per second) at the end of its life; negative: "
                        "unlimited.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, useSpawnTexcoords, false,
                        "Use the texture coordinates of the emitter mesh when spawning particles.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, alignParticlesToVelocity, false,
                        "Rotates the particles such that they are always aligned with their direction of travel.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, enableCollisionDetection, false,
                        "Enables particle collisions with the world.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, collisionRestitution, 0.5f,
                        "The fraction of velocity retained after a collision with scene geometry.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, collisionThickness, 5.f,
                        "The maximum penetration depth (in centimeters) at which a particle still collides.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, useTurbulence, false, "Enable turbulence simulation.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, turbulenceForce, 5.f,
                        "How much turbulence influences the velocity of a particle (centimeters per second squared).");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, turbulenceFrequency, 0.05f,
                        "Frequency (rate of change) of the turbulence forces.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, enableMotionTrail, false,
                        "Elongates the particle with respect to velocity (motion-blur like).");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, motionTrailMultiplier, 1.f,
                        "Motion trail length multiplier.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", int, billboardType, 0,
                        "Billboard type: 0 classic, 1 cylindrical (up axis locked), 2 face camera position, 3 face "
                        "world up.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", int, spriteSheetMode, 0,
                        "Sprite sheet mode: 0 material sprite sheet, 1 animate over the lifetime, 2 random sprite.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", Vec3f, attractorPosition, Vec3f(0.f),
                        "The position in world space of the attractor.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, attractorForce, 0.f,
                        "How strongly the particles are attracted to the attractor position (cm/s^2).");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, attractorRadius, 0.f,
                        "Tunable falloff for the attractor.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", int, collisionMode, 0,
                        "Collision mode: 0 bounce, 1 stop, 2 kill.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, dragCoefficient, 0.f,
                        "Slows particles down over time, like air resistance.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, restrictVelocityX, false, "Restricts particle velocity in X.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, restrictVelocityY, false, "Restricts particle velocity in Y.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", bool, restrictVelocityZ, false, "Restricts particle velocity in Z.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", int, randomFlipAxis, 0,
                        "Random flip on spawn: 0 none, 1 horizontal, 2 vertical, 3 both.");
    FUSE_RELIGHT_OPTION("rtx.particles.globalPreset", float, initialRotationDeviationDegrees, 0.f,
                        "Range of degrees to rotate each particle by on spawn.");

    /// rtx.sceneScale (1 when absent).
    static float sceneScale();
    /// rtx.resolveTransparencyThreshold (1/255 when absent).
    static float resolveTransparencyThreshold();
    /// The scene up axis from rtx.zUp: (0, 0, 1) when set, else (0, 1, 0).
    static Vec3f sceneUp();
};

/// References every option above (static libraries: keeps the registrations linked).
void registerParticleOptions();

} // namespace fuse::relight::particles
