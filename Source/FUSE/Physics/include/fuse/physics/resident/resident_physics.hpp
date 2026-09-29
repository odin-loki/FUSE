#pragma once

// Resident physics pipeline: broadphase -> narrowphase -> coloured solver iterations on buffers that
// stay in one memory space between stages and between calls (docs/compute-kernels.md).
//
//   Space::Cuda  device memory allocated once (grow-only) and one CUDA stream. Every stage only
//                enqueues kernels (resident_kernels.hpp through kernels/physics_resident.cu); nothing
//                waits for the device until the caller reads something back. Copies happen only at
//                the edges: upload*() before, read*/download*() after.
//   Space::Host  the same launch sequence over host memory on CpuReference / CpuParallel. This is
//                the reference the CPU gates run: fuse_b4_physics_kernel_gates checks it against the
//                legacy paths (runBroadphaseIntoBuffer, the CPU narrowphase, the coloured solve).
//
// Data-dependent sizes stay on the device (see resident_kernels.hpp). A stage whose capacity was too
// small clips and reports it in Status::overflow; fitBroadphase / fitNarrowphase (or a caller that
// reads the status later) grow the buffers and run again. Steady-state calls allocate nothing.
//
// Limits: at most 65536 bodies (packed 32-bit pair keys), no pair buffer max-capacity clamp, and the
// narrowphase dispatches sphere / box / capsule / plane shapes (uploadScene refuses convex hulls,
// which stay on the CPU path). Joints are not part of the resident solve.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/resident/resident_kernels.hpp>
#include <fuse/physics/solver/constraint_coloring.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::physics::resident {

enum class Space : u8 { Host = 0, Cuda = 1 };

/// One substep's constraint problem in the PBDSolver layout (host arrays), for uploadSolver.
struct SolverProblem {
    const RigidBodySoA* bodies = nullptr; ///< predictedPositions / predictedOrientations / flags / invMasses / frictionStatic
    const std::vector<vec3>* invInertia = nullptr; ///< SolverWorkBuffers::bodyInvInertia
    const std::vector<narrowphase::ContactManifold>* contacts = nullptr;
    const std::vector<ContactAnchor>* anchors = nullptr; ///< kMaxContactPointsPerManifold per contact (may be empty)
    const std::vector<f32>* contactLambdas = nullptr;
    const std::vector<f32>* pointLambdas = nullptr;
    const std::vector<DistanceConstraint>* distances = nullptr;
    const std::vector<f32>* distanceLambdas = nullptr;
    const ConstraintColoring* coloring = nullptr;
};

/// Sizes of the buffers the pipeline owns (grow-only).
struct Capacities {
    u32 bodies = 0;
    u32 shapes = 0;
    u32 entries = 0;  ///< (cell, body) entries = run capacity
    u32 pairs = 0;    ///< cell pairs + plane slots = unique pair capacity = narrowphase grid
    u32 contacts = 0; ///< narrowphase output
};

class ResidentPhysics {
public:
    ResidentPhysics();
    ~ResidentPhysics();
    ResidentPhysics(const ResidentPhysics&) = delete;
    ResidentPhysics& operator=(const ResidentPhysics&) = delete;

    /// True when this build has the CUDA backend and a device is usable.
    static bool cudaAvailable();

    /// Space::Host runs the kernels on `hostBackend` (CpuReference / CpuParallel). Space::Cuda needs
    /// cudaAvailable(). Returns false (and stays uninitialised) otherwise.
    bool init(Space space, kernel::Backend hostBackend = kernel::Backend::CpuParallel);
    void release();
    bool initialized() const { return initialized_; }
    Space space() const { return space_; }
    const Capacities& capacities() const { return caps_; }

    // ---- edges: uploads (Cuda: queued on the stream) ------------------------------------------

    /// Shapes (types / params / scalars / bodies), the narrowphase body -> shape map, the plane list
    /// and the body state below. False for more than 65536 bodies or a convex-hull shape.
    bool uploadScene(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes);
    /// Positions, orientations, flags, inverse masses, friction and collision layers (per step).
    bool uploadBodyState(const RigidBodySoA& bodies);
    /// Candidate pairs for narrowphase() instead of the broadphase output (order kept).
    bool uploadPairs(const std::vector<broadphase::CandidatePair>& pairs);

    // ---- stages: enqueue only ---------------------------------------------------------------

    /// runBroadphaseIntoBuffer (use2D: runBroadphase2DIntoBuffer) with maxCapacity 0: the sorted,
    /// unique pair list (plane pairs merged) stays on the device as narrowphase input.
    bool broadphase(const broadphase::SpatialHashParams& params, bool use2D);
    /// Manifolds of the current pairs (the last broadphase or uploadPairs), in pair order.
    /// speculative == false: runNarrowphaseIntoBuffer / narrowphasePairContact per pair (margin
    /// ignored). speculative == true: collidePairs(pairs, ..., margin) (the solver's contacts).
    bool narrowphase(f32 margin = 0.f, bool speculative = false);

    /// broadphase() + readStatus, growing the buffers and running again until nothing overflowed.
    bool fitBroadphase(const broadphase::SpatialHashParams& params, bool use2D, Status* status = nullptr);
    /// narrowphase() + readStatus, growing the contact buffer until nothing overflowed.
    bool fitNarrowphase(f32 margin = 0.f, bool speculative = false, Status* status = nullptr);

    // ---- solver -------------------------------------------------------------------------------

    /// Body state, contacts, anchors, lambdas, distance constraints and the colouring of one substep.
    bool uploadSolver(const SolverProblem& problem);
    /// `iterations` x (one launch per colour + the serial overflow list), back to back on the stream:
    /// the loop PBDSolver runs for ConstraintSolveMode::ColoredKernel (without joints).
    bool solveIterations(u32 iterations, f32 dt, f32 contactCompliance);
    /// Predicted positions / orientations of every body and the lambdas (null vectors are skipped).
    bool downloadSolver(RigidBodySoA& bodies, std::vector<f32>* contactLambdas, std::vector<f32>* pointLambdas,
                        std::vector<f32>* distanceLambdas);

    // ---- edges: readback (synchronises) --------------------------------------------------------

    bool synchronize();
    bool readStatus(Status& out);
    /// The unique pair list of the last broadphase as runBroadphaseIntoBuffer leaves the buffer.
    bool downloadPairs(broadphase::PairBufferSoA& out);
    bool downloadContacts(std::vector<narrowphase::ContactManifold>& out);

    /// Grows the buffers an overflowed Status asks for; true when something grew (run again).
    bool growFromStatus(const Status& status);

    // ---- timing: CUDA events on the stream (Space::Host: the host clock) -----------------------

    bool timerBegin();
    /// Milliseconds since timerBegin (Cuda: waits for the stop event). Negative on failure.
    double timerEndMs();

    struct Impl;

private:
    bool initialized_ = false;
    Space space_ = Space::Host;
    kernel::Backend hostBackend_ = kernel::Backend::CpuParallel;
    Capacities caps_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace fuse::physics::resident
