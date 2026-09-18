#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::SelfPair:
        return "SelfPair";
    case IslandBuildRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case IslandBuildRejectReason::InvalidContact:
        return "InvalidContact";
    }
    return "Unknown";
}

IslandBuildPreflight preflight_island_build(u32 bodyCount,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;

    for (const narrowphase::ContactManifold& contact : contacts) {
        const IslandBuildRejectReason reason = contactBuildRejectReason(contact, bodyCount);
        switch (reason) {
        case IslandBuildRejectReason::None:
            ++preflight.validContactCount;
            break;
        case IslandBuildRejectReason::InvalidContact:
            ++preflight.invalidContactCount;
            break;
        case IslandBuildRejectReason::SelfPair:
            ++preflight.selfPairContactCount;
            break;
        case IslandBuildRejectReason::OutOfRangeBody:
            ++preflight.oobContactCount;
            break;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        const IslandBuildRejectReason reason = distanceBuildRejectReason(constraint, bodyCount);
        switch (reason) {
        case IslandBuildRejectReason::None:
            ++preflight.validDistanceCount;
            break;
        case IslandBuildRejectReason::SelfPair:
            ++preflight.selfPairDistanceCount;
            break;
        case IslandBuildRejectReason::OutOfRangeBody:
            ++preflight.oobDistanceCount;
            break;
        case IslandBuildRejectReason::InvalidContact:
            break;
        }
    }

    preflight.skipped = bodyCount == 0u && contacts.empty() && distanceConstraints.empty();
    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).skipped;
}

IslandBuildValidation validate_island_indices(const ContactIslandGraph& graph,
                                              u32 contactCount,
                                              u32 distanceCount) {
    IslandBuildValidation validation{};
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        for (u32 contactIndex : island.contactIndices) {
            if (contactIndex >= contactCount) {
                ++validation.oobContactIndexCount;
                validation.valid = false;
            }
        }
        for (u32 distanceIndex : island.distanceIndices) {
            if (distanceIndex >= distanceCount) {
                ++validation.oobDistanceIndexCount;
                validation.valid = false;
            }
        }
    }
    return validation;
}

void ContactIslandGraph::clear() {
    parent_.clear();
    islands_.clear();
}

u32 ContactIslandGraph::findRoot(u32 index) const {
    u32 root = index;
    while (parent_[root] != root) {
        root = parent_[root];
    }
    return root;
}

void ContactIslandGraph::compressPath(u32 index) {
    const u32 root = findRoot(index);
    while (parent_[index] != root) {
        const u32 next = parent_[index];
        parent_[index] = root;
        index = next;
    }
}

void ContactIslandGraph::unionBodies(u32 a, u32 b) {
    if (a >= parent_.size() || b >= parent_.size()) {
        return;
    }

    u32 rootA = findRoot(a);
    u32 rootB = findRoot(b);
    if (rootA == rootB) {
        return;
    }
    if (rootA < rootB) {
        parent_[rootB] = rootA;
    } else {
        parent_[rootA] = rootB;
    }
}

void ContactIslandGraph::build(u32 bodyCount,
                               const std::vector<narrowphase::ContactManifold>& contacts,
                               const std::vector<DistanceConstraint>& distanceConstraints) {
    clear();
    parent_.resize(bodyCount);
    for (u32 i = 0; i < bodyCount; ++i) {
        parent_[i] = i;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        unionBodies(constraint.bodyA, constraint.bodyB);
    }

    for (u32 i = 0; i < bodyCount; ++i) {
        compressPath(i);
    }

    std::vector<u32> rootToIsland(bodyCount, invalidIsland);
    islands_.clear();

    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        const u32 root = findRoot(bodyIndex);
        if (rootToIsland[root] == invalidIsland) {
            rootToIsland[root] = static_cast<u32>(islands_.size());
            islands_.push_back({});
        }
        islands_[rootToIsland[root]].bodyIndices.push_back(bodyIndex);
    }

    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }
        const u32 islandIndex = rootToIsland[findRoot(contact.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].contactIndices.push_back(contactIndex);
        }
    }

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        const u32 islandIndex = rootToIsland[findRoot(constraint.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].distanceIndices.push_back(distanceIndex);
        }
    }

    std::sort(islands_.begin(), islands_.end(), [](const Island& left, const Island& right) {
        if (left.bodyIndices.empty() || right.bodyIndices.empty()) {
            return left.bodyIndices.size() < right.bodyIndices.size();
        }
        return left.bodyIndices.front() < right.bodyIndices.front();
    });
}

u32 ContactIslandGraph::constrainedIslandCount() const {
    u32 count = 0;
    for (const Island& island : islands_) {
        if (!island.isEmpty()) {
            ++count;
        }
    }
    return count;
}

u32 ContactIslandGraph::bodyIsland(u32 bodyIndex) const {
    if (bodyIndex >= parent_.size()) {
        return invalidIsland;
    }

    const u32 root = findRoot(bodyIndex);
    for (u32 islandIndex = 0; islandIndex < islands_.size(); ++islandIndex) {
        for (u32 index : islands_[islandIndex].bodyIndices) {
            if (index == bodyIndex || findRoot(index) == root) {
                return islandIndex;
            }
        }
    }
    return invalidIsland;
}

} // namespace fuse::physics
