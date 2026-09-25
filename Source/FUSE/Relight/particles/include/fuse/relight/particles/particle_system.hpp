// FUSE Relight RL-3.6: the particle system manager (upstream RtxParticleSystemManager) and its CPU backend.
//
// ParticleSystemManager is the host side of the simulation, identical for every backend:
//   * systems are keyed by (description hash, material key) and created on the first spawn request (upstream
//     fetchParticleSystem); each owns a ring of maxNumParticles slots in the shared particle pool, a vertex range
//     (4 or 8 vertices per slot), a spawn-map range and an animation table;
//   * spawn(request) per emitter instance per frame (upstream spawnParticles): the Poisson-distributed count of
//     rate x dt (or bursts every spawnBurstDuration; every slot when spawnRatePerSecond >= maxNumParticles), capacity
//     and wrap guards, then a spawn context (emitter mesh + transforms) the new slots map to;
//   * simulate(backend) (upstream simulate): per system the conservative live count (spawns added on the host,
//     retirements counted by the evolve kernel), the ring's tail and the simulated range, the frame constants;
//     then the backend runs spawn, evolve and billboard over every system and the manager prepares the next frame
//     (retires idle systems, advances the ring, clears the spawn contexts).
//
// Retirement counts reach the host with a fixed latency of `framesInFlight` frames (the backend's counter slot of
// frame f is read at the start of frame f + framesInFlight, after that frame's fence), so the live count, the ring
// and every integer the kernels see are a pure function of the inputs: two managers fed the same calls produce the
// same integer state on any backend (upstream reads a host copy of the GPU counter at whatever point it is ready).
//
// Steady state is allocation-free: pools, the system table, spawn contexts and the backends' buffers are sized by
// ManagerConfig at construction; creating a system (a description seen for the first time) copies its description.
//
// Semantics follow dxvk-remix src/dxvk/rtx_render/rtx_particle_system.{h,cpp} @0867d3c (MIT: facts and control flow
// only; see particle_kernels.hpp for the ported shader code and the list of modifications).
#pragma once

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/relight/particles/particle_desc.hpp>
#include <fuse/relight/particles/particle_types.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace fuse::relight::particles {

struct ManagerConfig {
    u32 maxSystems = 64;
    u32 particleCapacity = 1u << 18;       ///< particles of every system together
    u32 vertexCapacity = 1u << 20;         ///< billboard vertices of every system together
    u32 maxSpawnContexts = 4096;           ///< emitter instances per frame
    u32 geometryVertexCapacity = 1u << 18; ///< emitter-mesh vertices (positions + previous positions)
    u32 geometryIndexCapacity = 3u << 18;  ///< emitter-mesh indices
    u32 framesInFlight = 2;                ///< retirement-count latency (>= 1)
};

/// An emitter mesh as the host registers it (copied into the geometry pool).
struct EmitterMesh {
    std::span<const float> positions;     ///< 3 per vertex (object space)
    std::span<const float> prevPositions; ///< optional, 3 per vertex: previous frame (skinned / deforming emitters)
    std::span<const std::uint32_t> colors;  ///< optional, RGBA8 per vertex (R in the low byte)
    std::span<const float> texcoords;     ///< optional, 2 per vertex
    std::span<const std::uint32_t> indices; ///< triangle list
};
using EmitterMeshId = std::uint32_t;
inline constexpr EmitterMeshId kInvalidMesh = 0xFFFFFFFFu;

/// Row-major 3x4 (world = M * (p, 1)).
using Mat34 = std::array<float, 12>;
Mat34 identity34();
/// From a row-vector 4x4 (D3D / USD convention, translation in m[12..14]; Relight replace::Mat4d).
Mat34 fromRowVector44(const std::array<double, 16>& m);

struct FrameInput {
    std::uint32_t frameIndex = 0;  ///< the renderer's frame id (random streams)
    float deltaTimeSecs = 1.f / 60.f; ///< raw frame time (clamped to 1/30 and scaled for the simulation)
    double absoluteTimeSecs = 0.0;
    std::array<float, 16> viewToWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; ///< column-major
    std::array<float, 16> prevWorldToProjection{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::uint32_t renderWidth = 1920, renderHeight = 1080;
    std::array<float, 3> up{0.f, 1.f, 0.f};
    float sceneScale = 1.f;
    float resolveTransparencyThreshold = 1.f / 255.f;
    float minParticleSize = 2.f; ///< pixels (upstream constant)
    /// Filled from the options (rtx.particles.* / rtx.sceneScale / rtx.zUp / rtx.resolveTransparencyThreshold).
    static FrameInput fromOptions();
};

struct SpawnRequest {
    const ParticleSystemDesc* desc = nullptr;
    std::uint64_t descHash = 0;     ///< hashDesc(*desc) (0: computed)
    std::uint64_t materialKey = 0;  ///< systems are per (description, material)
    EmitterMeshId mesh = kInvalidMesh;
    Mat34 objectToWorld = identity34();
    Mat34 prevObjectToWorld = identity34();
};

struct SpawnResult {
    std::int32_t system = -1;  ///< system slot (-1: none: disabled, no particles, table full)
    std::uint32_t particles = 0; ///< slots queued for this emitter this frame
};

/// What a backend needs to run one frame (spans into the manager's host arrays).
struct FrameWork {
    std::uint64_t serial = 0; ///< the manager's frame counter (counter slot = serial % framesInFlight)
    std::uint32_t framesInFlight = 1;
    std::span<const GpuFrameConstants> constants;   ///< per system slot
    std::span<const std::uint32_t> activeSystems;   ///< slots to dispatch, ascending
    std::span<const GpuSpawnContext> spawnContexts; ///< this frame's contexts
    std::span<const std::uint32_t> spawnMap;        ///< the whole spawn-map pool
    std::span<const float> positions;
    std::span<const std::uint32_t> colors;
    std::span<const float> texcoords;
    std::span<const std::uint32_t> indices;
    std::span<const Float4> animation;
    /// Host data changed since the previous frame (the GPU backend re-uploads these ranges).
    std::uint32_t geometryVertexBegin = 0, geometryVertexEnd = 0; ///< dirty vertices
    std::uint32_t geometryIndexBegin = 0, geometryIndexEnd = 0;   ///< dirty indices
    std::span<const std::uint32_t> animationDirty;  ///< system slots whose tables changed
    /// Particle ranges to reset to dead before this frame (systems created since the previous frame).
    std::span<const std::array<std::uint32_t, 2>> clears;
};

class ParticleBackend {
public:
    virtual ~ParticleBackend() = default;
    virtual const char* name() const = 0;
    /// Creates the pools. false: the backend cannot run.
    virtual bool init(const ManagerConfig& config) = 0;
    /// Runs the frame: clears, spawn / evolve / billboard of every active system; retirements into counter slot
    /// serial % framesInFlight.
    virtual bool execute(const FrameWork& work) = 0;
    /// The per-slot retirement counts of frame `serial` (completed), `slots` entries.
    virtual void readRetirements(std::uint64_t serial, std::uint32_t* out, std::uint32_t slots) = 0;
};

/// Draw information of one system after simulate() (upstream submitDrawState's inputs).
struct SystemDraw {
    std::uint32_t system = 0;
    std::uint64_t descHash = 0;
    std::uint64_t materialKey = 0;
    std::uint32_t vertexBase = 0;
    std::uint32_t vertexCount = 0; ///< particleCount x verticesPerParticle (unused slots are zero vertices)
    std::uint32_t verticesPerParticle = 4;
    std::uint32_t generation = 0;
};

struct SystemCounters {
    std::uint32_t head = 0, spawnOffset = 0, spawnCount = 0, particleCount = 0, tail = 0, simulateCount = 0;
    std::int64_t cachedTotal = 0;
    std::uint32_t generation = 0;
    bool operator==(const SystemCounters&) const = default;
};

class ParticleSystemManager {
public:
    explicit ParticleSystemManager(ManagerConfig config = {});
    ~ParticleSystemManager();
    ParticleSystemManager(const ParticleSystemManager&) = delete;
    ParticleSystemManager& operator=(const ParticleSystemManager&) = delete;

    const ManagerConfig& config() const { return m_config; }

    /// Copies an emitter mesh into the geometry pool; kInvalidMesh when the pool is full or the mesh is empty.
    EmitterMeshId registerMesh(const EmitterMesh& mesh);
    std::uint32_t meshTriangles(EmitterMeshId id) const;

    /// Starts a frame: the retirement counts of frame serial - framesInFlight, the options, the frame constants.
    /// `backend` must be the one simulate() runs (it holds the counters).
    void beginFrame(const FrameInput& input, ParticleBackend& backend);
    SpawnResult spawn(const SpawnRequest& request);
    /// Runs the frame on `backend` and prepares the next one. false: the backend failed.
    bool simulate(ParticleBackend& backend);

    /// After simulate(): the systems drawn this frame (ascending slot).
    std::span<const SystemDraw> draws() const { return m_draws; }
    std::uint32_t activeSystemCount() const;
    /// Slot of the system with this key (-1: none).
    std::int32_t findSystem(std::uint64_t descHash, std::uint64_t materialKey) const;
    const ParticleSystemDesc* systemDesc(std::uint32_t slot) const;
    SystemCounters counters(std::uint32_t slot) const;
    std::uint32_t particleBase(std::uint32_t slot) const;
    std::uint32_t vertexBase(std::uint32_t slot) const;
    std::uint64_t frameSerial() const { return m_serial; }
    /// The constants of the last simulate() (per slot).
    std::span<const GpuFrameConstants> constants() const { return m_constants; }

private:
    struct System;
    struct Range {
        std::uint32_t base = 0, count = 0;
    };
    bool allocRange(std::vector<Range>& freeList, std::uint32_t count, std::uint32_t& base);
    void freeRange(std::vector<Range>& freeList, std::uint32_t base, std::uint32_t count);
    std::int32_t createSystem(const ParticleSystemDesc& desc, std::uint64_t descHash, std::uint64_t materialKey);
    void destroySystem(std::uint32_t slot);
    std::uint32_t spawnCountFor(System& s);
    void prepareForNextFrame();

    ManagerConfig m_config;
    std::vector<System> m_systems;
    std::unordered_map<std::uint64_t, std::uint32_t> m_byKey; ///< (desc hash ^ material rotl) -> slot
    std::vector<Range> m_particleFree, m_vertexFree;
    std::uint32_t m_systemCounter = 0;

    // Frame state.
    FrameInput m_input;
    std::uint64_t m_serial = 0;
    bool m_frameOpen = false;
    bool m_enable = true, m_enableSpawning = true;
    float m_timeScale = 1.f;
    std::uint64_t m_nowMs = 0;

    // Host arrays (the backends read them through FrameWork).
    std::vector<GpuFrameConstants> m_constants;
    std::vector<std::uint32_t> m_active;
    std::vector<GpuSpawnContext> m_spawnContexts;
    std::uint32_t m_spawnContextCount = 0;
    std::vector<std::uint32_t> m_spawnMap;
    std::vector<float> m_positions;
    std::vector<std::uint32_t> m_colors;
    std::vector<float> m_texcoords;
    std::vector<std::uint32_t> m_indices;
    std::uint32_t m_geoVertices = 0, m_geoIndices = 0;
    struct MeshRecord {
        std::uint32_t indexOffset = 0, triangleCount = 0, vertexOffset = 0, prevVertexOffset = 0, flags = 0;
    };
    std::vector<MeshRecord> m_meshes;
    std::uint32_t m_dirtyVertexBegin = 0, m_dirtyVertexEnd = 0, m_dirtyIndexBegin = 0, m_dirtyIndexEnd = 0;
    std::vector<Float4> m_animation;
    std::vector<std::uint32_t> m_animationDirty;
    std::vector<std::array<std::uint32_t, 2>> m_clears;
    std::vector<std::uint32_t> m_retired;
    std::vector<SystemDraw> m_draws;
};

/// The CPU backend: the kernels through kernel::launch on CpuReference or CpuParallel.
class CpuParticleBackend final : public ParticleBackend {
public:
    explicit CpuParticleBackend(kernel::Backend backend = kernel::Backend::CpuParallel) : m_backend(backend) {}
    const char* name() const override;
    bool init(const ManagerConfig& config) override;
    bool execute(const FrameWork& work) override;
    void readRetirements(std::uint64_t serial, std::uint32_t* out, std::uint32_t slots) override;

    std::span<const GpuParticle> particles() const { return m_particles; }
    std::span<const GpuParticleVertex> vertices() const { return m_vertices; }
    kernel::Backend backend() const { return m_backend; }
    /// Backends that actually ran the last frame's launches (CpuParallel may be CpuReference-equivalent).
    kernel::Backend lastExecuted() const { return m_lastExecuted; }

private:
    kernel::Backend m_backend;
    kernel::Backend m_lastExecuted = kernel::Backend::CpuReference;
    ManagerConfig m_config;
    std::vector<GpuParticle> m_particles;
    std::vector<GpuParticleVertex> m_vertices;
    std::vector<std::uint32_t> m_counters; ///< framesInFlight x maxSystems
};

} // namespace fuse::relight::particles
