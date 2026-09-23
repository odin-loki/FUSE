#pragma once

// Graph colouring of the PBD solver's constraints for the colored kernel solve
// (SolverParams::solveMode = ConstraintSolveMode::ColoredKernel).
//
// Constraints (valid in-range contacts, then in-range distance constraints, in index order) are
// greedily given the lowest colour not used by either of their *dynamic* bodies (awake, not
// static / kinematic, invMass > 0 — the only bodies a constraint solve writes). Within a colour no
// two constraints write the same body or read a body another one writes, so one launch of
// "physics_solve_color" per colour and iteration runs them in parallel with no atomics, and the
// result is bit-identical on every CPU backend and worker count. Constraints that find no free
// colour among kMaxColors go to an overflow list solved serially after the colours.
//
// The colored order differs from the default island Gauss-Seidel order (contacts of an island in
// index order), so the two modes are separately deterministic but not bit-identical to each other;
// the island path stays the default.

#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

enum class ConstraintSolveMode : u8 {
    /// Default: islands in order, each island's contacts then distance constraints (Gauss-Seidel).
    IslandGaussSeidel = 0,
    /// Graph-coloured Gauss-Seidel: one "physics_solve_color" kernel launch per colour.
    ColoredKernel = 1,
};

inline constexpr const char* kSolveColorKernelName = "physics_solve_color";

struct ConstraintColoring {
    static constexpr u32 kMaxColors = 64u;
    /// Set on distance-constraint entries of `items` (clear: contact index).
    static constexpr u32 kDistanceBit = 0x80000000u;

    /// items[colorStart[c] .. colorStart[c + 1]) is colour c; colour kMaxColors is the serial overflow.
    std::vector<u32> items;
    u32 colorStart[kMaxColors + 2u]{};
    u32 colorCount = 0;    ///< colours in use (overflow excluded)
    u32 overflowCount = 0; ///< constraints in the serial overflow list

    std::vector<u64> bodyMasks;  ///< scratch: colours used per body
    std::vector<u32> itemColors; ///< scratch: colour per constraint in build order
    std::vector<u32> buildOrder; ///< scratch: constraint refs in build order

    void build(const RigidBodySoA& bodies, const std::vector<narrowphase::ContactManifold>& contacts,
               const std::vector<DistanceConstraint>& distanceConstraints);

    u32 colorSize(u32 color) const { return colorStart[color + 1u] - colorStart[color]; }
    const u32* colorItems(u32 color) const { return items.data() + colorStart[color]; }
    u32 constraintCount() const { return static_cast<u32>(items.size()); }
};

/// Body written by a constraint solve: awake, not static / kinematic, positive inverse mass.
inline bool solverBodyIsDynamic(const RigidBodySoA& bodies, u32 index) {
    const u32 flags = bodies.flags[index];
    return (flags & (RB_STATIC | RB_KINEMATIC | RB_SLEEPING)) == 0u && bodies.invMasses[index] > 0.f;
}

} // namespace fuse::physics
