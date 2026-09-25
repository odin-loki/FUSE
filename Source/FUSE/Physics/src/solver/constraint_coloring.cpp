#include <fuse/physics/solver/constraint_coloring.hpp>

#include <algorithm>
#include <bit>

namespace fuse::physics {

void ConstraintColoring::build(const RigidBodySoA& bodies, const std::vector<narrowphase::ContactManifold>& contacts,
                               const std::vector<DistanceConstraint>& distanceConstraints) {
    const u32 bodyCount = bodies.count();
    bodyMasks.assign(bodyCount, 0u);
    buildOrder.clear();
    itemColors.clear();
    const usize maxItems = contacts.size() + distanceConstraints.size();
    if (buildOrder.capacity() < maxItems) {
        buildOrder.reserve(maxItems + maxItems / 2u);
        itemColors.reserve(maxItems + maxItems / 2u);
    }
    u32 counts[kMaxColors + 1u]{};

    const auto assign = [&](u32 ref, u32 a, u32 b) {
        const bool dynA = solverBodyIsDynamic(bodies, a);
        const bool dynB = solverBodyIsDynamic(bodies, b);
        const u64 used = (dynA ? bodyMasks[a] : 0u) | (dynB ? bodyMasks[b] : 0u);
        u32 color = kMaxColors; // overflow
        if (used != ~0ull) {
            color = static_cast<u32>(std::countr_zero(~used));
            const u64 bit = 1ull << color;
            if (dynA) {
                bodyMasks[a] |= bit;
            }
            if (dynB) {
                bodyMasks[b] |= bit;
            }
        }
        buildOrder.push_back(ref);
        itemColors.push_back(color);
        ++counts[color];
    };

    for (u32 i = 0; i < contacts.size(); ++i) {
        const narrowphase::ContactManifold& c = contacts[i];
        if (c.valid && c.bodyA < bodyCount && c.bodyB < bodyCount) {
            assign(i, c.bodyA, c.bodyB);
        }
    }
    for (u32 i = 0; i < distanceConstraints.size(); ++i) {
        const DistanceConstraint& d = distanceConstraints[i];
        if (d.bodyA < bodyCount && d.bodyB < bodyCount) {
            assign(i | kDistanceBit, d.bodyA, d.bodyB);
        }
    }

    // Stable bucket by colour: within a colour, build (index) order.
    colorCount = 0u;
    colorStart[0] = 0u;
    for (u32 c = 0; c <= kMaxColors; ++c) {
        colorStart[c + 1u] = colorStart[c] + counts[c];
        if (c < kMaxColors && counts[c] > 0u) {
            colorCount = c + 1u;
        }
    }
    overflowCount = counts[kMaxColors];
    if (items.capacity() < buildOrder.size()) {
        items.reserve(buildOrder.capacity());
    }
    items.resize(buildOrder.size());
    u32 cursor[kMaxColors + 1u];
    std::copy(colorStart, colorStart + kMaxColors + 1u, cursor);
    for (usize i = 0; i < buildOrder.size(); ++i) {
        items[cursor[itemColors[i]]++] = buildOrder[i];
    }
}

} // namespace fuse::physics
