// FUSE Relight RL-3.6: the runtime description of a particle system (upstream RtxParticleSystemDesc): the kernel-
// visible GpuSystemDesc plus the animation channels (keyframe lists, sampled over normalized age 0 = birth ..
// 1 = death). A USD ParticleSystemAPI prim (or an RL-3.2 relight_particles record) becomes one of these through
// particle_usd.hpp; draws tagged ParticleEmitter without one use globalPresetDesc() (rtx.particles.globalPreset.*).
//
// Semantics follow dxvk-remix rtx_particle_system.{h,cpp} and rtx_mod_usd.cpp processParticleSystem @0867d3c (MIT;
// facts: schema fallbacks, legacy spawn -> target fallback, animation-table sampling, index topology).
#pragma once

#include <fuse/relight/particles/particle_types.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace fuse::relight::particles {

using Channel = std::vector<std::array<float, 4>>;

struct ParticleSystemDesc {
    GpuSystemDesc gpu{};
    /// Animation channels (upstream minColor ... maxRotationSpeed): keyframes evenly spread over normalized age;
    /// empty = the channel's default (colour 1, size 1, rotation 0, velocity 0).
    Channel minColor, maxColor;             ///< RGBA
    Channel minSize, maxSize;               ///< x, y (centimetres)
    Channel minRotationSpeed, maxRotationSpeed; ///< x
    Channel maxVelocity;                    ///< x, y, z (<= 0: unlimited on that axis)

    bool hideEmitter() const { return (gpu.flags & kFlagHideEmitter) != 0u; }
    bool motionTrail() const { return (gpu.flags & kFlagEnableMotionTrail) != 0u; }
    /// Every slot respawns every frame (upstream isNumParticlesConstant): spawnRatePerSecond >= maxNumParticles.
    bool constantCount() const { return gpu.spawnRatePerSecond >= static_cast<float>(gpu.maxNumParticles); }
};

/// The ParticleSystemAPI schema's fallback values (RemixParticleSystem generatedSchema.usda), with the legacy
/// spawn / target pairs as two-key channels (minColor (1,1,1,1) -> (1,1,1,0), size 10 -> 0, rotation 0 -> 0,
/// velocity 0 -> 0).
ParticleSystemDesc schemaDefaultDesc();

/// upstream createGlobalParticleSystemDesc: rtx.particles.globalPreset.* (1x1 sprite sheet).
ParticleSystemDesc globalPresetDesc();

/// Content hash of a description (the GPU struct bytes, then every channel): the system key with the material key.
std::uint64_t hashDesc(const ParticleSystemDesc& desc);

/// Fills `out[kAnimationTexels]`: row r, texel x holds the channel at normalized age 1 - x / 255 lerped over its
/// keyframes (upstream allocStaticBuffers' animation texture, in f32).
void buildAnimationTable(const ParticleSystemDesc& desc, Float4* out);

std::uint32_t verticesPerParticle(const ParticleSystemDesc& desc);
std::uint32_t indicesPerParticle(const ParticleSystemDesc& desc);
/// The quad corner offsets of the kernels (8 x (x, y)).
void vertexOffsets(const ParticleSystemDesc& desc, float* out16);
/// The static triangle list of `particles` billboards (upstream index buffer).
void buildIndices(const ParticleSystemDesc& desc, std::uint32_t particles, std::vector<std::uint32_t>& out);

} // namespace fuse::relight::particles
