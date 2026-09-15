#pragma once

#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {

enum class BroadphaseMode : u8 {
    SpatialHash3D,
    SpatialHash2D
};

struct PhysicsPipelineDesc {
    u32 maxBodies = 4096;
    u32 maxPairs = 16384;
    f32 cellSize = 2.f;
    BroadphaseMode broadphaseMode = BroadphaseMode::SpatialHash3D;
};

/// B4.1 frame pipeline — CPU stub of the CUDA-first physics stream.
class PhysicsPipeline {
public:
    void init(const PhysicsPipelineDesc& desc);
    void reset();

    u32 addSphereBody(vec3 position, f32 radius, f32 invMass = 1.f, u32 flags = 0);
    u32 addStaticPlane(vec3 normal, f32 distance);

    void step(f32 dt);

    RigidBodySoA& bodies() { return m_bodies; }
    const RigidBodySoA& bodies() const { return m_bodies; }
    const CollisionShapeSoA& shapes() const { return m_shapes; }
    const std::vector<broadphase::CandidatePair>& candidatePairs() const { return m_candidatePairs; }
    const broadphase::PairBufferSoA& pairBuffer() const { return m_pairBuffer; }
    const std::vector<narrowphase::ContactManifold>& contacts() const { return m_contacts; }
    const narrowphase::ContactBufferSoA& contactBuffer() const { return m_contactBuffer; }

    u32 bodyCount() const { return m_bodies.count(); }
    u32 contactCount() const;
    f32 lastStepDt() const { return m_lastDt; }

private:
    void integrateStub(f32 dt);

    PhysicsPipelineDesc m_desc{};
    RigidBodySoA m_bodies;
    CollisionShapeSoA m_shapes;
    broadphase::SpatialHashParams m_hashParams{};
    broadphase::PairBufferSoA m_pairBuffer;
    std::vector<broadphase::CandidatePair> m_candidatePairs;
    narrowphase::ContactBufferSoA m_contactBuffer;
    std::vector<narrowphase::ContactManifold> m_contacts;
    f32 m_lastDt = 0.f;
    bool m_initialized = false;
};

} // namespace fuse::physics
