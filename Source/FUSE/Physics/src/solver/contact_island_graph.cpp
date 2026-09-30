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
        }
        if (contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++inRangeContactCount;
        } else {
            outOfRangeContact = true;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++inRangeDistanceCount;
        } else {
            outOfRangeDistance = true;
        }
    }

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return ContactIslandGraphBuildRejectReason::EmptyInput;
    }
    if (outOfRangeContact) {
        return ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies;
    }
    if (outOfRangeDistance) {
        return ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies;
    }
    return ContactIslandGraphBuildRejectReason::None;
}

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
    }
}

ContactIslandGraphBuildRejectReason contactIslandGraphBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
}

bool contactIslandGraphBuildRejectsForReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    ContactIslandGraphBuildRejectReason expected) {
    return contactIslandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
}

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandGraphBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.validContactCount;
        }
        if (contact.valid && contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++preflight.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceBodyCount;
        }
    }

    preflight.reason = diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == ContactIslandGraphBuildRejectReason::EmptyInput;
    return preflight;
}

bool canSkipContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();
}

bool shouldRunContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();
}

void ContactIslandGraph::clear() {
    parent_.clear();
    islands_.clear();
    ranges_.clear();
    flatBodies_.clear();
    flatContacts_.clear();
    flatDistances_.clear();
    islandCount_ = 0;
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

ContactIslandGraph::ContactIslandGraph(const ContactIslandGraph& other)
    : parent_(other.parent_),
      islands_(other.islands_),
      ranges_(other.ranges_),
      islandCount_(other.islandCount_),
      rootToIsland_(other.rootToIsland_),
      flatBodies_(other.flatBodies_),
      flatContacts_(other.flatContacts_),
      flatDistances_(other.flatDistances_),
      cursor_(other.cursor_) {
    rebindSpans_();
}

ContactIslandGraph& ContactIslandGraph::operator=(const ContactIslandGraph& other) {
    if (this != &other) {
        parent_ = other.parent_;
        islands_ = other.islands_;
        ranges_ = other.ranges_;
        islandCount_ = other.islandCount_;
        rootToIsland_ = other.rootToIsland_;
        flatBodies_ = other.flatBodies_;
        flatContacts_ = other.flatContacts_;
        flatDistances_ = other.flatDistances_;
        cursor_ = other.cursor_;
        rebindSpans_();
    }
    return *this;
}

void ContactIslandGraph::reserve(u32 maxBodies, u32 maxContacts, u32 maxConstraints) {
    parent_.reserve(maxBodies);
    rootToIsland_.reserve(maxBodies);
    islands_.reserve(maxBodies);
    ranges_.reserve(maxBodies);
    cursor_.reserve(maxBodies);
    flatBodies_.reserve(maxBodies);
    flatContacts_.reserve(maxContacts);
    flatDistances_.reserve(maxConstraints);
}

void ContactIslandGraph::rebindSpans_() {
    for (u32 i = 0; i < islandCount_ && i < islands_.size() && i < ranges_.size(); ++i) {
        const Range& r = ranges_[i];
        Island& island = islands_[i];
        island.bodyIndices = {flatBodies_.data() + r.bodyBegin, r.bodyCount};
        island.contactIndices = {flatContacts_.data() + r.contactBegin, r.contactCount};
        island.distanceIndices = {flatDistances_.data() + r.distanceBegin, r.distanceCount};
    }
}

void ContactIslandGraph::build(u32 bodyCount,
                               const std::vector<narrowphase::ContactManifold>& contacts,
                               const std::vector<DistanceConstraint>& distanceConstraints) {
    // All storage is reused across builds; within the reserve()d limits nothing allocates.
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

    std::vector<u32>& rootToIsland = rootToIsland_;
    rootToIsland.assign(bodyCount, invalidIsland);
    u32 islandCount = 0;
    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        const u32 root = findRoot(bodyIndex);
        if (rootToIsland[root] == invalidIsland) {
            rootToIsland[root] = islandCount++;
        }
    }
    islandCount_ = islandCount;
    if (islands_.size() < islandCount) {
        islands_.resize(islandCount);
    }
    ranges_.assign(islandCount, Range{});

    // Pass 1: count each island's bodies, contacts and distance constraints.
    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        ++ranges_[rootToIsland[findRoot(bodyIndex)]].bodyCount;
    }
    u32 contactTotal = 0;
    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid || !contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            continue;
        }
        ++ranges_[rootToIsland[findRoot(contact.bodyA)]].contactCount;
        ++contactTotal;
    }
    u32 distanceTotal = 0;
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            continue;
        }
        ++ranges_[rootToIsland[findRoot(constraint.bodyA)]].distanceCount;
        ++distanceTotal;
    }

    // Prefix sums: each island owns a contiguous range of the flat arrays.
    u32 bodyOffset = 0;
    u32 contactOffset = 0;
    u32 distanceOffset = 0;
    for (Range& r : ranges_) {
        r.bodyBegin = bodyOffset;
        r.contactBegin = contactOffset;
        r.distanceBegin = distanceOffset;
        bodyOffset += r.bodyCount;
        contactOffset += r.contactCount;
        distanceOffset += r.distanceCount;
    }
    flatBodies_.resize(bodyCount);
    flatContacts_.resize(contactTotal);
    flatDistances_.resize(distanceTotal);

    // Pass 2: scatter in ascending index order (the same order the lists always had).
    cursor_.resize(islandCount);
    for (u32 i = 0; i < islandCount; ++i) {
        cursor_[i] = ranges_[i].bodyBegin;
    }
    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        flatBodies_[cursor_[rootToIsland[findRoot(bodyIndex)]]++] = bodyIndex;
    }
    for (u32 i = 0; i < islandCount; ++i) {
        cursor_[i] = ranges_[i].contactBegin;
    }
    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid || !contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            continue;
        }
        flatContacts_[cursor_[rootToIsland[findRoot(contact.bodyA)]]++] = contactIndex;
    }
    for (u32 i = 0; i < islandCount; ++i) {
        cursor_[i] = ranges_[i].distanceBegin;
    }
    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            continue;
        }
        flatDistances_[cursor_[rootToIsland[findRoot(constraint.bodyA)]]++] = distanceIndex;
    }

    // Stale slots beyond islandCount_ keep no views into the flat arrays.
    for (usize i = islandCount; i < islands_.size(); ++i) {
        islands_[i] = Island{};
    }
    rebindSpans_();

    // Islands are numbered in order of their lowest body index already (bodies are visited in
    // ascending order), which is the ordering callers rely on.
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
}

u32 ContactIslandGraph::constrainedIslandCount() const {
    u32 count = 0;
    for (u32 i = 0; i < islandCount_; ++i) {
        if (!islands_[i].isEmpty()) {
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
    for (u32 islandIndex = 0; islandIndex < islandCount_; ++islandIndex) {
        for (u32 index : islands_[islandIndex].bodyIndices) {
            if (index == bodyIndex || findRoot(index) == root) {
                return islandIndex;
            }
        }
    }
    return invalidIsland;
}

} // namespace fuse::physics
