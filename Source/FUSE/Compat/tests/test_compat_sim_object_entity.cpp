#include <fuse/compat/object_label.hpp>
#include <fuse/compat/sim_object_entity.hpp>

#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

fuse::ecs::EntityID ent(fuse::u32 index, fuse::u32 generation) {
    return fuse::ecs::EntityID{index, generation};
}

bool simIs(const std::optional<fuse::u32>& id, fuse::u32 expected) {
    return id.has_value() && *id == expected;
}

void testObjectLabelOk() {
    expectTrue(fuse::compat::object_label_ok("Player"), "Player is an object label");
    expectTrue(!fuse::compat::object_label_ok(""), "empty label is rejected");
    expectTrue(!fuse::compat::object_label_ok("1Player"), "label starting with a digit is rejected");
    expectTrue(fuse::compat::object_label_ok("a"), "single ASCII letter is an object label");
    expectTrue(fuse::compat::object_label_ok("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab"),
               "64-character label starting with a letter is accepted");
    expectTrue(!fuse::compat::object_label_ok("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abc"),
               "65-character label is rejected");
}

void testUnbound() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID missing = table.find(1u);
    expectTrue(missing == fuse::ecs::EntityID::null(), "find unbound is EntityID::null()");
    expectTrue(!missing.valid(), "null entity is not valid");
    expectTrue(missing.generation == 0u, "null id is generation 0");
    expectTrue(table.entityFor(1u) == fuse::ecs::EntityID::null(), "entityFor unbound is null");
    expectTrue(!table.simObjectFor(ent(1u, 1u)).has_value(), "simObjectFor unbound is empty");
    expectTrue(!table.unbind(1u), "unbind missing sim object");
    expectTrue(!table.unbindEntity(ent(1u, 1u)), "unbindEntity missing entity");
    expectTrue(!table.unbindEntity(fuse::ecs::EntityID::null()), "unbindEntity null is a no-op");
}

void testIndexZeroIsLive() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID zeroIndex = ent(0u, 1u);
    table.bind(5u, zeroIndex);
    expectTrue(table.find(5u) == zeroIndex, "index 0 generation 1 is a live entity");
    expectTrue(table.find(5u) != fuse::ecs::EntityID::null(), "live index 0 is not null");
    expectTrue(table.find(5u).valid(), "live index 0 is valid");
    expectTrue(simIs(table.simObjectFor(zeroIndex), 5u), "index 0 entity maps back");
}

void testBindRoundTrip() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(3u, 1u);
    const fuse::ecs::EntityID b = ent(4u, 1u);
    table.bind(10u, a);
    table.bind(11u, b);
    expectTrue(table.find(10u) == a, "find returns bound entity");
    expectTrue(table.entityFor(10u) == a, "entityFor matches find");
    const std::optional<fuse::u32> simA = table.simObjectFor(a);
    expectTrue(simIs(simA, 10u), "simObjectFor returns sim object id");
    if (simA.has_value()) {
        expectTrue(table.entityFor(*simA) == a, "entityFor is inverse of simObjectFor");
    }
    expectTrue(simIs(table.simObjectFor(table.entityFor(10u)), 10u), "simObjectFor is inverse of entityFor");
    expectTrue(table.find(11u) == b, "second binding stays");
    expectTrue(simIs(table.simObjectFor(b), 11u), "second entity keeps its own sim object");
}

void testReplaceSimObject() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(1u, 1u);
    const fuse::ecs::EntityID b = ent(2u, 1u);
    table.bind(5u, a);
    table.bind(5u, b);
    expectTrue(table.find(5u) == b, "bind replaces entity for sim object id");
    expectTrue(!table.simObjectFor(a).has_value(), "previous entity is unbound");
    expectTrue(simIs(table.simObjectFor(b), 5u), "new entity maps back to sim object");
}

void testRebindMovesId() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(4u, 2u);
    table.bind(1u, a);
    table.bind(2u, a);
    expectTrue(table.find(1u) == fuse::ecs::EntityID::null(), "old sim object id is unbound");
    expectTrue(table.find(2u) == a, "entity moved to the new sim object id");
    expectTrue(simIs(table.simObjectFor(a), 2u), "entity maps to only the new id");
}

void testCrossRebind() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(1u, 1u);
    const fuse::ecs::EntityID b = ent(2u, 1u);
    table.bind(10u, a);
    table.bind(20u, b);
    table.bind(10u, b);
    expectTrue(table.find(10u) == b, "sim object takes the new entity");
    expectTrue(table.find(20u) == fuse::ecs::EntityID::null(), "previous sim object of that entity is cleared");
    expectTrue(!table.simObjectFor(a).has_value(), "displaced entity is unbound");
    expectTrue(simIs(table.simObjectFor(b), 10u), "entity keeps a single sim object");
}

void testDistinctGenerations() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(1u, 1u);
    const fuse::ecs::EntityID recycled = ent(1u, 2u);
    table.bind(7u, a);
    table.bind(8u, recycled);
    expectTrue(table.find(7u) == a, "generation 1 stays bound");
    expectTrue(table.find(8u) == recycled, "generation 2 is a different entity");
    expectTrue(simIs(table.simObjectFor(a), 7u), "same index different generation is not the same key");
    expectTrue(simIs(table.simObjectFor(recycled), 8u), "recycled entity has its own sim object");
}

void testUnbind() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(9u, 1u);
    const fuse::ecs::EntityID b = ent(8u, 1u);
    table.bind(3u, a);
    table.bind(4u, b);
    expectTrue(table.unbind(3u), "unbind removes sim object");
    expectTrue(table.find(3u) == fuse::ecs::EntityID::null(), "find after unbind is null");
    expectTrue(!table.simObjectFor(a).has_value(), "entity side cleared by unbind");
    expectTrue(table.find(4u) == b, "unbind leaves the other row");

    expectTrue(table.unbindEntity(b), "unbindEntity removes entity");
    expectTrue(table.find(4u) == fuse::ecs::EntityID::null(), "sim object side cleared by unbindEntity");
    expectTrue(!table.simObjectFor(b).has_value(), "simObjectFor after unbindEntity is empty");
    expectTrue(!table.unbind(3u), "second unbind is a no-op");
}

void testClearAndNullBind() {
    fuse::compat::SimObjectEntityTable table;
    table.clear();
    expectTrue(table.find(1u) == fuse::ecs::EntityID::null(), "clear on an empty table");

    const fuse::ecs::EntityID a = ent(1u, 1u);
    const fuse::ecs::EntityID b = ent(2u, 4u);
    table.bind(1u, a);
    table.bind(0u, b);
    table.bind(1u, fuse::ecs::EntityID::null());
    expectTrue(table.find(1u) == fuse::ecs::EntityID::null(), "binding null drops the sim object");
    expectTrue(!table.simObjectFor(a).has_value(), "null bind unbinds the previous entity");
    expectTrue(table.find(0u) == b, "sim object id 0 is a real id");
    expectTrue(simIs(table.simObjectFor(b), 0u), "id 0 round-trips");

    table.bind(2u, fuse::ecs::EntityID{5u, 0u});
    expectTrue(table.find(2u) == fuse::ecs::EntityID::null(), "generation 0 is not stored");
    expectTrue(!table.simObjectFor(fuse::ecs::EntityID{5u, 0u}).has_value(), "invalid entity has no sim object");

    table.clear();
    expectTrue(table.find(0u) == fuse::ecs::EntityID::null(), "clear drops every sim object");
    expectTrue(!table.simObjectFor(b).has_value(), "clear drops every entity");
}

void testIdempotentBindAndCopy() {
    fuse::compat::SimObjectEntityTable table;
    const fuse::ecs::EntityID a = ent(6u, 3u);
    table.bind(4u, a);
    table.bind(4u, a);
    expectTrue(table.find(4u) == a, "rebinding the same pair keeps the entity");
    expectTrue(simIs(table.simObjectFor(a), 4u), "rebinding the same pair keeps the sim object");

    fuse::compat::SimObjectEntityTable copy = table;
    expectTrue(copy.unbind(4u), "copy unbind drops the copied row");
    expectTrue(table.find(4u) == a, "original table keeps its row");
    expectTrue(copy.find(4u) == fuse::ecs::EntityID::null(), "copy no longer has the row");
}

void testRebindKeepsBijection() {
    fuse::compat::SimObjectEntityTable table;
    for (fuse::u32 i = 0; i < 32u; ++i) {
        table.bind(1000u + i, ent(i + 1u, 1u));
    }
    for (fuse::u32 i = 0; i < 32u; ++i) {
        const fuse::ecs::EntityID id = ent(i + 1u, 1u);
        expectTrue(table.find(1000u + i) == id, "bulk find");
        expectTrue(table.entityFor(1000u + i) == id, "bulk entityFor");
        const std::optional<fuse::u32> sim = table.simObjectFor(id);
        expectTrue(simIs(sim, 1000u + i), "bulk simObjectFor");
        if (sim.has_value()) {
            expectTrue(table.entityFor(*sim) == id, "bulk inverse");
        }
    }
    for (fuse::u32 i = 0; i < 32u; i += 2u) {
        table.bind(2000u + i, ent(i + 1u, 1u));
    }
    for (fuse::u32 i = 0; i < 32u; ++i) {
        const fuse::ecs::EntityID id = ent(i + 1u, 1u);
        if ((i % 2u) == 0u) {
            expectTrue(table.find(1000u + i) == fuse::ecs::EntityID::null(), "moved id left the old sim object");
            expectTrue(table.find(2000u + i) == id, "moved id arrived");
            expectTrue(simIs(table.simObjectFor(id), 2000u + i), "moved entity has one sim object");
        } else {
            expectTrue(table.find(1000u + i) == id, "unmoved entity stays");
            expectTrue(simIs(table.simObjectFor(id), 1000u + i), "unmoved sim object stays");
        }
    }
}

} // namespace

int main() {
    testObjectLabelOk();
    testUnbound();
    testIndexZeroIsLive();
    testBindRoundTrip();
    testReplaceSimObject();
    testRebindMovesId();
    testCrossRebind();
    testDistinctGenerations();
    testUnbind();
    testClearAndNullBind();
    testIdempotentBindAndCopy();
    testRebindKeepsBijection();

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_compat_sim_object: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_compat_sim_object: all tests passed\n");
    return EXIT_SUCCESS;
}
