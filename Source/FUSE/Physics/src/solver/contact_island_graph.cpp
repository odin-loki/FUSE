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

bool has_out_of_range_contact_body(u32 bodyCount, const std::vector<narrowphase::ContactManifold>& contacts) {
        if (!contact_pair_in_range(bodyCount, contact)) {

bool has_out_of_range_distance_body(u32 bodyCount, const std::vector<DistanceConstraint>& distanceConstraints) {
        if (!distance_pair_in_range(bodyCount, constraint)) {

    if (has_out_of_range_contact_body(bodyCount, contacts)) {
        return IslandBuildRejectReason::OutOfRangeContactBody;
    if (has_out_of_range_distance_body(bodyCount, distanceConstraints)) {
        return IslandBuildRejectReason::OutOfRangeDistanceBody;

bool island_build_rejects_for_reason(u32 bodyCount,

    preflight.distanceCount = static_cast<u32>(distanceConstraints.size());


    preflight.zeroBodyCount = bodyCount == 0u;

    if (preflight.zeroBodyCount) {
        preflight.reason = IslandBuildRejectReason::ZeroBodyCount;

        if (!contact_body_refs_in_range(contact.bodyA, contact.bodyB, bodyCount)) {
            ++preflight.staleContactRefCount;

        if (!contact_body_refs_in_range(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++preflight.staleDistanceRefCount;

    if (preflight.staleContactRefCount > 0u) {
        preflight.reason = IslandBuildRejectReason::StaleContactBodyRefs;
    } else if (preflight.staleDistanceRefCount > 0u) {
        preflight.reason = IslandBuildRejectReason::StaleDistanceBodyRefs;



bool build_guarded(ContactIslandGraph& graph,






    (void)contacts;
    (void)distanceConstraints;



        if (!contact_body_indices_in_range(contact, bodyCount)) {
        ++preflight.connectableConstraintCount;

        if (!distance_body_indices_in_range(constraint, bodyCount)) {
            ++preflight.skippedDistanceCount;




bool contactBodiesInRange(u32 bodyCount, const narrowphase::ContactManifold& contact) {

bool distanceBodiesInRange(u32 bodyCount, const DistanceConstraint& constraint) {


    case IslandBuildRejectReason::OutOfRangeContactBodies:
    case IslandBuildRejectReason::OutOfRangeDistanceBodies:

        return IslandBuildRejectReason::ZeroBodyCount;

        if (!contactBodiesInRange(bodyCount, contact)) {
            return IslandBuildRejectReason::OutOfRangeContactBodies;

        if (!distanceBodiesInRange(bodyCount, constraint)) {
            return IslandBuildRejectReason::OutOfRangeDistanceBodies;



        ++preflight.ownedContactCount;
        if (contactBodiesInRange(bodyCount, contact)) {

        ++preflight.ownedDistanceCount;
        if (distanceBodiesInRange(bodyCount, constraint)) {



bool can_build_island_graph(u32 bodyCount,
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();







        preflight.reason = IslandBuildRejectReason::ZeroBodies;



    if (preflight.skippedContactCount > 0u || preflight.skippedDistanceCount > 0u) {
        preflight.reason = IslandBuildRejectReason::OutOfRangeBodies;


bool should_skip_island_graph_build(
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();

    const IslandBuildPreflight preflight = preflight_island_graph_build(bodyCount, contacts, distanceConstraints);


bool contact_refs_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {

bool distance_refs_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
bool isOutOfRangeBodyRef(u32 bodyCount, u32 bodyIndex) {
    return bodyIndex >= bodyCount;

u32 countOutOfRangeContactRefs(u32 bodyCount,
                               const std::vector<narrowphase::ContactManifold>& contacts) {
    u32 count = 0;
        if (isOutOfRangeBodyRef(bodyCount, contact.bodyA) || isOutOfRangeBodyRef(bodyCount, contact.bodyB)) {

u32 countOutOfRangeDistanceRefs(u32 bodyCount,
        if (isOutOfRangeBodyRef(bodyCount, constraint.bodyA) || isOutOfRangeBodyRef(bodyCount, constraint.bodyB)) {



        preflight.skipped = !contacts.empty() || !distanceConstraints.empty();

        if (!contact_refs_in_range(bodyCount, contact)) {
            ++preflight.staleContactCount;

        if (distance_refs_in_range(bodyCount, constraint)) {
            ++preflight.staleDistanceCount;

    if (preflight.validContactCount == 0u && preflight.validDistanceCount == 0u &&
        (!contacts.empty() || !distanceConstraints.empty())) {
        preflight.reason = IslandBuildRejectReason::AllConstraintsStale;



bool has_usable_island_build_constraints(
    return preflight.validContactCount > 0u || preflight.validDistanceCount > 0u;

bool contact_bodies_in_range(u32 bodyCount, u32 bodyA, u32 bodyB) {

bool distance_constraint_bodies_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return contact_bodies_in_range(bodyCount, constraint.bodyA, constraint.bodyB);
    const u32 outOfRangeCount =
        countOutOfRangeContactRefs(bodyCount, contacts) + countOutOfRangeDistanceRefs(bodyCount, distanceConstraints);
    if (outOfRangeCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeBodyRef;



        if (contact_bodies_in_range(bodyCount, contact.bodyA, contact.bodyB)) {

        if (distance_constraint_bodies_in_range(bodyCount, constraint)) {



    if (bodyCount == 0u && contacts.empty() && distanceConstraints.empty()) {

        ++preflight.validInRangeContactCount;




bool is_island_build_body_index_valid(u32 bodyIndex, u32 bodyCount) {

bool is_contact_valid_for_island_build(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return is_island_build_body_index_valid(contact.bodyA, bodyCount) &&
           is_island_build_body_index_valid(contact.bodyB, bodyCount);

bool is_distance_constraint_valid_for_island_build(const DistanceConstraint& constraint, u32 bodyCount) {
    return is_island_build_body_index_valid(constraint.bodyA, bodyCount) &&
           is_island_build_body_index_valid(constraint.bodyB, bodyCount);


        if (is_contact_valid_for_island_build(contact, bodyCount)) {

        if (is_distance_constraint_valid_for_island_build(constraint, bodyCount)) {

    preflight.skipped =
        bodyCount == 0u && contacts.empty() && distanceConstraints.empty();


IslandBuildStats compute_island_build_stats(const ContactIslandGraph& graph) {
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        if (graph.island(islandIndex).isEmpty()) {
            ++stats.emptyCount;
            ++stats.constrainedCount;


bool contact_indices_valid(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.valid && contact.bodyA < bodyCount && contact.bodyB < bodyCount;

bool distance_indices_valid(const DistanceConstraint& constraint, u32 bodyCount) {




        if (contact_indices_valid(contact, bodyCount)) {

        if (distance_indices_valid(constraint, bodyCount)) {

    preflight.skipped = bodyCount == 0u && !preflight.has_valid_constraints();


bool contact_references_in_range_body(u32 bodyA, u32 bodyB, u32 bodyCount) {

bool distance_constraint_references_in_range_body(const DistanceConstraint& constraint, u32 bodyCount) {
    return contact_references_in_range_body(constraint.bodyA, constraint.bodyB, bodyCount);


    preflight.ownedContactCount = static_cast<u32>(contacts.size());
    preflight.ownedDistanceCount = static_cast<u32>(distanceConstraints.size());

        if (!contact_references_in_range_body(contact.bodyA, contact.bodyB, bodyCount)) {

        if (distance_constraint_references_in_range_body(constraint, bodyCount)) {



void build_island_graph_guarded(ContactIslandGraph& graph,
        return;
            ++preflight.outOfRangeBodyRefCount;


    preflight.rejected = preflight.reason != IslandBuildRejectReason::None;

    return !preflight_island_build(bodyCount, contacts, distanceConstraints).has_constraints();
bool is_contact_in_body_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {

bool is_distance_in_body_range(const DistanceConstraint& constraint, u32 bodyCount) {


IslandBuildStats compute_island_build_stats(const ContactIslandGraph& graph,
                                            const IslandBuildPreflight& inputPreflight) {
    stats.constrainedCount = graph.constrainedIslandCount();
    stats.emptyCount = stats.totalIslands - stats.constrainedCount;
    stats.orphanContactCount = inputPreflight.outOfRangeContactCount + inputPreflight.invalidContactCount;
    stats.orphanDistanceCount = inputPreflight.outOfRangeDistanceCount;

    case ContactIslandGraphBuildRejectReason::UnsafeRefs:
        return "UnsafeRefs";
    case ContactIslandGraphBuildRejectReason::SelfContact:
        return "SelfContact";

    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

            ++preflight.stats.validContactCount;
        if (contact.bodyA == contact.bodyB) {
            ++preflight.stats.selfContactCount;
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
            ++preflight.stats.outOfRangeContactBodyCount;

        if (constraint.bodyA == constraint.bodyB) {
            ++preflight.stats.inRangeDistanceCount;
            ++preflight.stats.outOfRangeDistanceBodyCount;

    if (bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
        preflight.stats.inRangeDistanceCount == 0u) {
        preflight.reason = ContactIslandGraphBuildRejectReason::EmptyInput;
    } else if (preflight.has_unsafe_refs()) {
        preflight.reason = ContactIslandGraphBuildRejectReason::UnsafeRefs;
    } else if (preflight.has_self_contacts()) {
        preflight.reason = ContactIslandGraphBuildRejectReason::SelfContact;


bool shouldSkipContactIslandGraphBuild(u32 bodyCount,

    case IslandBuildRejectReason::EmptyInput:
    case IslandBuildRejectReason::UnsafeRefs:

ContactIslandBuildInputStats compute_contact_island_build_input_stats(
    ContactIslandBuildInputStats stats{};
    stats.contactSlotCount = static_cast<u32>(contacts.size());
    stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

            ++stats.inRangeContactCount;
            ++stats.outOfRangeContactBodyCount;

            ++stats.inRangeDistanceCount;
            ++stats.outOfRangeDistanceBodyCount;


    const ContactIslandBuildInputStats stats =
        compute_contact_island_build_input_stats(bodyCount, contacts, distanceConstraints);

    if (bodyCount == 0u && stats.inRangeContactCount == 0u && stats.inRangeDistanceCount == 0u) {
        return IslandBuildRejectReason::EmptyInput;
    if (stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::UnsafeRefs;


bool can_skip_contact_island_build(
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) != IslandBuildRejectReason::None;

bool should_run_contact_island_build(
    return !can_skip_contact_island_build(bodyCount, contacts, distanceConstraints);

IslandBuildInputStats scan_island_build_inputs(




bool island_build_inputs_safe(u32 bodyCount,
    const IslandBuildInputStats stats = scan_island_build_inputs(bodyCount, contacts, distanceConstraints);
    return !stats.has_unsafe_refs();

bool ContactIslandGraph::bodies_in_range(u32 bodyA, u32 bodyB, u32 bodyCount) {

bool ContactIslandGraph::is_self_contact(u32 bodyA, u32 bodyB) {
    return bodyA == bodyB;



bool constraint_pair_is_degenerate(u32 bodyA, u32 bodyB) {

bool ContactIslandGraph::partitionBodyInRange(u32 bodyCount, u32 bodyIndex) {

bool ContactIslandGraph::contactPartitionInRange(u32 bodyCount,
                                               const narrowphase::ContactManifold& contact) {
    return partitionBodyInRange(bodyCount, contact.bodyA) && partitionBodyInRange(bodyCount, contact.bodyB);

bool ContactIslandGraph::distancePartitionInRange(u32 bodyCount, const DistanceConstraint& constraint) {
    return partitionBodyInRange(bodyCount, constraint.bodyA) &&
           partitionBodyInRange(bodyCount, constraint.bodyB);

u32 ContactIslandGraph::countUnionableContacts(u32 bodyCount,
        if (contact.valid && contactPartitionInRange(bodyCount, contact)) {

u32 ContactIslandGraph::countUnionableDistanceConstraints(
        if (distancePartitionInRange(bodyCount, constraint)) {

IslandGraphBuildPreflight preflightIslandGraphBuild(
    IslandGraphBuildPreflight preflight{};



    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;

bool shouldSkipIslandGraphBuild(u32 bodyCount,
    return !preflightIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();

bool contact_bodies_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {

bool distance_bodies_in_range(const DistanceConstraint& constraint, u32 bodyCount) {


bool bodiesInRange(u32 bodyA, u32 bodyB, u32 bodyCount) {








bool ContactIslandGraph::isBodyPairInRange(u32 bodyA, u32 bodyB, u32 bodyCount) {

bool ContactIslandGraph::isContactInRange(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return isBodyPairInRange(contact.bodyA, contact.bodyB, bodyCount);

bool ContactIslandGraph::isDistanceConstraintInRange(const DistanceConstraint& constraint, u32 bodyCount) {
    return isBodyPairInRange(constraint.bodyA, constraint.bodyB, bodyCount);











bool isInRangeBodyPair(u32 bodyA, u32 bodyB, u32 bodyCount) {
bool bodiesInRange(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

} // namespace

namespace {

bool bodyIndexInRange(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;
}

bool contactPairEligibleForBuild(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyA != bodyB && bodyIndexInRange(bodyA, bodyCount) && bodyIndexInRange(bodyB, bodyCount);
}

bool distancePairEligibleForBuild(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyIndexInRange(bodyA, bodyCount) && bodyIndexInRange(bodyB, bodyCount);
}

} // namespace

ContactIslandBuildPreflight preflight_contact_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool should_skip_contact_island_build(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_contact_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool body_pair_in_range(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

bool contact_manifold_bodies_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_constraint_bodies_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

IslandBuildInputScan scan_island_build_inputs(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildInputScan scan{};
    scan.bodyCount = bodyCount;
    scan.contactSlotCount = static_cast<u32>(contacts.size());
    scan.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++scan.validContactCount;
        }
        if (contact_manifold_bodies_in_range(bodyCount, contact)) {
            ++scan.inRangeContactCount;
        } else if (contact.valid) {
            ++scan.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_constraint_bodies_in_range(bodyCount, constraint)) {
            ++scan.inRangeDistanceCount;
        } else {
            ++scan.outOfRangeDistanceBodyCount;
        }
    }

    return scan;
}

bool island_build_inputs_safe(const IslandBuildInputScan& scan) {
    return !scan.has_unsafe_refs();
}

bool can_partition_island_build_inputs(u32 bodyCount, const IslandBuildInputScan& scan) {
    if (!island_build_inputs_safe(scan)) {
        return false;
    }
    return bodyCount > 0u || scan.inRangeContactCount > 0u || scan.inRangeDistanceCount > 0u;
}

bool island_build_body_pair_in_range(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

IslandGraphBuildStats count_island_graph_build_input(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildStats stats{};
    stats.bodyCount = bodyCount;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++stats.validContactCount;
        }
        if (island_build_body_pair_in_range(bodyCount, contact.bodyA, contact.bodyB)) {
            ++stats.inRangeContactCount;
        } else if (contact.valid) {
            ++stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (island_build_body_pair_in_range(bodyCount, constraint.bodyA, constraint.bodyB)) {
            ++stats.inRangeDistanceCount;
        } else {
            ++stats.outOfRangeDistanceBodyCount;
        }
    }

    return stats;
}

IslandGraphBuildPreflight preflight_contact_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.stats = count_island_graph_build_input(bodyCount, contacts, distanceConstraints);
    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool should_skip_contact_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_contact_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

IslandGraphBuildPreflight ContactIslandGraph::preflightBuildInputs(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
            if (contact.valid && contact.bodyA == contact.bodyB) {
                ++preflight.stats.selfReferencingContactCount;
            }
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        const bool inRange = constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeDistanceCount;
            if (constraint.bodyA == constraint.bodyB) {
                ++preflight.stats.selfReferencingDistanceCount;
            }
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool ContactIslandGraph::shouldSkipBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightBuildInputs(bodyCount, contacts, distanceConstraints).can_build();
}

bool ContactIslandGraph::buildGuarded(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (shouldSkipBuild(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
}

bool island_body_index_in_range(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;
}

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInputs:
        return "EmptyInputs";
    case IslandGraphBuildRejectReason::OutOfRangeContactBody:
        return "OutOfRangeContactBody";
    case IslandGraphBuildRejectReason::OutOfRangeDistanceBody:
        return "OutOfRangeDistanceBody";
    default:
        return "Unknown";
    }
}

IslandGraphBuildRejectReason island_graph_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (bodyCount == 0u && contacts.empty() && distanceConstraints.empty()) {
        return IslandGraphBuildRejectReason::EmptyInputs;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (!island_body_index_in_range(contact.bodyA, bodyCount) ||
            !island_body_index_in_range(contact.bodyB, bodyCount)) {
            return IslandGraphBuildRejectReason::OutOfRangeContactBody;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!island_body_index_in_range(constraint.bodyA, bodyCount) ||
            !island_body_index_in_range(constraint.bodyB, bodyCount)) {
            return IslandGraphBuildRejectReason::OutOfRangeDistanceBody;
        }
    }

    return IslandGraphBuildRejectReason::None;
}

bool island_graph_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason expected) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandGraphBuildRejectReason::UnsafeContactRefs:
        return "UnsafeContactRefs";
    case IslandGraphBuildRejectReason::UnsafeDistanceRefs:
        return "UnsafeDistanceRefs";
    default:
        return "Unknown";
    }
}

IslandGraphBuildRejectReason island_graph_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (bodyCount == 0u && contacts.empty() && distanceConstraints.empty()) {
        return IslandGraphBuildRejectReason::EmptyInput;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
            return IslandGraphBuildRejectReason::UnsafeContactRefs;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
            return IslandGraphBuildRejectReason::UnsafeDistanceRefs;
        }
    }

    return IslandGraphBuildRejectReason::None;
}

bool island_graph_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason expected) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

bool can_skip_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) !=
           IslandGraphBuildRejectReason::None;
}

bool should_run_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !can_skip_island_graph_build(bodyCount, contacts, distanceConstraints);
}

namespace {

bool contactBodiesInRange(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

} // namespace

const char* ContactIslandGraph::buildRejectReasonName(BuildRejectReason reason) {
    switch (reason) {
    case BuildRejectReason::None:
        return "None";
    case BuildRejectReason::EmptyInputs:
        return "EmptyInputs";
    case BuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case BuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";
    }
}

ContactIslandGraph::BuildRejectReason ContactIslandGraph::buildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    const BuildPreflight preflight = preflightBuild(bodyCount, contacts, distanceConstraints);
    return preflight.reason;
}

bool ContactIslandGraph::buildRejectsForReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    BuildRejectReason expected) {
    return buildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
}

ContactIslandGraph::BuildPreflight ContactIslandGraph::preflightBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    BuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        if (contactBodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.skippedOutOfRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.skippedOutOfRangeDistanceCount;
        }
    }

    if (bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
        preflight.stats.inRangeDistanceCount == 0u) {
        preflight.reason = BuildRejectReason::EmptyInputs;
        preflight.rejected = true;
        return preflight;
    }

    if (preflight.stats.skippedOutOfRangeContactCount > 0u) {
        preflight.reason = BuildRejectReason::OutOfRangeContactBodies;
        preflight.rejected = true;
        return preflight;
    }

    if (preflight.stats.skippedOutOfRangeDistanceCount > 0u) {
        preflight.reason = BuildRejectReason::OutOfRangeDistanceBodies;
        preflight.rejected = true;
        return preflight;
    }

    return preflight;
}

bool ContactIslandGraph::shouldSkipBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightBuild(bodyCount, contacts, distanceConstraints).can_build();
}

bool ContactIslandGraph::buildGuarded(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    if (shouldSkipBuild(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
}

bool contact_body_indices_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_body_indices_in_range(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

bool constraint_pair_is_degenerate(u32 bodyA, u32 bodyB) {
    return bodyA == bodyB;
}

namespace {

bool contactBodiesInRange(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distanceBodiesInRange(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

} // namespace

ContactIslandBuildPreflight preflight_contact_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        if (contactBodiesInRange(contact, bodyCount)) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distanceBodiesInRange(constraint, bodyCount)) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool should_skip_contact_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_contact_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

namespace {

bool contact_refs_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_refs_in_range(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

} // namespace

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInputs:
        return "EmptyInputs";
    case IslandBuildRejectReason::OutOfRangeContact:
        return "OutOfRangeContact";
    case IslandBuildRejectReason::OutOfRangeDistance:
        return "OutOfRangeDistance";
    }
    return "Unknown";
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contact_refs_in_range(contact, bodyCount)) {
            ++inRangeContactCount;
        } else {
            return IslandBuildRejectReason::OutOfRangeContact;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_refs_in_range(constraint, bodyCount)) {
            ++inRangeDistanceCount;
        } else {
            return IslandBuildRejectReason::OutOfRangeDistance;
        }
    }

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandBuildRejectReason::EmptyInputs;
    }

    return IslandBuildRejectReason::None;
}

bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

ContactIslandGraphBuildPreflight preflight_contact_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandGraphBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contact_refs_in_range(contact, bodyCount)) {
            ++preflight.inRangeContactCount;
        } else {
            ++preflight.outOfRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_refs_in_range(constraint, bodyCount)) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceCount;
        }
    }

    preflight.skipped = preflight.reason == IslandBuildRejectReason::EmptyInputs;
    return preflight;
}

bool can_build_contact_island_graph(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_contact_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool should_skip_island_graph_build(u32 bodyCount,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool contact_refs_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool distance_refs_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;
}

IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        if (contact_refs_in_range(bodyCount, contact)) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distance_refs_in_range(bodyCount, constraint)) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool should_skip_island_graph_build(u32 bodyCount,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandGraphBuildRejectReason::UnsafeContactRef:
        return "UnsafeContactRef";
    case IslandGraphBuildRejectReason::UnsafeDistanceRef:
        return "UnsafeDistanceRef";
    default:
        return "Unknown";
    }
}

IslandGraphBuildRejectReason island_graph_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 outOfRangeContactCount = 0;
    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++inRangeContactCount;
        } else {
            ++outOfRangeContactCount;
        }
    }

    u32 inRangeDistanceCount = 0;
    u32 outOfRangeDistanceCount = 0;
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++inRangeDistanceCount;
        } else {
            ++outOfRangeDistanceCount;
        }
    }

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandGraphBuildRejectReason::EmptyInput;
    }
    if (outOfRangeContactCount > 0u) {
        return IslandGraphBuildRejectReason::UnsafeContactRef;
    }
    if (outOfRangeDistanceCount > 0u) {
        return IslandGraphBuildRejectReason::UnsafeDistanceRef;
    }
    return IslandGraphBuildRejectReason::None;
}

bool island_graph_build_rejects_for_reason(u32 bodyCount,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           IslandGraphBuildRejectReason expected) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

ContactIslandGraphBuildPreflight preflight_contact_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandGraphBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.reason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contact.bodyA < bodyCount && contact.bodyB < bodyCount) {
            ++preflight.inRangeContactCount;
        } else {
            ++preflight.outOfRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceCount;
        }
    }

    preflight.skipped = preflight.reason == IslandGraphBuildRejectReason::EmptyInput;
    return preflight;
}

bool should_skip_contact_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_contact_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandGraphBuildRejectReason::OutOfRangeContactBody:
        return "OutOfRangeContactBody";
    case IslandGraphBuildRejectReason::OutOfRangeDistanceBody:
        return "OutOfRangeDistanceBody";
    }
    return "Unknown";
}

IslandGraphBuildRejectReason island_graph_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    bool hasInRangeContact = false;
    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.bodyA < bodyCount && contact.bodyB < bodyCount) {
            hasInRangeContact = true;
        } else if (contact.valid) {
            return IslandGraphBuildRejectReason::OutOfRangeContactBody;
        }
    }

    bool hasInRangeDistance = false;
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            hasInRangeDistance = true;
        } else {
            return IslandGraphBuildRejectReason::OutOfRangeDistanceBody;
        }
    }

    if (bodyCount == 0u && !hasInRangeContact && !hasInRangeDistance) {
        return IslandGraphBuildRejectReason::EmptyInput;
    }

    return IslandGraphBuildRejectReason::None;
}

bool island_graph_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason expected) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.reason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == IslandGraphBuildRejectReason::EmptyInput;
    return preflight;
}

bool should_skip_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandGraphBuildRejectReason::OutOfRangeContactBody:
        return "OutOfRangeContactBody";
    case IslandGraphBuildRejectReason::OutOfRangeDistanceBody:
        return "OutOfRangeDistanceBody";
    }
    return "Unknown";
}

IslandGraphBuildRejectReason island_graph_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;

    for (const narrowphase::ContactManifold& contact : contacts) {
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++inRangeContactCount;
        } else if (contact.valid) {
            ++outOfRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++inRangeDistanceCount;
        } else {
            ++outOfRangeDistanceCount;
        }
    }

    if (outOfRangeContactCount > 0u) {
        return IslandGraphBuildRejectReason::OutOfRangeContactBody;
    }
    if (outOfRangeDistanceCount > 0u) {
        return IslandGraphBuildRejectReason::OutOfRangeDistanceBody;
    }
    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandGraphBuildRejectReason::EmptyInput;
    }
    return IslandGraphBuildRejectReason::None;
}

bool island_graph_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason expected) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.reason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.validContactCount;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceBodyCount;
        }
    }

    return preflight;
}

bool can_build_island_graph(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
}

const char* islandGraphBuildRejectReasonName(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandGraphBuildRejectReason::OutOfRangeRefs:
        return "OutOfRangeRefs";
    }
    return "Unknown";
}

IslandGraphBuildRejectReason islandGraphBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

    for (const narrowphase::ContactManifold& contact : contacts) {
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++inRangeContactCount;
        } else if (contact.valid) {
            ++outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++inRangeDistanceCount;
        } else {
            ++outOfRangeDistanceBodyCount;
        }
    }

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandGraphBuildRejectReason::EmptyInput;
    }
    if (outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u) {
        return IslandGraphBuildRejectReason::OutOfRangeRefs;
    }
    return IslandGraphBuildRejectReason::None;
}

bool islandGraphBuildRejectsForReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason expected) {
    return islandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
}

void ContactIslandGraph::clear() {
    parent_.clear();
    bodyToIsland_.clear();
    islands_.clear();
}

bool ContactIslandGraph::bodiesInRange(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
bool ContactIslandGraph::contactInRange(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;
}

bool ContactIslandGraph::distanceInRange(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;

IslandBuildInputCoverage ContactIslandGraph::scanBuildInputs(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildInputCoverage coverage{};
    coverage.bodyCount = bodyCount;
    coverage.contactSlotCount = static_cast<u32>(contacts.size());
    coverage.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++coverage.validContactCount;
        if (contactInRange(contact, bodyCount)) {
            ++coverage.inRangeContactCount;
        } else if (contact.valid) {
            ++coverage.outOfRangeContactCount;

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distanceInRange(constraint, bodyCount)) {
            ++coverage.inRangeDistanceCount;
        } else {
            ++coverage.outOfRangeDistanceCount;

    return coverage;

bool ContactIslandGraph::canAcceptBuildInputs(const IslandBuildInputCoverage& coverage) {
    return !coverage.isEmptyInput() && !coverage.hasUnsafeRefs();

bool ContactIslandGraph::buildGuarded(u32 bodyCount,
    const IslandBuildInputCoverage coverage = scanBuildInputs(bodyCount, contacts, distanceConstraints);
    if (!canAcceptBuildInputs(coverage)) {
        clear();
        return false;
    build(bodyCount, contacts, distanceConstraints);
    return true;
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
    if (!bodiesInRange(static_cast<u32>(parent_.size()), a, b)) {
    if (!bodies_in_union_range(parent_.size(), a, b)) {
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
bool ContactIslandGraph::buildGuarded(u32 bodyCount,
    if (shouldSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints)) {
    const IslandBuildInputStats stats = scan_island_build_inputs(bodyCount, contacts, distanceConstraints);
    if (stats.has_unsafe_refs()) {
        clear();
        return false;
    }
    if (bodyCount == 0u && stats.inRangeContactCount == 0u && stats.inRangeDistanceCount == 0u) {
    build(bodyCount, contacts, distanceConstraints);
    return true;
bool ContactIslandGraph::bodyIndexInRange(u32 bodyIndex) const {
    return bodyIndex < parent_.size();
void ContactIslandGraph::buildInRange(u32 bodyCount,
                                      const std::vector<DistanceConstraint>& distanceConstraints,
                                      ContactIslandGraphBuildStats* outStats) {
    ContactIslandGraphBuildStats stats{};
    stats.bodyCount = bodyCount;

    parent_.resize(bodyCount);
    for (u32 i = 0; i < bodyCount; ++i) {
        parent_[i] = i;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
            ++stats.skippedOutOfRangeContactCount;
        ++stats.processedValidContactCount;
        unionBodies(contact.bodyA, contact.bodyB);

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
            ++stats.skippedOutOfRangeDistanceCount;
        ++stats.processedDistanceCount;
        unionBodies(constraint.bodyA, constraint.bodyB);

        compressPath(i);

    std::vector<u32> rootToIsland(bodyCount, invalidIsland);
    islands_.clear();

    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        const u32 root = findRoot(bodyIndex);
        if (rootToIsland[root] == invalidIsland) {
            rootToIsland[root] = static_cast<u32>(islands_.size());
            islands_.push_back({});
        islands_[rootToIsland[root]].bodyIndices.push_back(bodyIndex);

    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        const u32 islandIndex = rootToIsland[findRoot(contact.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].contactIndices.push_back(contactIndex);

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        const u32 islandIndex = rootToIsland[findRoot(constraint.bodyA)];
            islands_[islandIndex].distanceIndices.push_back(distanceIndex);

    std::sort(islands_.begin(), islands_.end(), [](const Island& left, const Island& right) {
        if (left.bodyIndices.empty() || right.bodyIndices.empty()) {
            return left.bodyIndices.size() < right.bodyIndices.size();
        return left.bodyIndices.front() < right.bodyIndices.front();
    });

    if (outStats != nullptr) {
        *outStats = stats;
    if (can_skip_island_graph_build(bodyCount, contacts, distanceConstraints)) {
    if (!can_build_island_graph(bodyCount, contacts, distanceConstraints)) {
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
        if (!contact.valid || !contact_bodies_in_range(contact, bodyCount)) {
            continue;
        }
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!contact_pair_in_range(contact, bodyCount)) {
        if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
        if (!contact.valid || !bodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!bodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!contact.valid || !isContactInRange(contact, bodyCount)) {
        if (!bodyIndexInRange(contact.bodyA) || !bodyIndexInRange(contact.bodyB)) {
        if (!isInRangeBodyPair(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!bodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
        if (!contactPairEligibleForBuild(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!island_build_body_pair_in_range(bodyCount, contact.bodyA, contact.bodyB)) {
        if (!contact_in_island_build_range(bodyCount, contact)) {
        if (!island_body_index_in_range(contact.bodyA, bodyCount) ||
            !island_body_index_in_range(contact.bodyB, bodyCount)) {
        if (!contactBodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
        if (!contact.valid || !contactBodiesInRange(contact, bodyCount)) {
        if (!contact.valid || !contact_refs_in_range(bodyCount, contact)) {
            continue;
        }
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!distance_pair_in_range(constraint, bodyCount)) {
        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
        if (!distance_bodies_in_range(constraint, bodyCount)) {
        if (!bodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!isDistanceConstraintInRange(constraint, bodyCount)) {
        if (!bodyIndexInRange(constraint.bodyA) || !bodyIndexInRange(constraint.bodyB)) {
        if (!isInRangeBodyPair(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!bodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (!distancePairEligibleForBuild(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!island_build_body_pair_in_range(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (!distance_in_island_build_range(bodyCount, constraint)) {
        if (!island_body_index_in_range(constraint.bodyA, bodyCount) ||
            !island_body_index_in_range(constraint.bodyB, bodyCount)) {
        if (!contactBodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (!distanceBodiesInRange(constraint, bodyCount)) {
        if (!distance_refs_in_range(bodyCount, constraint)) {
            continue;
        }
        unionBodies(constraint.bodyA, constraint.bodyB);
    }

    for (u32 i = 0; i < bodyCount; ++i) {
        compressPath(i);
    }

    std::vector<u32> rootToIsland(bodyCount, invalidIsland);
    islands_.clear();

    bodyToIsland_.assign(bodyCount, invalidIsland);
    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        const u32 root = findRoot(bodyIndex);
        if (rootToIsland[root] == invalidIsland) {
            rootToIsland[root] = static_cast<u32>(islands_.size());
            islands_.push_back({});
        }
        const u32 islandIndex = rootToIsland[root];
        islands_[islandIndex].bodyIndices.push_back(bodyIndex);
        bodyToIsland_[bodyIndex] = islandIndex;
    }

    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid || !contact_body_indices_in_range(contact, bodyCount)) {
            continue;
        }
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!contact_pair_in_range(contact, bodyCount)) {
        if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
        if (!contact.valid || !contact_bodies_in_range(contact, bodyCount)) {
        if (!contact.valid || !bodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!bodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!contact.valid || !isContactInRange(contact, bodyCount)) {
        if (!bodyIndexInRange(contact.bodyA) || !bodyIndexInRange(contact.bodyB)) {
        if (!isInRangeBodyPair(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!bodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
        if (!contactPairEligibleForBuild(contact.bodyA, contact.bodyB, bodyCount)) {
        if (!island_build_body_pair_in_range(bodyCount, contact.bodyA, contact.bodyB)) {
        if (!contact.valid || !contact_in_island_build_range(bodyCount, contact)) {
        if (!island_body_index_in_range(contact.bodyA, bodyCount) ||
            !island_body_index_in_range(contact.bodyB, bodyCount)) {
        if (!contactBodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
        if (!contact.valid || !contactBodiesInRange(contact, bodyCount)) {
        if (!contact.valid || !contact_refs_in_range(bodyCount, contact)) {
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
        if (!distance_body_indices_in_range(constraint, bodyCount)) {
        if (!distance_bodies_in_range(constraint, bodyCount)) {
        if (!bodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!isDistanceConstraintInRange(constraint, bodyCount)) {
        if (!bodyIndexInRange(constraint.bodyA) || !bodyIndexInRange(constraint.bodyB)) {
        if (!isInRangeBodyPair(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!bodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (!distancePairEligibleForBuild(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (!island_build_body_pair_in_range(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (!distance_in_island_build_range(bodyCount, constraint)) {
        if (!island_body_index_in_range(constraint.bodyA, bodyCount) ||
            !island_body_index_in_range(constraint.bodyB, bodyCount)) {
        if (!contactBodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (!distanceBodiesInRange(constraint, bodyCount)) {
        if (!distance_refs_in_range(bodyCount, constraint)) {
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
    if (shouldSkipIslandGraphBuild(bodyCount, contacts, distanceConstraints)) {
    if (should_skip_contact_island_build(bodyCount, contacts, distanceConstraints)) {
    if (should_skip_contact_island_graph_build(bodyCount, contacts, distanceConstraints)) {
bool ContactIslandGraph::buildGuarded(
    u32 bodyCount,
    if (should_skip_island_graph_build(bodyCount, contacts, distanceConstraints)) {
    if (islandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) !=
        IslandGraphBuildRejectReason::None) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
void ContactIslandGraph::build_guarded(u32 bodyCount,
        return;
    if (should_skip_island_build(bodyCount)) {
void ContactIslandGraph::build_skipping_unsafe_refs(
    std::vector<narrowphase::ContactManifold> filteredContacts;
    filteredContacts.reserve(contacts.size());
    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        if (!contact_refs_in_range(contact, bodyCount)) {
        filteredContacts.push_back(contact);

    std::vector<DistanceConstraint> filteredConstraints;
    filteredConstraints.reserve(distanceConstraints.size());
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_refs_in_range(constraint, bodyCount)) {
        filteredConstraints.push_back(constraint);

    build(bodyCount, filteredContacts, filteredConstraints);
IslandGraphBuildOutcome ContactIslandGraph::build_guarded(
    IslandGraphBuildOutcome outcome{};
    const IslandGraphBuildPreflight preflight =
        preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
    outcome.unsafeRefs = preflight.has_unsafe_refs();
        outcome.skipped = true;
        return outcome;

    outcome.built = true;
}

u32 ContactIslandGraph::constrainedIslandCount() const {
    return computePartitionStats().constrainedCount;
}

u32 ContactIslandGraph::emptyIslandCount() const {
    return computePartitionStats().emptyCount;
}

IslandGraphPartitionStats ContactIslandGraph::computePartitionStats() const {
    IslandGraphPartitionStats stats{};
    stats.totalIslands = islandCount();
    for (const Island& island : islands_) {
        if (island.isEmpty()) {
            ++stats.emptyCount;
        } else {
            ++stats.constrainedCount;
        }
    }
    return stats;
}

u32 ContactIslandGraph::bodyIsland(u32 bodyIndex) const {
    if (bodyIndex >= bodyToIsland_.size()) {
        return invalidIsland;
    }
    return bodyToIsland_[bodyIndex];
}

bool ContactIslandGraph::build_guarded(u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    if (!preflight.can_build()) {
bool ContactIslandGraph::buildGuarded(u32 bodyCount,
    if (should_skip_island_graph_build(bodyCount, contacts, distanceConstraints)) {
bool island_graph_build_inputs_valid(
    u32 bodyCount,
    if (bodyCount == 0u) {
        bool hasInRangeContact = false;
        for (const narrowphase::ContactManifold& contact : contacts) {
            if (contact.bodyA < bodyCount && contact.bodyB < bodyCount) {
                hasInRangeContact = true;
                break;
            }
        bool hasInRangeDistance = false;
        for (const DistanceConstraint& constraint : distanceConstraints) {
            if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
                hasInRangeDistance = true;
        if (!hasInRangeContact && !hasInRangeDistance) {
            return false;

        if (contact.valid && (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount)) {

        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {

    return true;

    if (!island_graph_build_inputs_valid(bodyCount, contacts, distanceConstraints)) {
        clear();
    build(bodyCount, contacts, distanceConstraints);

bool is_valid_island_build_body_count(u32 bodyCount) {
    return bodyCount > 0u;

bool contact_references_valid_bodies(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return contact.bodyA < bodyCount && contact.bodyB < bodyCount;

bool distance_constraint_references_valid_bodies(const DistanceConstraint& constraint, u32 bodyCount) {
    return constraint.bodyA < bodyCount && constraint.bodyB < bodyCount;

IslandBuildPreflight preflight_island_build(
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactCount = static_cast<u32>(contacts.size());
    preflight.distanceConstraintCount = static_cast<u32>(distanceConstraints.size());
    preflight.zeroBodies = !is_valid_island_build_body_count(bodyCount);
    preflight.skipped = preflight.zeroBodies;

        if (!contact.valid || !contact_references_valid_bodies(contact, bodyCount)) {
            ++preflight.invalidContactCount;
        } else {
            ++preflight.validContactCount;
IslandGraphBuildPreflight preflight_island_graph_build(
    IslandGraphBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

        if (contact.valid) {
            ++preflight.stats.validContactCount;
        if (contact.bodyA == contact.bodyB) {
                ++preflight.stats.selfPairContactCount;
            continue;
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;

        if (!distance_constraint_references_valid_bodies(constraint, bodyCount)) {
            ++preflight.invalidDistanceCount;
            ++preflight.validDistanceCount;

    return preflight;

bool should_skip_island_build(u32 bodyCount) {
    return !is_valid_island_build_body_count(bodyCount);
        if (constraint.bodyA == constraint.bodyB) {
            ++preflight.stats.selfPairDistanceCount;
            ++preflight.stats.inRangeDistanceCount;
            ++preflight.stats.outOfRangeDistanceBodyCount;

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;

bool should_skip_island_graph_build(u32 bodyCount,
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();

bool build_island_graph_guarded(ContactIslandGraph& graph,
    return graph.build_guarded(bodyCount, contacts, distanceConstraints);
    return graph.buildGuarded(bodyCount, contacts, distanceConstraints);

IslandGraphIntegrityPreflight preflight_island_graph_integrity(const ContactIslandGraph& graph,
                                                                u32 contactSlotCount,
                                                                u32 distanceSlotCount) {
    IslandGraphIntegrityPreflight preflight{};
    preflight.stats.islandCount = graph.islandCount();
    preflight.stats.constrainedIslandCount = graph.constrainedIslandCount();
    if (preflight.stats.islandCount == 0u) {
        preflight.skipped = true;
IslandGraphIntegrityPreflight preflight_island_graph_integrity(
    const ContactIslandGraph& graph,
    if (bodyCount == 0u && graph.islandCount() == 0u) {

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        for (u32 bodyIndex : island.bodyIndices) {
            if (bodyIndex >= bodyCount) {
                ++preflight.stats.outOfRangeBodyIndexCount;
        for (u32 contactIndex : island.contactIndices) {
            if (contactIndex >= contactSlotCount) {
                ++preflight.stats.outOfRangeContactRefCount;
        for (u32 distanceIndex : island.distanceIndices) {
            if (distanceIndex >= distanceSlotCount) {
                ++preflight.stats.outOfRangeDistanceRefCount;

bool should_skip_island_graph_integrity(const ContactIslandGraph& graph,
    return !preflight_island_graph_integrity(graph, bodyCount, contactSlotCount, distanceSlotCount).can_use();
            if (contactIndex >= contacts.size()) {
                ++preflight.stats.orphanedContactRefCount;
            if (distanceIndex >= distanceConstraints.size()) {
                ++preflight.stats.orphanedDistanceRefCount;


    return !preflight_island_graph_integrity(graph, bodyCount, contacts, distanceConstraints).is_consistent();
bool bodies_in_union_range(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return islandUnionRejectReason(bodyCount, bodyA, bodyB) == IslandUnionRejectReason::None;

bool contact_in_island_build_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact.valid && contact.bodyA < bodyCount && contact.bodyB < bodyCount;

bool distance_in_island_build_range(u32 bodyCount, const DistanceConstraint& constraint) {

const char* islandUnionRejectReasonName(IslandUnionRejectReason reason) {
    switch (reason) {
    case IslandUnionRejectReason::None:
        return "None";
    case IslandUnionRejectReason::OutOfRangeBodyA:
        return "OutOfRangeBodyA";
    case IslandUnionRejectReason::OutOfRangeBodyB:
        return "OutOfRangeBodyB";
    default:
        return "Unknown";

IslandUnionRejectReason islandUnionRejectReason(u32 bodyCount, u32 bodyA, u32 bodyB) {
    if (bodyA >= bodyCount) {
        return IslandUnionRejectReason::OutOfRangeBodyA;
    if (bodyB >= bodyCount) {
        return IslandUnionRejectReason::OutOfRangeBodyB;
    return IslandUnionRejectReason::None;

bool islandUnionRejectsForReason(u32 bodyCount, u32 bodyA, u32 bodyB, IslandUnionRejectReason expected) {
    return islandUnionRejectReason(bodyCount, bodyA, bodyB) == expected;
}

} // namespace fuse::physics
