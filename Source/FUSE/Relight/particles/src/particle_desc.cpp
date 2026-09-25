/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_particle_system.cpp@0867d3c (createGlobalParticleSystemDesc, the
// animation-table fill of allocStaticBuffers, the index topology)

// FUSE Relight RL-3.6: particle system descriptions (see particle_desc.hpp).
#include <fuse/relight/particles/particle_desc.hpp>

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/particles/particle_kernels.hpp>
#include <fuse/relight/particles/particle_options.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::particles {

ParticleSystemDesc schemaDefaultDesc() {
    ParticleSystemDesc d;
    GpuSystemDesc& g = d.gpu;
    g = GpuSystemDesc{};
    g.maxNumParticles = 10000;
    g.spawnRatePerSecond = 0.f;
    g.spawnBurstDuration = 0.f;
    g.minTimeToLive = 1.f;
    g.maxTimeToLive = 1.f;
    g.turbulenceForce = 5.f;
    g.turbulenceFrequency = 0.05f;
    g.collisionRestitution = 0.5f;
    g.collisionThickness = 5.f;
    g.motionTrailMultiplier = 1.f;
    g.billboardType = static_cast<u32>(BillboardType::FaceCameraSpherical);
    g.spriteSheetMode = static_cast<u32>(SpriteSheetMode::UseMaterialSpriteSheet);
    g.collisionMode = static_cast<u32>(CollisionMode::Bounce);
    g.randomFlipAxis = static_cast<u32>(RandomFlipAxis::None);
    g.spriteSheetRows = 0;
    g.spriteSheetCols = 0;
    d.minColor = {{1.f, 1.f, 1.f, 1.f}, {1.f, 1.f, 1.f, 0.f}};
    d.maxColor = {{1.f, 1.f, 1.f, 1.f}, {1.f, 1.f, 1.f, 0.f}};
    d.minSize = {{10.f, 10.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
    d.maxSize = {{10.f, 10.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
    d.minRotationSpeed = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
    d.maxRotationSpeed = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
    d.maxVelocity = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}};
    return d;
}

ParticleSystemDesc globalPresetDesc() {
    using O = ParticleOptions;
    ParticleSystemDesc d;
    GpuSystemDesc& g = d.gpu;
    g = GpuSystemDesc{};
    g.initialVelocityFromMotion = O::initialVelocityFromMotion();
    g.initialVelocityFromNormal = O::initialVelocityFromNormal();
    g.initialVelocityConeAngleDegrees = O::initialVelocityConeAngleDegrees();
    g.gravityForce = O::gravityForce();
    g.turbulenceFrequency = O::turbulenceFrequency();
    g.turbulenceForce = O::turbulenceForce();
    g.minTimeToLive = O::minParticleLife();
    g.maxTimeToLive = O::maxParticleLife();
    g.maxNumParticles = static_cast<u32>(std::max(0, O::numberOfParticlesPerMaterial()));
    g.minSpawnRotationSpeed = O::minSpawnRotationSpeed();
    g.collisionRestitution = O::collisionRestitution();
    g.collisionThickness = O::collisionThickness();
    g.motionTrailMultiplier = O::motionTrailMultiplier();
    g.spawnRatePerSecond = static_cast<float>(O::spawnRatePerSecond());
    g.billboardType = static_cast<u32>(std::clamp(O::billboardType(), 0, 3));
    g.spriteSheetMode = static_cast<u32>(std::clamp(O::spriteSheetMode(), 0, 2));
    g.spriteSheetCols = 1;
    g.spriteSheetRows = 1;
    g.collisionMode = static_cast<u32>(std::clamp(O::collisionMode(), 0, 2));
    g.attractorPosition[0] = O::attractorPosition().x;
    g.attractorPosition[1] = O::attractorPosition().y;
    g.attractorPosition[2] = O::attractorPosition().z;
    g.attractorForce = O::attractorForce();
    g.attractorRadius = O::attractorRadius();
    g.dragCoefficient = O::dragCoefficient();
    g.spawnBurstDuration = O::spawnBurstDuration();
    g.randomFlipAxis = static_cast<u32>(std::clamp(O::randomFlipAxis(), 0, 3));
    g.initialRotationDeviationDegrees = O::initialRotationDeviationDegrees();
    u32 flags = 0;
    flags |= O::alignParticlesToVelocity() ? kFlagAlignParticlesToVelocity : 0u;
    flags |= O::useTurbulence() ? kFlagUseTurbulence : 0u;
    flags |= O::useSpawnTexcoords() ? kFlagUseSpawnTexcoords : 0u;
    flags |= O::enableCollisionDetection() ? kFlagEnableCollisionDetection : 0u;
    flags |= O::enableMotionTrail() ? kFlagEnableMotionTrail : 0u;
    flags |= O::restrictVelocityX() ? kFlagRestrictVelocityX : 0u;
    flags |= O::restrictVelocityY() ? kFlagRestrictVelocityY : 0u;
    flags |= O::restrictVelocityZ() ? kFlagRestrictVelocityZ : 0u;
    g.flags = flags;
    const auto v4 = [](const options::Vec4f& v) { return std::array<float, 4>{v.x, v.y, v.z, v.w}; };
    d.minColor = {v4(O::minSpawnColor()), v4(O::minTargetColor())};
    d.maxColor = {v4(O::maxSpawnColor()), v4(O::maxTargetColor())};
    d.minSize = {{O::minSpawnSize().x, O::minSpawnSize().y, 0.f, 0.f}, {O::minTargetSize().x, O::minTargetSize().y, 0.f, 0.f}};
    d.maxSize = {{O::maxSpawnSize().x, O::maxSpawnSize().y, 0.f, 0.f}, {O::maxTargetSize().x, O::maxTargetSize().y, 0.f, 0.f}};
    d.minRotationSpeed = {{O::minSpawnRotationSpeed(), 0.f, 0.f, 0.f}, {O::minTargetRotationSpeed(), 0.f, 0.f, 0.f}};
    d.maxRotationSpeed = {{O::maxSpawnRotationSpeed(), 0.f, 0.f, 0.f}, {O::maxTargetRotationSpeed(), 0.f, 0.f, 0.f}};
    const options::Vec3f s = O::maxSpawnVelocity();
    const options::Vec3f t = O::maxTargetVelocity();
    d.maxVelocity = {{s.x, s.y, s.z, 0.f}, {t.x, t.y, t.z, 0.f}};
    return d;
}

std::uint64_t hashDesc(const ParticleSystemDesc& desc) {
    std::uint64_t h = hash::xxh64(&desc.gpu, sizeof(desc.gpu), 0);
    for (const Channel* c : {&desc.minColor, &desc.maxColor, &desc.minSize, &desc.maxSize, &desc.minRotationSpeed,
                             &desc.maxRotationSpeed, &desc.maxVelocity}) {
        const std::uint64_t n = c->size();
        h = hash::xxh64(&n, sizeof(n), h);
        if (!c->empty()) {
            h = hash::xxh64(c->data(), c->size() * sizeof((*c)[0]), h);
        }
    }
    return h;
}

namespace {

/// upstream sampleAnimation: lerp over the keyframes at normalized position u.
std::array<float, 4> sampleChannel(const Channel& data, float u, const std::array<float, 4>& fallback) {
    if (data.empty()) {
        return fallback;
    }
    if (data.size() == 1) {
        return data[0];
    }
    const float pos = u * static_cast<float>(data.size() - 1);
    const std::size_t i0 = std::min(static_cast<std::size_t>(pos), data.size() - 1);
    const std::size_t i1 = std::min(i0 + 1, data.size() - 1);
    const float t = pos - static_cast<float>(i0);
    std::array<float, 4> out{};
    for (int c = 0; c < 4; ++c) {
        out[c] = data[i0][c] + (data[i1][c] - data[i0][c]) * t;
    }
    return out;
}

} // namespace

void buildAnimationTable(const ParticleSystemDesc& desc, Float4* out) {
    struct Row {
        const Channel* channel;
        std::array<float, 4> fallback;
        int components;
    };
    const Row rows[kAnimationRowCount] = {
        {&desc.minColor, {1.f, 1.f, 1.f, 1.f}, 4},        {&desc.maxColor, {1.f, 1.f, 1.f, 1.f}, 4},
        {&desc.minSize, {1.f, 1.f, 0.f, 0.f}, 2},         {&desc.maxSize, {1.f, 1.f, 0.f, 0.f}, 2},
        {&desc.minRotationSpeed, {0.f, 0.f, 0.f, 0.f}, 1}, {&desc.maxRotationSpeed, {0.f, 0.f, 0.f, 0.f}, 1},
        {&desc.maxVelocity, {0.f, 0.f, 0.f, 0.f}, 3},
    };
    for (u32 r = 0; r < kAnimationRowCount; ++r) {
        for (u32 x = 0; x < kAnimationWidth; ++x) {
            // Normalized life is 1 at birth and 0 at death: texel x holds age 1 - x / (width - 1).
            const float u = 1.f - static_cast<float>(x) / static_cast<float>(kAnimationWidth - 1u);
            const std::array<float, 4> v = sampleChannel(*rows[r].channel, u, rows[r].fallback);
            Float4& o = out[r * kAnimationWidth + x];
            for (int c = 0; c < 4; ++c) {
                o.v[c] = c < rows[r].components ? v[c] : 0.f;
            }
        }
    }
}

std::uint32_t verticesPerParticle(const ParticleSystemDesc& desc) { return desc.motionTrail() ? 8u : 4u; }
std::uint32_t indicesPerParticle(const ParticleSystemDesc& desc) { return desc.motionTrail() ? 18u : 6u; }

void vertexOffsets(const ParticleSystemDesc& desc, float* out16) {
    std::fill(out16, out16 + 16, 0.f);
    if (desc.motionTrail()) {
        std::copy(std::begin(kernels::kTrailOffsets), std::end(kernels::kTrailOffsets), out16);
    } else {
        std::copy(std::begin(kernels::kQuadOffsets), std::end(kernels::kQuadOffsets), out16);
    }
}

void buildIndices(const ParticleSystemDesc& desc, std::uint32_t particles, std::vector<std::uint32_t>& out) {
    const std::uint32_t vpp = verticesPerParticle(desc);
    const std::uint32_t ipp = indicesPerParticle(desc);
    out.assign(static_cast<std::size_t>(particles) * ipp, 0u);
    static constexpr std::uint32_t kQuad[6] = {0, 1, 2, 2, 1, 3};
    static constexpr std::uint32_t kTrail[12] = {1, 4, 3, 3, 4, 6, 4, 5, 6, 6, 5, 7};
    for (std::uint32_t i = 0; i < particles; ++i) {
        std::uint32_t* o = out.data() + static_cast<std::size_t>(i) * ipp;
        for (std::uint32_t k = 0; k < 6; ++k) {
            o[k] = i * vpp + kQuad[k];
        }
        if (desc.motionTrail()) {
            for (std::uint32_t k = 0; k < 12; ++k) {
                o[6 + k] = i * vpp + kTrail[k];
            }
        }
    }
}

} // namespace fuse::relight::particles
