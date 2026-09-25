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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_particle_system.cpp@0867d3c

// FUSE Relight RL-3.6: the particle system manager and the CPU backend (see particle_system.hpp).
//
// Control flow ported from dxvk-remix src/dxvk/rtx_render/rtx_particle_system.cpp @0867d3c.
//
// Modifications (FUSE): pooled buffers for every system instead of per-system GPU buffers; the conservative counter
// reads the retirements of frame f - framesInFlight exactly once (upstream: a host ring read whenever ready); the
// Poisson draw is Knuth's over the deterministic hash stream with a libm-free exp (upstream:
// std::poisson_distribution over std::mt19937), so counts are identical with every C runtime; the first burst of a
// burst-mode system fires at once (upstream's stated intent; its creation stamp delayed it by one interval); a
// constant-count system respawns only on frames with a spawn request (upstream re-dispatched every slot with a stale
// spawn map on frames without one); spawn contexts carry the emitter mesh and transforms directly (upstream: an
// instance index resolved at simulate time).
#include <fuse/relight/particles/particle_system.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/relight/particles/particle_kernels.hpp>
#include <fuse/relight/particles/particle_options.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::relight::particles {

Mat34 identity34() { return Mat34{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f}; }

Mat34 fromRowVector44(const std::array<double, 16>& m) {
    // world = p * M (row vector): world.x = p.x m[0] + p.y m[4] + p.z m[8] + m[12].
    Mat34 r{};
    for (int row = 0; row < 3; ++row) {
        r[row * 4 + 0] = static_cast<float>(m[0 + row]);
        r[row * 4 + 1] = static_cast<float>(m[4 + row]);
        r[row * 4 + 2] = static_cast<float>(m[8 + row]);
        r[row * 4 + 3] = static_cast<float>(m[12 + row]);
    }
    return r;
}

FrameInput FrameInput::fromOptions() {
    FrameInput in;
    const options::Vec3f up = ParticleOptions::sceneUp();
    in.up = {up.x, up.y, up.z};
    in.sceneScale = ParticleOptions::sceneScale();
    in.resolveTransparencyThreshold = ParticleOptions::resolveTransparencyThreshold();
    return in;
}

struct ParticleSystemManager::System {
    bool active = false;
    std::uint64_t key = 0;
    std::uint64_t descHash = 0;
    std::uint64_t materialKey = 0;
    ParticleSystemDesc desc;
    std::uint32_t seed = 0;
    std::uint32_t particleBase = 0;
    std::uint32_t vertexBase = 0;
    std::uint32_t vpp = 4;
    std::uint32_t head = 0, spawnOffset = 0, spawnCount = 0, particleCount = 0, tail = 0, simulateCount = 0;
    std::int64_t cachedTotal = 0;
    std::uint32_t generation = 0;
    std::uint64_t lastSpawnTimeMs = 0;
    bool burstStarted = false;
    std::uint64_t createdSerial = 0;
    std::uint32_t rngCounter = 0;
};

namespace {

std::uint64_t systemKey(std::uint64_t descHash, std::uint64_t materialKey) {
    return descHash ^ ((materialKey << 31) | (materialKey >> 33)) ^ 0x9E3779B97F4A7C15ull;
}

/// Uniform [0, 1) of the system's host stream.
float hostUniform(std::uint32_t seed, std::uint32_t& counter) {
    return kernels::unorm23ToFloat(kernels::uintHash3(counter++, seed, 0x51A7u));
}

/// exp(x) for x <= 0 from IEEE add / mul / div and ldexp only (range reduction by ln 2, Taylor series), so the host
/// draws are the same bits with every C runtime (glibc, MinGW).
double portableExpNeg(double x) {
    constexpr double kLn2 = 0.6931471805599453094;
    const double nf = std::floor(x / kLn2 + 0.5);
    const double r = x - nf * kLn2; // |r| <= ln 2 / 2
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 24; ++i) {
        term = term * r / static_cast<double>(i);
        sum += term;
    }
    return std::ldexp(sum, static_cast<int>(nf));
}

/// Poisson(lambda) from the deterministic stream: Knuth's product of uniforms against exp(-lambda), in chunks of
/// lambda <= 256 (exp(-256) stays a normal double). Upstream uses std::poisson_distribution over std::mt19937.
std::uint32_t poisson(float lambda, std::uint32_t seed, std::uint32_t& counter) {
    if (!(lambda > 0.f)) {
        return 0;
    }
    double remaining = std::min(static_cast<double>(lambda), 1.0e6);
    std::uint32_t total = 0;
    while (remaining > 0.0) {
        const double chunk = std::min(remaining, 256.0);
        remaining -= chunk;
        const double limit = portableExpNeg(-chunk);
        double p = 1.0;
        std::uint32_t k = 0;
        do {
            ++k;
            p *= static_cast<double>(hostUniform(seed, counter));
        } while (p > limit && k < 100000u);
        total += k - 1u;
    }
    return total;
}

} // namespace

ParticleSystemManager::ParticleSystemManager(ManagerConfig config) : m_config(config) {
    m_config.framesInFlight = std::max(1u, m_config.framesInFlight);
    m_systems.resize(m_config.maxSystems);
    m_byKey.reserve(m_config.maxSystems * 2u);
    m_particleFree = {{0u, m_config.particleCapacity}};
    m_vertexFree = {{0u, m_config.vertexCapacity}};
    m_particleFree.reserve(m_config.maxSystems + 2u);
    m_vertexFree.reserve(m_config.maxSystems + 2u);
    m_constants.assign(m_config.maxSystems, GpuFrameConstants{});
    m_active.reserve(m_config.maxSystems);
    m_spawnContexts.assign(m_config.maxSpawnContexts, GpuSpawnContext{});
    m_spawnMap.assign(m_config.particleCapacity, 0u);
    m_positions.assign(static_cast<std::size_t>(m_config.geometryVertexCapacity) * 3u, 0.f);
    m_colors.assign(m_config.geometryVertexCapacity, 0u);
    m_texcoords.assign(static_cast<std::size_t>(m_config.geometryVertexCapacity) * 2u, 0.f);
    m_indices.assign(m_config.geometryIndexCapacity, 0u);
    m_animation.assign(static_cast<std::size_t>(m_config.maxSystems) * kAnimationTexels, Float4{});
    m_animationDirty.reserve(m_config.maxSystems);
    m_clears.reserve(m_config.maxSystems);
    m_retired.assign(m_config.maxSystems, 0u);
    m_draws.reserve(m_config.maxSystems);
    m_meshes.reserve(256);
}

ParticleSystemManager::~ParticleSystemManager() = default;

EmitterMeshId ParticleSystemManager::registerMesh(const EmitterMesh& mesh) {
    const std::size_t vertices = mesh.positions.size() / 3u;
    const std::size_t indices = mesh.indices.size() - mesh.indices.size() % 3u;
    if (vertices == 0 || indices == 0) {
        return kInvalidMesh;
    }
    const bool prev = mesh.prevPositions.size() == mesh.positions.size();
    const std::size_t needVertices = vertices * (prev ? 2u : 1u);
    if (m_geoVertices + needVertices > m_config.geometryVertexCapacity || m_geoIndices + indices > m_config.geometryIndexCapacity) {
        return kInvalidMesh;
    }
    for (std::size_t i = 0; i < indices; ++i) {
        if (mesh.indices[i] >= vertices) {
            return kInvalidMesh;
        }
    }
    MeshRecord r;
    r.vertexOffset = m_geoVertices;
    r.prevVertexOffset = prev ? m_geoVertices + static_cast<std::uint32_t>(vertices) : m_geoVertices;
    r.indexOffset = m_geoIndices;
    r.triangleCount = static_cast<std::uint32_t>(indices / 3u);
    std::copy(mesh.positions.begin(), mesh.positions.begin() + static_cast<std::ptrdiff_t>(vertices * 3u),
              m_positions.begin() + static_cast<std::ptrdiff_t>(r.vertexOffset) * 3);
    if (prev) {
        std::copy(mesh.prevPositions.begin(), mesh.prevPositions.end(),
                  m_positions.begin() + static_cast<std::ptrdiff_t>(r.prevVertexOffset) * 3);
    }
    if (mesh.colors.size() >= vertices) {
        std::copy(mesh.colors.begin(), mesh.colors.begin() + static_cast<std::ptrdiff_t>(vertices),
                  m_colors.begin() + r.vertexOffset);
        r.flags |= kSpawnHasColors;
    }
    if (mesh.texcoords.size() >= vertices * 2u) {
        std::copy(mesh.texcoords.begin(), mesh.texcoords.begin() + static_cast<std::ptrdiff_t>(vertices * 2u),
                  m_texcoords.begin() + static_cast<std::ptrdiff_t>(r.vertexOffset) * 2);
        r.flags |= kSpawnHasTexcoords;
    }
    std::copy(mesh.indices.begin(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(indices), m_indices.begin() + r.indexOffset);
    if (m_dirtyVertexEnd == m_dirtyVertexBegin) {
        m_dirtyVertexBegin = m_geoVertices;
        m_dirtyIndexBegin = m_geoIndices;
    }
    m_geoVertices += static_cast<std::uint32_t>(needVertices);
    m_geoIndices += static_cast<std::uint32_t>(indices);
    m_dirtyVertexEnd = m_geoVertices;
    m_dirtyIndexEnd = m_geoIndices;
    m_meshes.push_back(r);
    return static_cast<EmitterMeshId>(m_meshes.size() - 1u);
}

std::uint32_t ParticleSystemManager::meshTriangles(EmitterMeshId id) const {
    return id < m_meshes.size() ? m_meshes[id].triangleCount : 0u;
}

bool ParticleSystemManager::allocRange(std::vector<Range>& freeList, std::uint32_t count, std::uint32_t& base) {
    for (std::size_t i = 0; i < freeList.size(); ++i) {
        if (freeList[i].count >= count) {
            base = freeList[i].base;
            freeList[i].base += count;
            freeList[i].count -= count;
            if (freeList[i].count == 0) {
                freeList.erase(freeList.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return true;
        }
    }
    return false;
}

void ParticleSystemManager::freeRange(std::vector<Range>& freeList, std::uint32_t base, std::uint32_t count) {
    auto it = std::lower_bound(freeList.begin(), freeList.end(), base, [](const Range& r, std::uint32_t b) { return r.base < b; });
    it = freeList.insert(it, Range{base, count});
    // Merge with the next, then the previous range.
    if (it + 1 != freeList.end() && it->base + it->count == (it + 1)->base) {
        it->count += (it + 1)->count;
        freeList.erase(it + 1);
    }
    if (it != freeList.begin() && (it - 1)->base + (it - 1)->count == it->base) {
        (it - 1)->count += it->count;
        freeList.erase(it);
    }
}

std::int32_t ParticleSystemManager::createSystem(const ParticleSystemDesc& desc, std::uint64_t descHash, std::uint64_t materialKey) {
    std::int32_t slot = -1;
    for (std::uint32_t i = 0; i < m_systems.size(); ++i) {
        if (!m_systems[i].active) {
            slot = static_cast<std::int32_t>(i);
            break;
        }
    }
    const std::uint32_t max = desc.gpu.maxNumParticles;
    const std::uint32_t vpp = verticesPerParticle(desc);
    std::uint32_t pBase = 0, vBase = 0;
    if (slot < 0 || !allocRange(m_particleFree, max, pBase)) {
        return -1;
    }
    if (!allocRange(m_vertexFree, max * vpp, vBase)) {
        freeRange(m_particleFree, pBase, max);
        return -1;
    }
    System& s = m_systems[static_cast<std::uint32_t>(slot)];
    s = System{};
    s.active = true;
    s.key = systemKey(descHash, materialKey);
    s.descHash = descHash;
    s.materialKey = materialKey;
    s.desc = desc;
    s.seed = kernels::uintHash(m_systemCounter++ + 1u);
    s.particleBase = pBase;
    s.vertexBase = vBase;
    s.vpp = vpp;
    s.lastSpawnTimeMs = m_nowMs;
    s.createdSerial = m_serial;
    buildAnimationTable(desc, m_animation.data() + static_cast<std::size_t>(slot) * kAnimationTexels);
    m_animationDirty.push_back(static_cast<std::uint32_t>(slot));
    m_clears.push_back({pBase, max});
    m_byKey[s.key] = static_cast<std::uint32_t>(slot);
    return slot;
}

void ParticleSystemManager::destroySystem(std::uint32_t slot) {
    System& s = m_systems[slot];
    if (!s.active) {
        return;
    }
    m_byKey.erase(s.key);
    freeRange(m_particleFree, s.particleBase, s.desc.gpu.maxNumParticles);
    freeRange(m_vertexFree, s.vertexBase, s.desc.gpu.maxNumParticles * s.vpp);
    s.active = false;
}

void ParticleSystemManager::beginFrame(const FrameInput& input, ParticleBackend& backend) {
    m_input = input;
    m_frameOpen = true;
    m_enable = ParticleOptions::enable();
    m_enableSpawning = ParticleOptions::enableSpawning();
    m_timeScale = ParticleOptions::timeScale();
    m_nowMs = static_cast<std::uint64_t>(std::max(0.0, std::floor(input.absoluteTimeSecs * 1000.0)));
    // Retirements of frame serial - framesInFlight (its fence has passed: the backend waited before reusing the
    // counter slot). Systems created after that frame ignore the slot's stale count.
    if (m_serial >= m_config.framesInFlight) {
        const std::uint64_t done = m_serial - m_config.framesInFlight;
        backend.readRetirements(done, m_retired.data(), m_config.maxSystems);
        for (std::uint32_t i = 0; i < m_systems.size(); ++i) {
            System& s = m_systems[i];
            if (s.active && s.createdSerial <= done && !s.desc.constantCount()) {
                s.cachedTotal -= m_retired[i];
                s.cachedTotal = std::max<std::int64_t>(s.cachedTotal, 0);
            }
        }
    }
}

std::uint32_t ParticleSystemManager::spawnCountFor(System& s) {
    const GpuSystemDesc& d = s.desc.gpu;
    const float rate = d.spawnRatePerSecond;
    const float burst = d.spawnBurstDuration;
    float elapsed = 0.f;
    if (burst <= 0.f) {
        elapsed = m_input.deltaTimeSecs;
    } else {
        const std::uint64_t burstMs = static_cast<std::uint64_t>(burst * 1000.f);
        // The first burst fires at once (upstream's intent: lastSpawnTimeMs == 0 "pretends one full interval
        // elapsed"; its constructor stamps the creation time, so in practice the first burst waited an interval).
        const std::uint64_t elapsedMs = !s.burstStarted ? burstMs : m_nowMs - s.lastSpawnTimeMs;
        if (elapsedMs < burstMs) {
            return 0;
        }
        elapsed = std::min(static_cast<float>(elapsedMs) * 0.001f, burst * 4.f);
        s.lastSpawnTimeMs = m_nowMs;
        s.burstStarted = true;
    }
    const float lambda = rate * elapsed;
    if (std::isnan(lambda) || lambda <= 0.f) {
        return 0;
    }
    std::uint32_t n = std::min(poisson(lambda, s.seed, s.rngCounter), d.maxNumParticles);
    if (s.particleCount + s.spawnCount + n >= d.maxNumParticles) {
        return 0;
    }
    // No wrap-around inside a frame: the host would otherwise run ahead of the ring.
    if (s.head + n >= d.maxNumParticles) {
        n = d.maxNumParticles - s.head;
    }
    return n;
}

SpawnResult ParticleSystemManager::spawn(const SpawnRequest& request) {
    SpawnResult r;
    if (!m_frameOpen || !m_enable || !m_enableSpawning || request.desc == nullptr || request.desc->gpu.maxNumParticles == 0u) {
        return r;
    }
    if (request.mesh >= m_meshes.size() || m_meshes[request.mesh].triangleCount == 0u) {
        return r;
    }
    const std::uint64_t descHash = request.descHash != 0u ? request.descHash : hashDesc(*request.desc);
    const auto it = m_byKey.find(systemKey(descHash, request.materialKey));
    std::int32_t slot = it != m_byKey.end() ? static_cast<std::int32_t>(it->second) : createSystem(*request.desc, descHash, request.materialKey);
    if (slot < 0) {
        return r;
    }
    System& s = m_systems[static_cast<std::uint32_t>(slot)];
    r.system = slot;
    const GpuSystemDesc& d = s.desc.gpu;
    std::uint32_t n = s.desc.constantCount() ? d.maxNumParticles - s.spawnCount : spawnCountFor(s);
    if (n == 0u || m_spawnContextCount >= m_config.maxSpawnContexts) {
        return r;
    }
    const MeshRecord& mesh = m_meshes[request.mesh];
    GpuSpawnContext& ctx = m_spawnContexts[m_spawnContextCount];
    std::memcpy(ctx.objectToWorld, request.objectToWorld.data(), sizeof(ctx.objectToWorld));
    std::memcpy(ctx.prevObjectToWorld, request.prevObjectToWorld.data(), sizeof(ctx.prevObjectToWorld));
    ctx.indexOffset = mesh.indexOffset;
    ctx.triangleCount = mesh.triangleCount;
    ctx.vertexOffset = mesh.vertexOffset;
    ctx.prevVertexOffset = mesh.prevVertexOffset;
    ctx.flags = mesh.flags;
    ctx.pad0 = ctx.pad1 = ctx.pad2 = 0u;
    std::fill_n(m_spawnMap.begin() + s.particleBase + s.spawnCount, n, m_spawnContextCount);
    ++m_spawnContextCount;
    s.head += n;
    s.spawnCount += n;
    s.lastSpawnTimeMs = m_nowMs;
    r.particles = n;
    return r;
}

bool ParticleSystemManager::simulate(ParticleBackend& backend) {
    if (!m_frameOpen) {
        return false;
    }
    m_frameOpen = false;
    m_draws.clear();
    m_active.clear();
    if (!m_enable) {
        for (std::uint32_t i = 0; i < m_systems.size(); ++i) {
            destroySystem(i);
        }
        m_spawnContextCount = 0;
        ++m_serial;
        return true;
    }
    const float dt = std::min(kernels::minimumParticleLife(), m_input.deltaTimeSecs) * m_timeScale;
    const float invDt = dt > 0.f ? 1.f / dt : 0.f;
    for (std::uint32_t i = 0; i < m_systems.size(); ++i) {
        System& s = m_systems[i];
        if (!s.active) {
            continue;
        }
        const GpuSystemDesc& d = s.desc.gpu;
        const std::uint32_t max = d.maxNumParticles;
        if (s.desc.constantCount()) {
            s.simulateCount = max;
            s.particleCount = max;
            s.head = max;
            s.tail = 0;
            s.spawnOffset = 0;
        } else {
            s.cachedTotal += s.spawnCount;
            s.cachedTotal = std::min<std::int64_t>(s.cachedTotal, max);
            s.particleCount = static_cast<std::uint32_t>(s.cachedTotal);
            s.tail = (s.head + max - s.particleCount % max) % max;
            s.simulateCount = s.particleCount - std::min(s.spawnCount, s.particleCount);
        }
        GpuFrameConstants& c = m_constants[i];
        c = GpuFrameConstants{};
        c.desc = d;
        std::copy(m_input.viewToWorld.begin(), m_input.viewToWorld.end(), c.viewToWorld);
        std::copy(m_input.prevWorldToProjection.begin(), m_input.prevWorldToProjection.end(), c.prevWorldToProjection);
        c.upDirection[0] = m_input.up[0];
        c.upDirection[1] = m_input.up[1];
        c.upDirection[2] = m_input.up[2];
        c.deltaTimeSecs = dt;
        c.absoluteTimeSecs = static_cast<float>(m_input.absoluteTimeSecs * static_cast<double>(m_timeScale));
        c.invDeltaTimeSecs = invDt;
        c.frameIdx = m_input.frameIndex;
        c.systemSeed = s.seed;
        c.renderingWidth = m_input.renderWidth;
        c.renderingHeight = m_input.renderHeight;
        c.resolveTransparencyThreshold = m_input.resolveTransparencyThreshold;
        c.minParticleSize = m_input.minParticleSize;
        c.sceneScale = m_input.sceneScale;
        c.particleBase = s.particleBase;
        c.vertexBase = s.vertexBase;
        c.animationBase = i * kAnimationTexels;
        c.spawnMapBase = s.particleBase;
        c.spawnParticleOffset = s.spawnOffset;
        c.spawnParticleCount = s.spawnCount;
        c.particleTailOffset = s.tail;
        c.simulateParticleCount = s.simulateCount;
        c.particleCount = s.particleCount;
        c.verticesPerParticle = s.vpp;
        c.counterIndex = i;
        vertexOffsets(s.desc, c.vertexOffsets);
        m_active.push_back(i);
        SystemDraw draw;
        draw.system = i;
        draw.descHash = s.descHash;
        draw.materialKey = s.materialKey;
        draw.vertexBase = s.vertexBase;
        draw.vertexCount = s.particleCount * s.vpp;
        draw.verticesPerParticle = s.vpp;
        draw.generation = s.generation;
        m_draws.push_back(draw);
    }

    FrameWork work;
    work.serial = m_serial;
    work.framesInFlight = m_config.framesInFlight;
    work.constants = m_constants;
    work.activeSystems = m_active;
    work.spawnContexts = std::span<const GpuSpawnContext>(m_spawnContexts.data(), m_spawnContextCount);
    work.spawnMap = m_spawnMap;
    work.positions = m_positions;
    work.colors = m_colors;
    work.texcoords = m_texcoords;
    work.indices = m_indices;
    work.animation = m_animation;
    work.geometryVertexBegin = m_dirtyVertexBegin;
    work.geometryVertexEnd = m_dirtyVertexEnd;
    work.geometryIndexBegin = m_dirtyIndexBegin;
    work.geometryIndexEnd = m_dirtyIndexEnd;
    work.animationDirty = m_animationDirty;
    work.clears = m_clears;
    const bool ok = backend.execute(work);
    m_dirtyVertexBegin = m_dirtyVertexEnd = m_geoVertices;
    m_dirtyIndexBegin = m_dirtyIndexEnd = m_geoIndices;
    m_animationDirty.clear();
    m_clears.clear();
    prepareForNextFrame();
    ++m_serial;
    return ok;
}

void ParticleSystemManager::prepareForNextFrame() {
    m_spawnContextCount = 0;
    for (std::uint32_t i = 0; i < m_systems.size(); ++i) {
        System& s = m_systems[i];
        if (!s.active) {
            continue;
        }
        const GpuSystemDesc& d = s.desc.gpu;
        const std::uint64_t idleMs = static_cast<std::uint64_t>((d.spawnBurstDuration + d.maxTimeToLive) * 1000.f);
        if (s.lastSpawnTimeMs + idleMs < m_nowMs) {
            destroySystem(i);
            continue;
        }
        ++s.generation;
        s.spawnOffset = s.head;
        s.spawnCount = 0;
        if (s.head >= d.maxNumParticles) {
            s.head = 0;
        }
    }
}

std::uint32_t ParticleSystemManager::activeSystemCount() const {
    std::uint32_t n = 0;
    for (const System& s : m_systems) {
        n += s.active ? 1u : 0u;
    }
    return n;
}

std::int32_t ParticleSystemManager::findSystem(std::uint64_t descHash, std::uint64_t materialKey) const {
    const auto it = m_byKey.find(systemKey(descHash, materialKey));
    return it == m_byKey.end() ? -1 : static_cast<std::int32_t>(it->second);
}

const ParticleSystemDesc* ParticleSystemManager::systemDesc(std::uint32_t slot) const {
    return slot < m_systems.size() && m_systems[slot].active ? &m_systems[slot].desc : nullptr;
}

SystemCounters ParticleSystemManager::counters(std::uint32_t slot) const {
    SystemCounters c;
    if (slot >= m_systems.size()) {
        return c;
    }
    const System& s = m_systems[slot];
    c.head = s.head;
    c.spawnOffset = s.spawnOffset;
    c.spawnCount = s.spawnCount;
    c.particleCount = s.particleCount;
    c.tail = s.tail;
    c.simulateCount = s.simulateCount;
    c.cachedTotal = s.cachedTotal;
    c.generation = s.generation;
    return c;
}

std::uint32_t ParticleSystemManager::particleBase(std::uint32_t slot) const {
    return slot < m_systems.size() ? m_systems[slot].particleBase : 0u;
}
std::uint32_t ParticleSystemManager::vertexBase(std::uint32_t slot) const {
    return slot < m_systems.size() ? m_systems[slot].vertexBase : 0u;
}

// ---- CPU backend ----------------------------------------------------------------------------------------------------

const char* CpuParticleBackend::name() const {
    return m_backend == kernel::Backend::CpuReference ? "cpu-reference" : "cpu-parallel";
}

bool CpuParticleBackend::init(const ManagerConfig& config) {
    m_config = config;
    m_config.framesInFlight = std::max(1u, m_config.framesInFlight);
    m_particles.assign(config.particleCapacity, GpuParticle{});
    m_vertices.assign(config.vertexCapacity, GpuParticleVertex{});
    m_counters.assign(static_cast<std::size_t>(m_config.framesInFlight) * config.maxSystems, 0u);
    return true;
}

bool CpuParticleBackend::execute(const FrameWork& work) {
    if (m_particles.empty()) {
        return false;
    }
    for (const auto& clear : work.clears) {
        std::fill_n(m_particles.begin() + clear[0], clear[1], GpuParticle{});
    }
    const std::size_t slot = static_cast<std::size_t>(work.serial % m_config.framesInFlight);
    std::uint32_t* counters = m_counters.data() + slot * m_config.maxSystems;
    std::fill_n(counters, m_config.maxSystems, 0u);

    kernels::Params p;
    p.constants = kernel::make_span(work.constants.data(), static_cast<u32>(work.constants.size()));
    p.particles = kernel::make_span(m_particles.data(), static_cast<u32>(m_particles.size()));
    p.spawnContexts = kernel::make_span(work.spawnContexts.data(), static_cast<u32>(work.spawnContexts.size()));
    p.spawnMap = kernel::make_span(work.spawnMap.data(), static_cast<u32>(work.spawnMap.size()));
    p.positions = kernel::make_span(work.positions.data(), static_cast<u32>(work.positions.size()));
    p.colors = kernel::make_span(work.colors.data(), static_cast<u32>(work.colors.size()));
    p.texcoords = kernel::make_span(work.texcoords.data(), static_cast<u32>(work.texcoords.size()));
    p.indices = kernel::make_span(work.indices.data(), static_cast<u32>(work.indices.size()));
    p.animation = kernel::make_span(work.animation.data(), static_cast<u32>(work.animation.size()));
    p.vertices = kernel::make_span(m_vertices.data(), static_cast<u32>(m_vertices.size()));
    p.counters = kernel::make_span(counters, m_config.maxSystems);

    bool ok = true;
    // Pass-major over the systems, as the GPU records them (one barrier between passes).
    for (std::uint32_t pass = 0; pass < 3u; ++pass) {
        for (const std::uint32_t sys : work.activeSystems) {
            const GpuFrameConstants& c = work.constants[sys];
            p.system = sys;
            kernel::LaunchResult r{};
            if (pass == 0u) {
                if (c.spawnParticleCount == 0u) {
                    continue;
                }
                r = kernel::launch(m_backend, kernel::KernelLaunch{kernels::kSpawnName, kernel::extent1(c.spawnParticleCount), kernels::kWorkgroup},
                                   kernels::SpawnKernel{}, p);
            } else if (pass == 1u) {
                if (c.simulateParticleCount == 0u) {
                    continue;
                }
                r = kernel::launch(m_backend,
                                   kernel::KernelLaunch{kernels::kEvolveName, kernel::extent1(c.simulateParticleCount), kernels::kWorkgroup},
                                   kernels::EvolveKernel{}, p);
            } else {
                r = kernel::launch(m_backend,
                                   kernel::KernelLaunch{kernels::kBillboardName, kernel::extent1(c.desc.maxNumParticles), kernels::kWorkgroup},
                                   kernels::BillboardKernel{}, p);
            }
            ok = ok && r.ok;
            m_lastExecuted = r.backend;
        }
    }
    return ok;
}

void CpuParticleBackend::readRetirements(std::uint64_t serial, std::uint32_t* out, std::uint32_t slots) {
    const std::size_t slot = static_cast<std::size_t>(serial % m_config.framesInFlight);
    const std::uint32_t n = std::min(slots, m_config.maxSystems);
    std::copy_n(m_counters.data() + slot * m_config.maxSystems, n, out);
}

} // namespace fuse::relight::particles
