#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/softbody/particle_system.hpp>

#include <vector>

namespace fuse::physics {

struct BufferHandleTag {};
using BufferHandle = u32;

struct ClothDesc {
    u32 rows = 0;
    u32 cols = 0;
    f32 particleSpacing = 0.1f;
    /// XPBD compliance of structural + shear edges (0 = inextensible).
    f32 stretchCompliance = 0.f;
    /// XPBD compliance of the skip-one bend edges.
    f32 bendCompliance = 0.001f;
    /// Bit 0: (row 0, col 0), bit 1: (row 0, last col), bit 2: (last row, col 0), bit 3: (last row, last col).
    u32 pinnedCorners = 0b0011u;
    /// Small substeps with one constraint pass each (XPBD "small steps").
    u32 substeps = 4;
    u32 iterations = 1;
    f32 particleMass = 0.01f;
    /// Aerodynamic drag coefficient per unit area (0.5 * rho * Cd folded together).
    f32 windDrag = 0.6f;
    /// Per-second velocity retention (1 = undamped).
    f32 damping = 0.99f;
    /// Collision skin kept between the cloth and sphere colliders.
    f32 thickness = 0.01f;
    /// Fraction of tangential slip removed per substep while touching a collider.
    f32 friction = 0.5f;
    /// Long-range attachments: each free particle may be no further from every pin than
    /// its rest distance, which removes the stretch of long hanging cloth.
    bool longRangeAttachments = true;
};

struct ClothSphereCollider {
    vec3 center{};
    f32 radius = 0.f;
};

/// Axis-aligned box collider. `halfExtents` are the solid half-sizes; cloth thickness is added outside.
struct ClothBoxCollider {
    vec3 center{};
    vec3 halfExtents{};
};

/// B4.8 — XPBD cloth (CPU path; CUDA kernels deferred).
class ClothSimulator {
public:
    ClothSimulator() = default;
    /// Owns the ParticleSoA arrays: release them on scope exit (LSan leak otherwise).
    ~ClothSimulator() { destroy(); }
    ClothSimulator(const ClothSimulator&) = delete;
    ClothSimulator& operator=(const ClothSimulator&) = delete;

    void init(const ClothDesc& desc, vec3 origin);
    void destroy();
    void step(f32 dt, vec3 gravity);

    /// Sets the ambient wind velocity; drag acts on each triangle along its normal.
    void applyWind(vec3 windVelocity);
    vec3 wind() const { return m_wind; }

    void addSphereCollider(vec3 center, f32 radius);
    void addBoxCollider(vec3 center, vec3 halfExtents);
    void clearColliders() {
        m_spheres.clear();
        m_boxes.clear();
    }

    BufferHandle vertexBuffer() const { return m_vertexBuffer; }
    BufferHandle indexBuffer() const { return m_indexBuffer; }
    u32 indexCount() const { return m_indexCount; }
    u32 particleCount() const { return m_particles.count; }
    bool ready() const { return m_ready; }

    const ClothDesc& desc() const { return m_desc; }
    u32 index(u32 row, u32 col) const { return row * m_desc.cols + col; }
    vec3 position(u32 particle) const { return m_particles.positions[particle]; }
    vec3 velocity(u32 particle) const { return m_particles.velocities[particle]; }
    bool pinned(u32 particle) const { return m_particles.invMasses[particle] == 0.f; }
    /// Structural + shear + bend constraints.
    u32 constraintCount() const { return static_cast<u32>(m_constraints.size()); }
    /// Largest structural edge length / rest length.
    f32 maxStretchRatio() const;

private:
    void buildConstraints_();
    void applyAerodynamics_(f32 dt);
    void solveConstraints_(f32 dt);
    void collideSpheres_();
    void collideBoxes_();
    void solveTethers_();
    void prepareSolve_(f32 dt);
    void solveRange_(u32 begin, u32 end);

    /// Constraint with its XPBD weights folded in for the current substep size.
    struct PackedConstraint {
        u32 a = 0;
        u32 b = 0;
        f32 restLength = 0.f;
        f32 kA = 0.f; // wA / (wA + wB + compliance / h^2)
        f32 kB = 0.f;
    };

    bool m_ready = false;
    ClothDesc m_desc{};
    ParticleSoA m_particles{};
    std::vector<ParticleConstraint> m_constraints{};
    u32 m_structuralCount = 0;
    std::vector<ParticleConstraint> m_tethers{};
    /// Packed constraints ordered band by band (row bands whose constraints touch disjoint
    /// particles, solved in parallel), then the band-crossing constraints (solved serially).
    std::vector<PackedConstraint> m_packed{};
    std::vector<u32> m_bandStarts{}; // bandCount + 1 offsets into m_packed; the rest are crossings
    f32 m_packedDt = 0.f;
    std::vector<ClothSphereCollider> m_spheres{};
    std::vector<ClothBoxCollider> m_boxes{};
    vec3 m_wind{};
    BufferHandle m_vertexBuffer = 0;
    BufferHandle m_indexBuffer = 0;
    u32 m_indexCount = 0;
};

} // namespace fuse::physics
