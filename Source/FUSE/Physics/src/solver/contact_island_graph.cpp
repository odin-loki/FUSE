#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {
namespace {

bool contact_body_indices_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_body_indices_in_range(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;

} // namespace

bool is_valid_island_build_body_count(u32 bodyCount) {
    return bodyCount > 0u;

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            ++preflight.skippedContactCount;
            continue;
        ++preflight.validContactCount;
        if (contact_body_indices_in_range(contact, bodyCount)) {
            ++preflight.inRangeContactCount;
            ++preflight.unionCandidateCount;
        } else {
            ++preflight.outOfRangeContactCount;

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_body_indices_in_range(constraint, bodyCount)) {
            ++preflight.inRangeDistanceCount;
            ++preflight.outOfRangeDistanceCount;

    const bool hasConstraintData =
        preflight.validContactCount > 0u || !distanceConstraints.empty();
    preflight.skipped = bodyCount == 0u && hasConstraintData;
    return preflight;

bool should_skip_island_build(
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();

bool build_contact_island_graph_guarded(ContactIslandGraph& graph,
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    if (!preflight.can_build()) {
        graph.clear();
        return false;
    graph.build(bodyCount, contacts, distanceConstraints);
    return true;

namespace {

bool contactBodiesInRange(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyA < bodyCount && bodyB < bodyCount;

ContactIslandGraphBuildRejectReason diagnoseBuildRejectReason(
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    bool outOfRangeContact = false;
    bool outOfRangeDistance = false;
const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyBodyCount:
        return "EmptyBodyCount";
    case IslandBuildRejectReason::InvalidContactBodyIndex:
        return "InvalidContactBodyIndex";
    case IslandBuildRejectReason::InvalidDistanceBodyIndex:
        return "InvalidDistanceBodyIndex";
    return "Unknown";

bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;

bool contact_references_valid_bodies(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return is_valid_island_build_body_index(contact.bodyA, bodyCount) &&
           is_valid_island_build_body_index(contact.bodyB, bodyCount);

bool distance_constraint_references_valid_bodies(const DistanceConstraint& constraint, u32 bodyCount) {
    return is_valid_island_build_body_index(constraint.bodyA, bodyCount) &&
           is_valid_island_build_body_index(constraint.bodyB, bodyCount);

IslandBuildStats compute_island_build_stats(
    IslandBuildStats stats{};
    stats.bodyCount = bodyCount;
bool bodyIndexInRange(u32 bodyIndex, u32 bodyCount) {



IslandBuildInputStats compute_island_build_input_stats(
    IslandBuildInputStats stats{};
    stats.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());

        if (contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++inRangeContactCount;
            outOfRangeContact = true;

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


const char* contactIslandGraphBuildRejectReasonName(ContactIslandGraphBuildRejectReason reason) {
    case ContactIslandGraphBuildRejectReason::None:
    case ContactIslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:

ContactIslandGraphBuildRejectReason contactIslandGraphBuildRejectReason(
    return diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);

bool contactIslandGraphBuildRejectsForReason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    ContactIslandGraphBuildRejectReason expected) {
    return contactIslandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(
    ContactIslandGraphBuildPreflight preflight{};
    preflight.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

        if (contact.valid) {
        if (contact.valid && contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        } else if (contact.valid) {
            ++preflight.outOfRangeContactBodyCount;
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    case IslandBuildRejectReason::SelfPair:
        return "SelfPair";
    case IslandBuildRejectReason::OutOfRangeBody:
        return "OutOfRangeBody";
    case IslandBuildRejectReason::InvalidContact:
        return "InvalidContact";

IslandBuildPreflight preflight_island_build(u32 bodyCount,

        const IslandBuildRejectReason reason = contactBuildRejectReason(contact, bodyCount);
            break;
            ++preflight.invalidContactCount;
            ++preflight.selfPairContactCount;
            ++preflight.oobContactCount;

            ++preflight.outOfRangeDistanceBodyCount;

    preflight.reason = diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == ContactIslandGraphBuildRejectReason::EmptyInput;

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
    stats.contactCount = static_cast<u32>(contacts.size());

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

bool distance_body_indices_in_range(u32 bodyCount, const DistanceConstraint& constraint) {


bool is_valid_island_build_body_count(u32 bodyCount,
        if (!contact_body_indices_in_range(bodyCount, contact)) {

        if (!distance_body_indices_in_range(bodyCount, constraint)) {




    preflight.invalidBodyCount =
        preflight.outOfRangeContactCount > 0u || preflight.outOfRangeDistanceCount > 0u;

bool body_index_in_range(u32 bodyIndex, u32 bodyCount) {

bool contact_pair_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return body_index_in_range(contact.bodyA, bodyCount) && body_index_in_range(contact.bodyB, bodyCount);

bool distance_pair_in_range(const DistanceConstraint& constraint, u32 bodyCount) {
    return body_index_in_range(constraint.bodyA, bodyCount) && body_index_in_range(constraint.bodyB, bodyCount);


    preflight.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());

        if (!contact_pair_in_range(contact, bodyCount)) {
            ++preflight.rejects.invalidContactPairCount;

        if (!distance_pair_in_range(constraint, bodyCount)) {
            ++preflight.rejects.invalidDistancePairCount;


bool should_skip_island_build(u32 bodyCount) {

bool island_build_inputs_valid(u32 bodyCount,
    return preflight.can_build() && preflight.rejects.invalidContactPairCount == 0u &&
           preflight.rejects.invalidDistancePairCount == 0u;


bool is_valid_island_contact_for_build(const narrowphase::ContactManifold& contact, u32 bodyCount) {

bool is_valid_island_distance_for_build(const DistanceConstraint& constraint, u32 bodyCount) {




u32 count_valid_island_build_contacts(const std::vector<narrowphase::ContactManifold>& contacts,
                                      u32 bodyCount) {
    u32 count = 0u;
        if (is_valid_island_contact_for_build(contact, bodyCount)) {
            ++count;
    return count;

u32 count_valid_island_build_distance_constraints(
        if (is_valid_island_distance_for_build(constraint, bodyCount)) {



    preflight.stats.bodyCount = bodyCount;
    preflight.stats.validContactCount = count_valid_island_build_contacts(contacts, bodyCount);
    preflight.stats.skippedInvalidContactCount =
        static_cast<u32>(contacts.size()) - preflight.stats.validContactCount;
    preflight.stats.validDistanceCount =
        count_valid_island_build_distance_constraints(distanceConstraints, bodyCount);
    preflight.stats.skippedInvalidDistanceCount =
        static_cast<u32>(distanceConstraints.size()) - preflight.stats.validDistanceCount;
    preflight.zeroBodies = !is_valid_island_build_body_count(bodyCount);
    preflight.skipped = preflight.zeroBodies;

    return !is_valid_island_build_body_count(bodyCount);

bool build_island_graph_guarded(
    ContactIslandGraph& graph,
        if (contact_references_valid_bodies(contact, bodyCount)) {
            ++stats.validContactCount;
            ++stats.invalidContactCount;

        if (distance_constraint_references_valid_bodies(constraint, bodyCount)) {
            ++stats.validDistanceCount;
            ++stats.invalidDistanceCount;


IslandBuildRejectReason island_build_reject_reason(
    if (bodyCount == 0u) {
        return IslandBuildRejectReason::EmptyBodyCount;

IslandBuildInput count_island_build_input(
    IslandBuildInput input{};
    input.bodyCount = bodyCount;
    input.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());




        if (!contact_references_valid_bodies(contact, bodyCount)) {
            return IslandBuildRejectReason::InvalidContactBodyIndex;

        if (!distance_constraint_references_valid_bodies(constraint, bodyCount)) {
            return IslandBuildRejectReason::InvalidDistanceBodyIndex;

    return IslandBuildRejectReason::None;

bool island_build_rejects_for_reason(
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;

    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason != IslandBuildRejectReason::None;


bool build_island_graph_guarded(u32 bodyCount,
    ContactIslandGraph graph;
    return graph.build_guarded(bodyCount, contacts, distanceConstraints);
            ++input.invalidContactCount;
        ++input.validContactCount;

            ++input.invalidConstraintCount;

    return input;
        if (!bodyIndexInRange(contact.bodyA, bodyCount) || !bodyIndexInRange(contact.bodyB, bodyCount)) {
            ++stats.invalidContactBodyRefs;

        if (!bodyIndexInRange(constraint.bodyA, bodyCount) || !bodyIndexInRange(constraint.bodyB, bodyCount)) {
            ++stats.invalidDistanceBodyRefs;

    case IslandBuildRejectReason::ZeroBodies:
        return "ZeroBodies";
    case IslandBuildRejectReason::NoConstraints:
        return "NoConstraints";

IslandBuildStats compute_island_build_input_stats(

    stats.constraintEdgeCount = stats.validContactCount + stats.distanceConstraintCount;

        return IslandBuildRejectReason::ZeroBodies;








    const IslandBuildStats stats = compute_island_build_input_stats(bodyCount, contacts, distanceConstraints);
    if (stats.constraintEdgeCount == 0u) {
        return IslandBuildRejectReason::NoConstraints;




    preflight.input = count_island_build_input(bodyCount, contacts, distanceConstraints);
        preflight.reason = IslandBuildRejectReason::EmptyBodyCount;
        preflight.skipped = true;

    preflight.stats = compute_island_build_input_stats(bodyCount, contacts, distanceConstraints);
    preflight.emptyBodyCount = bodyCount == 0u;
    preflight.skipped = preflight.emptyBodyCount;
    preflight.zeroBodies = preflight.reason == IslandBuildRejectReason::ZeroBodies;
    preflight.noConstraints = preflight.reason == IslandBuildRejectReason::NoConstraints;


bool build_island_graph_guarded(ContactIslandGraph& graph,




u32 count_valid_island_contacts(u32 bodyCount,
                              u32* invalidContactCountOut) {
    u32 validCount = 0;
    u32 invalidCount = 0;
            ++invalidCount;
        ++validCount;
    if (invalidContactCountOut != nullptr) {
        *invalidContactCountOut = invalidCount;
    return validCount;

bool has_island_build_constraints(
    if (count_valid_island_contacts(bodyCount, contacts) > 0u) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {

    preflight.validContactCount =
        count_valid_island_contacts(bodyCount, contacts, &preflight.invalidContactCount);

            ++preflight.invalidDistanceCount;

    preflight.noConstraints = !has_island_build_constraints(bodyCount, contacts, distanceConstraints);














    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {

    preflight.contactCount = static_cast<u32>(contacts.size());



    preflight.skipped = bodyCount == 0u &&
                        (preflight.contactCount > 0u || preflight.distanceConstraintCount > 0u);



bool contact_body_in_range(u32 bodyCount, u32 bodyIndex) {

bool contact_pair_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact_body_in_range(bodyCount, contact.bodyA) && contact_body_in_range(bodyCount, contact.bodyB);

bool distance_pair_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return contact_body_in_range(bodyCount, constraint.bodyA) && contact_body_in_range(bodyCount, constraint.bodyB);


    case IslandBuildRejectReason::OutOfRangeContactBody:
        return "OutOfRangeContactBody";
    case IslandBuildRejectReason::OutOfRangeDistanceBody:
        return "OutOfRangeDistanceBody";
bool contact_body_refs_in_range(u32 bodyA, u32 bodyB, u32 bodyCount) {


    case IslandBuildRejectReason::ZeroBodyCount:
        return "ZeroBodyCount";
    case IslandBuildRejectReason::StaleContactBodyRefs:
        return "StaleContactBodyRefs";
    case IslandBuildRejectReason::StaleDistanceBodyRefs:
        return "StaleDistanceBodyRefs";
    }
    return "Unknown";
}

bool has_out_of_range_contact_body(u32 bodyCount, const std::vector<narrowphase::ContactManifold>& contacts) {
    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact_pair_in_range(bodyCount, contact)) {
            return true;
        }
    return false;

bool has_out_of_range_distance_body(u32 bodyCount, const std::vector<DistanceConstraint>& distanceConstraints) {
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_pair_in_range(bodyCount, constraint)) {

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (has_out_of_range_contact_body(bodyCount, contacts)) {
        return IslandBuildRejectReason::OutOfRangeContactBody;
    if (has_out_of_range_distance_body(bodyCount, distanceConstraints)) {
        return IslandBuildRejectReason::OutOfRangeDistanceBody;
    return IslandBuildRejectReason::None;

bool island_build_rejects_for_reason(u32 bodyCount,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactCount = static_cast<u32>(contacts.size());
    preflight.distanceCount = static_cast<u32>(distanceConstraints.size());
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact_pair_in_range(bodyCount, contact)) {
            ++preflight.outOfRangeContactCount;
        }
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_pair_in_range(bodyCount, constraint)) {
            ++preflight.outOfRangeDistanceCount;

    preflight.skipped = preflight.reason != IslandBuildRejectReason::None;
    preflight.zeroBodyCount = bodyCount == 0u;

    if (preflight.zeroBodyCount) {
        preflight.reason = IslandBuildRejectReason::ZeroBodyCount;
        return preflight;

        if (!contact.valid) {
            continue;
        if (!contact_body_refs_in_range(contact.bodyA, contact.bodyB, bodyCount)) {
            ++preflight.staleContactRefCount;

        if (!contact_body_refs_in_range(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++preflight.staleDistanceRefCount;

    if (preflight.staleContactRefCount > 0u) {
        preflight.reason = IslandBuildRejectReason::StaleContactBodyRefs;
    } else if (preflight.staleDistanceRefCount > 0u) {
        preflight.reason = IslandBuildRejectReason::StaleDistanceBodyRefs;

    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool build_guarded(ContactIslandGraph& graph,
                   u32 bodyCount,
                   const std::vector<narrowphase::ContactManifold>& contacts,
                   const std::vector<DistanceConstraint>& distanceConstraints) {
bool build_island_graph_guarded(ContactIslandGraph& graph,
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
        return false;
    }
    graph.build(bodyCount, contacts, distanceConstraints);
    return true;
}

namespace {

bool contact_body_indices_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_body_indices_in_range(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;

} // namespace

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::ZeroBodies:
        return "ZeroBodies";
    default:
        return "Unknown";

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (bodyCount == 0u) {
        return IslandBuildRejectReason::ZeroBodies;
    (void)contacts;
    (void)distanceConstraints;
    return IslandBuildRejectReason::None;

bool island_build_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;

IslandBuildPreflight preflight_island_build(
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactCount = static_cast<u32>(contacts.size());
    preflight.distanceCount = static_cast<u32>(distanceConstraints.size());
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.zeroBodies = preflight.reason == IslandBuildRejectReason::ZeroBodies;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            ++preflight.skippedContactCount;
            continue;
        ++preflight.validContactCount;
        if (!contact_body_indices_in_range(contact, bodyCount)) {
        ++preflight.inRangeContactCount;
        ++preflight.connectableConstraintCount;

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_body_indices_in_range(constraint, bodyCount)) {
            ++preflight.skippedDistanceCount;
        ++preflight.inRangeDistanceCount;

    return preflight;

bool should_skip_island_build(
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();

namespace {

bool contactBodiesInRange(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distanceBodiesInRange(u32 bodyCount, const DistanceConstraint& constraint) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

} // namespace

const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::ZeroBodyCount:
        return "ZeroBodyCount";
    case IslandBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case IslandBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";
    }
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (bodyCount == 0u) {
        return IslandBuildRejectReason::ZeroBodyCount;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (!contactBodiesInRange(bodyCount, contact)) {
            return IslandBuildRejectReason::OutOfRangeContactBodies;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distanceBodiesInRange(bodyCount, constraint)) {
            return IslandBuildRejectReason::OutOfRangeDistanceBodies;
        }
    }

    return IslandBuildRejectReason::None;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason != IslandBuildRejectReason::None;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        ++preflight.ownedContactCount;
        if (contactBodiesInRange(bodyCount, contact)) {
            ++preflight.inRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        ++preflight.ownedDistanceCount;
        if (distanceBodiesInRange(bodyCount, constraint)) {
            ++preflight.inRangeDistanceCount;
        }
    }

    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool can_build_island_graph(u32 bodyCount,
                            const std::vector<narrowphase::ContactManifold>& contacts,
                            const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

namespace {

bool contact_body_indices_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_body_indices_in_range(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

} // namespace

bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;
}

IslandBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;

    if (bodyCount == 0u) {
        preflight.reason = IslandBuildRejectReason::ZeroBodies;
        preflight.skipped = true;
        return preflight;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contact_body_indices_in_range(contact, bodyCount)) {
            ++preflight.validContactCount;
        } else {
            ++preflight.skippedContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_body_indices_in_range(constraint, bodyCount)) {
            ++preflight.validDistanceCount;
        } else {
            ++preflight.skippedDistanceCount;
        }
    }

    if (preflight.skippedContactCount > 0u || preflight.skippedDistanceCount > 0u) {
        preflight.reason = IslandBuildRejectReason::OutOfRangeBodies;
        preflight.skipped = true;
    }

    return preflight;
}

bool should_skip_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandBuildPreflight preflight = preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
    if (!preflight.can_build()) {
        return false;
    }
    graph.build(bodyCount, contacts, distanceConstraints);
    return true;
}

namespace {

bool contact_refs_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_refs_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

} // namespace

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;

    if (bodyCount == 0u) {
        preflight.reason = IslandBuildRejectReason::ZeroBodies;
        preflight.skipped = !contacts.empty() || !distanceConstraints.empty();
        return preflight;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact_refs_in_range(bodyCount, contact)) {
            ++preflight.staleContactCount;
            continue;
        }
        if (contact.valid) {
            ++preflight.validContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_refs_in_range(bodyCount, constraint)) {
            ++preflight.validDistanceCount;
        } else {
            ++preflight.staleDistanceCount;
        }
    }

    if (preflight.validContactCount == 0u && preflight.validDistanceCount == 0u &&
        (!contacts.empty() || !distanceConstraints.empty())) {
        preflight.reason = IslandBuildRejectReason::AllConstraintsStale;
        preflight.skipped = true;
    }

    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool has_usable_island_build_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    return preflight.validContactCount > 0u || preflight.validDistanceCount > 0u;
}

bool contact_bodies_in_range(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

bool distance_constraint_bodies_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return contact_bodies_in_range(bodyCount, constraint.bodyA, constraint.bodyB);
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactCount = static_cast<u32>(contacts.size());
    preflight.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());

    if (bodyCount == 0u) {
        preflight.skipped = true;
        return preflight;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact_bodies_in_range(bodyCount, contact.bodyA, contact.bodyB)) {
            ++preflight.inRangeContactCount;
            if (contact.valid) {
                ++preflight.validContactCount;
            }
        } else {
            ++preflight.outOfRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_constraint_bodies_in_range(bodyCount, constraint)) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceCount;
        }
    }

    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).skipped;
}

bool is_island_build_body_index_valid(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;
}

bool is_contact_valid_for_island_build(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    if (!contact.valid) {
        return false;
    }
    return is_island_build_body_index_valid(contact.bodyA, bodyCount) &&
           is_island_build_body_index_valid(contact.bodyB, bodyCount);
}

bool is_distance_constraint_valid_for_island_build(const DistanceConstraint& constraint, u32 bodyCount) {
    return is_island_build_body_index_valid(constraint.bodyA, bodyCount) &&
           is_island_build_body_index_valid(constraint.bodyB, bodyCount);
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (is_contact_valid_for_island_build(contact, bodyCount)) {
            ++preflight.validContactCount;
        } else if (contact.valid) {
            ++preflight.staleContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (is_distance_constraint_valid_for_island_build(constraint, bodyCount)) {
            ++preflight.validDistanceCount;
        } else {
            ++preflight.staleDistanceCount;
        }
    }

    preflight.skipped =
        bodyCount == 0u && contacts.empty() && distanceConstraints.empty();
    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).skipped;
}

IslandBuildStats compute_island_build_stats(const ContactIslandGraph& graph) {
    IslandBuildStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        if (graph.island(islandIndex).isEmpty()) {
            ++stats.emptyCount;
        } else {
            ++stats.constrainedCount;
        }
    }
    return stats;
}

namespace {

bool contact_indices_valid(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.valid && contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_indices_valid(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

} // namespace

bool is_valid_island_build_body_count(u32 bodyCount) {
    return true;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact_indices_valid(contact, bodyCount)) {
            ++preflight.validContactCount;
        } else {
            ++preflight.invalidContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_indices_valid(constraint, bodyCount)) {
            ++preflight.validDistanceCount;
        } else {
            ++preflight.invalidDistanceCount;
        }
    }

    preflight.skipped = bodyCount == 0u && !preflight.has_valid_constraints();
    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).skipped;
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

bool ContactIslandGraph::build_guarded(u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints) {
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
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
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!contact_pair_in_range(contact, bodyCount)) {
            continue;
        }
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!distance_pair_in_range(constraint, bodyCount)) {
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
        if (!contact_pair_in_range(contact, bodyCount)) {
        if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
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
        if (!distance_pair_in_range(constraint, bodyCount)) {
        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
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
bool ContactIslandGraph::build_guarded(u32 bodyCount,
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    if (!preflight.can_build()) {
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
void ContactIslandGraph::build_guarded(u32 bodyCount,
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
        return;
    if (should_skip_island_build(bodyCount)) {
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
