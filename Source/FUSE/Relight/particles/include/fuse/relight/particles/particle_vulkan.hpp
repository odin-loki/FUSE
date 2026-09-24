// FUSE Relight RL-3.6: the Vulkan compute backend of the particle system (built with the renderer's Vulkan backend
// only). The three passes are the GLSL twin of particle_kernels.hpp (shaders/particle_sim.comp compiled three times,
// PARTICLE_PASS = 0 spawn, 1 evolve, 2 billboard), embedded as SPIR-V at build time.
//
// Per frame (execute): the host-written arrays of the frame slot (serial % framesInFlight: constants, spawn contexts,
// spawn map) are copied into persistently mapped buffers; one command buffer clears the slot's retirement counters and
// the particle ranges of new systems, then dispatches spawn for every system with spawns, a barrier, evolve, a
// barrier, billboards, and a final barrier to host reads / vertex input / transfer. The submission signals the slot's
// fence; readRetirements(serial) waits for it and reads the slot's counters (the manager asks for frame
// serial - framesInFlight, i.e. just before the slot is reused). The shared pools written by the host (emitter
// geometry, animation tables) are re-uploaded only when they change, after waiting for every frame in flight.
//
// Particles and vertices live in device-local buffers; readback() copies them to the host (tests, capture).
#pragma once

#include <fuse/relight/particles/particle_system.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {
class VulkanDevice;
class GpuAllocator;
} // namespace fuse::renderer

namespace fuse::relight::particles {

/// True when the build embedded the GLSL kernels (glslangValidator found).
bool vulkanKernelsAvailable();

class VulkanParticleBackend final : public ParticleBackend {
public:
    VulkanParticleBackend(renderer::VulkanDevice& device, renderer::GpuAllocator& allocator);
    ~VulkanParticleBackend() override;
    VulkanParticleBackend(const VulkanParticleBackend&) = delete;
    VulkanParticleBackend& operator=(const VulkanParticleBackend&) = delete;

    const char* name() const override { return "vulkan"; }
    bool init(const ManagerConfig& config) override;
    bool execute(const FrameWork& work) override;
    void readRetirements(std::uint64_t serial, std::uint32_t* out, std::uint32_t slots) override;

    /// Waits for every frame in flight.
    bool waitIdle();
    /// Copies the particle and vertex pools to the host (waits for the GPU).
    bool readback(std::vector<GpuParticle>& particles, std::vector<GpuParticleVertex>& vertices);
    /// Overwrites the particle pool with `particles` (waits for the GPU): tests re-synchronize the GPU with the CPU
    /// reference to measure one-step parity of chaotic (turbulent) systems.
    bool uploadParticles(std::span<const GpuParticle> particles);
    /// The device-local vertex pool (VkBuffer as void*), for a renderer drawing the billboards.
    void* vertexBuffer() const;
    const std::string& lastError() const { return m_error; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_error;
};

} // namespace fuse::relight::particles
