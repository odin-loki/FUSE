#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

namespace {

bool contactBodiesInRange(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

ContactIslandGraphBuildRejectReason diagnoseBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    bool outOfRangeContact = false;
    bool outOfRangeDistance = false;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        if (contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++inRangeContactCount;
        } else {
            outOfRangeContact = true;

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++inRangeDistanceCount;
            outOfRangeDistance = true;

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return ContactIslandGraphBuildRejectReason::EmptyInput;
    if (outOfRangeContact) {
        return ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies;
    if (outOfRangeDistance) {
        return ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies;
    return ContactIslandGraphBuildRejectReason::None;

} // namespace

const char* contactIslandGraphBuildRejectReasonName(ContactIslandGraphBuildRejectReason reason) {
    switch (reason) {
    case ContactIslandGraphBuildRejectReason::None:
        return "None";
    case ContactIslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";

ContactIslandGraphBuildRejectReason contactIslandGraphBuildRejectReason(
    return diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);

bool contactIslandGraphBuildRejectsForReason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    ContactIslandGraphBuildRejectReason expected) {
    return contactIslandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(
    ContactIslandGraphBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

        if (contact.valid) {
            ++preflight.validContactCount;
        if (contact.valid && contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++preflight.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.outOfRangeContactBodyCount;
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    case IslandBuildRejectReason::None:
    case IslandBuildRejectReason::SelfPair:
        return "SelfPair";
    case IslandBuildRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case IslandBuildRejectReason::InvalidContact:
        return "InvalidContact";

IslandBuildPreflight preflight_island_build(u32 bodyCount,
    IslandBuildPreflight preflight{};

        const IslandBuildRejectReason reason = contactBuildRejectReason(contact, bodyCount);
            break;
            ++preflight.invalidContactCount;
            ++preflight.selfPairContactCount;
            ++preflight.oobContactCount;

            ++preflight.inRangeDistanceCount;
            ++preflight.outOfRangeDistanceBodyCount;

    preflight.reason = diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == ContactIslandGraphBuildRejectReason::EmptyInput;
    return preflight;

bool canSkipContactIslandGraphBuild(
    return !preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();

bool shouldRunContactIslandGraphBuild(
    return preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();
        const IslandBuildRejectReason reason = distanceBuildRejectReason(constraint, bodyCount);
            ++preflight.validDistanceCount;
            ++preflight.selfPairDistanceCount;
            ++preflight.oobDistanceCount;

    preflight.skipped = bodyCount == 0u && contacts.empty() && distanceConstraints.empty();

bool should_skip_island_build(u32 bodyCount,
    return preflight_island_build(bodyCount, contacts, distanceConstraints).skipped;

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
        for (u32 distanceIndex : island.distanceIndices) {
            if (distanceIndex >= distanceCount) {
                ++validation.oobDistanceIndexCount;
    return validation;
IslandBuildStats compute_island_build_stats(
    IslandBuildStats stats{};
    stats.bodyCount = bodyCount;
    stats.contactCount = static_cast<u32>(contacts.size());
    stats.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());

            ++stats.invalidContacts;
        if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
            ++stats.outOfRangeContacts;
        ++stats.validUnionEdges;

        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
            ++stats.outOfRangeDistanceConstraints;

    return stats;

IslandBuildPreflight preflight_island_graph_build(
    preflight.stats = compute_island_build_stats(bodyCount, contacts, distanceConstraints);
    preflight.skipped = bodyCount == 0u;

bool should_skip_island_graph_build(u32 bodyCount) {
    return bodyCount == 0u;
bool contact_body_indices_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;

bool distance_body_indices_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;


bool is_valid_island_build_body_count(u32 bodyCount,
        if (!contact_body_indices_in_range(bodyCount, contact)) {
            return false;

        if (!distance_body_indices_in_range(bodyCount, constraint)) {
    return true;


            ++preflight.outOfRangeContactCount;

            ++preflight.outOfRangeDistanceCount;

    preflight.invalidBodyCount =
        preflight.outOfRangeContactCount > 0u || preflight.outOfRangeDistanceCount > 0u;

    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
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
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            continue;
        }
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            continue;
        }
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
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            continue;
        }
        const u32 islandIndex = rootToIsland[findRoot(contact.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].contactIndices.push_back(contactIndex);
        }
    }

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            continue;
        }
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

bool ContactIslandGraph::buildGuarded(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    if (canSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
void ContactIslandGraph::build_guarded(u32 bodyCount,
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
        return;
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

bool ContactIslandGraph::build_guarded(u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    if (!preflight.can_build()) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
}

bool is_valid_island_build_body_count(u32 bodyCount) {
    return bodyCount > 0u;
}

bool contact_references_valid_bodies(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    if (bodyCount == 0u) {
        return false;
    }
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_constraint_references_valid_bodies(const DistanceConstraint& constraint, u32 bodyCount) {
    if (bodyCount == 0u) {
        return false;
    }
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactCount = static_cast<u32>(contacts.size());
    preflight.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());
    preflight.zeroBodies = !is_valid_island_build_body_count(bodyCount);
    preflight.skipped = preflight.zeroBodies;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid || !contact_references_valid_bodies(contact, bodyCount)) {
            ++preflight.invalidContactCount;
        } else {
            ++preflight.validContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_constraint_references_valid_bodies(constraint, bodyCount)) {
            ++preflight.invalidDistanceCount;
        } else {
            ++preflight.validDistanceCount;
        }
    }

    return preflight;
}

bool should_skip_island_build(u32 bodyCount) {
    return !is_valid_island_build_body_count(bodyCount);
}

bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints) {
    return graph.build_guarded(bodyCount, contacts, distanceConstraints);
}

} // namespace fuse::physics
