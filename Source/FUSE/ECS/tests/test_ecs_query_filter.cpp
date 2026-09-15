#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/query_filter.hpp>
#include <fuse/ecs/registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <typeindex>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

fuse::ecs::Archetype makeArchetypeWithComponents(const std::vector<std::type_index>& types) {
    fuse::ecs::Archetype archetype;
    archetype.component_types = types;
    for (const std::type_index& type : types) {
        if (type == std::type_index(typeid(fuse::ecs::Transform))) {
            archetype.ensure_column(type, sizeof(fuse::ecs::Transform));
        } else if (type == std::type_index(typeid(fuse::ecs::RigidBody))) {
            archetype.ensure_column(type, sizeof(fuse::ecs::RigidBody));
        } else if (type == std::type_index(typeid(fuse::ecs::TagStatic))) {
            archetype.ensure_column(type, sizeof(fuse::ecs::TagStatic));
        }
    }
    return archetype;
}

void testArchetypeMatchesWithRequiredComponents() {
    const fuse::ecs::Archetype archetype = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });

    const fuse::ecs::QueryFilter exact =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{});
    expectTrue(fuse::ecs::archetype_matches(archetype, exact), "exact With match accepts archetype");

    const fuse::ecs::QueryFilter subset = fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    expectTrue(fuse::ecs::archetype_matches(archetype, subset), "subset With match accepts superset archetype");

    const fuse::ecs::QueryFilter missing =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::TagStatic>{});
    expectTrue(!fuse::ecs::archetype_matches(archetype, missing), "missing With component rejects archetype");
}

void testArchetypeMatchesWithoutExcludedComponents() {
    const fuse::ecs::Archetype archetype = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });

    const fuse::ecs::QueryFilter allowed = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectTrue(!fuse::ecs::archetype_matches(archetype, allowed),
               "Without component rejects matching archetype");

    const fuse::ecs::QueryFilter dynamicOnly = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectTrue(!fuse::ecs::archetype_matches(archetype, dynamicOnly),
               "Without rejects even when With subset is present");

    const fuse::ecs::QueryFilter noExclusions = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody, fuse::ecs::TagStatic>{});
    expectTrue(fuse::ecs::archetype_matches(archetype, noExclusions),
               "Without-less filter accepts tagged archetype");
}

void testArchetypeMatchesEmptyArchetype() {
    const fuse::ecs::Archetype empty;

    const fuse::ecs::QueryFilter noRequirements = fuse::ecs::make_query_filter();
    expectTrue(fuse::ecs::archetype_matches(empty, noRequirements), "empty filter matches empty archetype");

    const fuse::ecs::QueryFilter requiresTransform =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::archetype_matches(empty, requiresTransform),
               "empty archetype fails non-empty With filter");
}

void testEachQueryWithWithoutFilters() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID transformOnly = reg.create();
    const fuse::ecs::EntityID dynamicBody = reg.create();
    const fuse::ecs::EntityID staticBody = reg.create();

    reg.add<fuse::ecs::Transform>(transformOnly);
    reg.add<fuse::ecs::Transform>(dynamicBody);
    reg.add<fuse::ecs::RigidBody>(dynamicBody);
    reg.add<fuse::ecs::Transform>(staticBody);
    reg.add<fuse::ecs::RigidBody>(staticBody);
    reg.add<fuse::ecs::TagStatic>(staticBody);

    fuse::u32 seen = 0;
    reg.each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            expectTrue(id == dynamicBody, "each_query visits only non-static rigid bodies");
            ++seen;
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectEq(seen, 1u, "each_query With/Without visits one entity");
}

void testEachQueryParallelMatchesSerial() {
    fuse::ecs::Registry reg;
    reg.init(64);

    fuse::ecs::EntityID ids[4];
    for (int i = 0; i < 4; ++i) {
        ids[i] = reg.create();
        reg.add<fuse::ecs::Transform>(ids[i]);
        reg.add<fuse::ecs::RigidBody>(ids[i]);
        if (i % 2 == 0) {
            reg.add<fuse::ecs::TagStatic>(ids[i]);
        }
    }

    std::vector<fuse::u32> serialIndices;
    reg.each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            serialIndices.push_back(id.index);
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    std::vector<fuse::u32> parallelIndices;
    reg.each_query_parallel<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            parallelIndices.push_back(id.index);
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{},
        1);

    expectEq(static_cast<fuse::u32>(serialIndices.size()), 2u, "serial query finds two dynamic bodies");
    expectEq(static_cast<fuse::u32>(parallelIndices.size()), 2u, "parallel query finds two dynamic bodies");

    std::sort(serialIndices.begin(), serialIndices.end());
    std::sort(parallelIndices.begin(), parallelIndices.end());
    expectTrue(serialIndices == parallelIndices, "each_query_parallel matches serial coverage");
}

} // namespace

int main() {
    testArchetypeMatchesWithRequiredComponents();
    testArchetypeMatchesWithoutExcludedComponents();
    testArchetypeMatchesEmptyArchetype();
    testEachQueryWithWithoutFilters();
    testEachQueryParallelMatchesSerial();

    if (g_failures == 0) {
        std::printf("fuse_ecs_query_filter_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_query_filter_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
