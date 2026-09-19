#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

const char* contactIslandGraphBuildRejectReasonName(ContactIslandGraphBuildRejectReason reason) {
    switch (reason) {
    case ContactIslandGraphBuildRejectReason::None:
        return "None";
    case ContactIslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case ContactIslandGraphBuildRejectReason::UnsafeRefs:
        return "UnsafeRefs";
    case ContactIslandGraphBuildRejectReason::SelfContact:
        return "SelfContact";
    }
    return "Unknown";
}

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandGraphBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        if (!contact.valid) {
            continue;
        }
        if (contact.bodyA == contact.bodyB) {
            ++preflight.stats.selfContactCount;
            continue;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
        } else {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA == constraint.bodyB) {
            ++preflight.stats.selfContactCount;
            continue;
        }
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    if (bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
        preflight.stats.inRangeDistanceCount == 0u) {
        preflight.skipped = true;
        preflight.reason = ContactIslandGraphBuildRejectReason::EmptyInput;
    } else if (preflight.has_unsafe_refs()) {
        preflight.reason = ContactIslandGraphBuildRejectReason::UnsafeRefs;
    } else if (preflight.has_self_contacts()) {
        preflight.reason = ContactIslandGraphBuildRejectReason::SelfContact;
    }

    return preflight;
}

bool shouldSkipContactIslandGraphBuild(u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();
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

bool ContactIslandGraph::buildGuarded(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    if (shouldSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
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
