#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/detail/iteration_parity.hpp>
#include <fuse/ecs/query_filter.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
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
        } else if (type == std::type_index(typeid(fuse::ecs::TagPlayer))) {
            archetype.ensure_column(type, sizeof(fuse::ecs::TagPlayer));
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

void testCompileTimeArchetypeMatches() {
    const fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    const fuse::ecs::Archetype staticBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });

    expectTrue(fuse::ecs::archetype_matches(dynamicBody, fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{}),
               "compile-time With accepts matching archetype");
    expectTrue(!fuse::ecs::archetype_matches(staticBody,
                                             fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
                                             fuse::ecs::Without<fuse::ecs::TagStatic>{}),
               "compile-time Without rejects tagged archetype");
}

void testArchetypeMatchesMultipleWithout() {
    const fuse::ecs::Archetype tagged = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });

    expectTrue(fuse::ecs::archetype_matches(tagged, fuse::ecs::With<fuse::ecs::Transform>{}),
               "With-only filter accepts tagged archetype");
    expectTrue(!fuse::ecs::archetype_matches(tagged,
                                             fuse::ecs::With<fuse::ecs::Transform>{},
                                             fuse::ecs::Without<fuse::ecs::TagStatic>{}),
               "single Without rejects tagged archetype");
    expectTrue(!fuse::ecs::archetype_matches(tagged,
                                             fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
                                             fuse::ecs::Without<fuse::ecs::TagStatic>{}),
               "missing With rejects archetype even when Without would pass");
}

void testQueryFilterEmptyAndConflict() {
    const fuse::ecs::QueryFilter empty = fuse::ecs::make_query_filter();
    expectTrue(fuse::ecs::query_filter_empty(empty), "default make_query_filter is empty");

    const fuse::ecs::QueryFilter withOnly =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::query_filter_empty(withOnly), "With-only filter is not empty");
    expectTrue(!fuse::ecs::query_filter_has_conflict(withOnly), "With-only filter has no conflict");

    const fuse::ecs::QueryFilter withoutOnly =
        fuse::ecs::make_query_filter(fuse::ecs::With<>{}, fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectTrue(!fuse::ecs::query_filter_empty(withoutOnly), "Without-only filter is not empty");
    expectTrue(!fuse::ecs::query_filter_has_conflict(withoutOnly), "Without-only filter has no conflict");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(fuse::ecs::query_filter_has_conflict(conflicting),
               "same type in With and Without is a conflict");
}

void testArchetypeMatchesConflictingFilter() {
    const fuse::ecs::Archetype archetype = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::archetype_matches(archetype, conflicting),
               "conflicting filter rejects every archetype");
}

void testArchetypeMatchesWithoutOnly() {
    const fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    const fuse::ecs::Archetype staticBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });

    expectTrue(fuse::ecs::archetype_matches(dynamicBody, fuse::ecs::Without<fuse::ecs::TagStatic>{}),
               "Without-only accepts archetype missing excluded type");
    expectTrue(!fuse::ecs::archetype_matches(staticBody, fuse::ecs::Without<fuse::ecs::TagStatic>{}),
               "Without-only rejects archetype carrying excluded type");
}

void testArchetypeMatchesZeroEntityArchetype() {
    const fuse::ecs::Archetype emptyRows = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    expectEq(static_cast<fuse::u32>(emptyRows.count()), 0u, "synthetic archetype has zero entities");

    const fuse::ecs::QueryFilter filter =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{});
    expectTrue(fuse::ecs::archetype_matches(emptyRows, filter),
               "zero-entity archetype still matches signature filter");

    const std::vector<fuse::ecs::Archetype> table = {emptyRows};
    expectEq(fuse::ecs::count_matching_archetypes(table, filter), 1u,
             "count_matching_archetypes includes zero-entity signatures");
}

void testEachQueryEmptyRegistry() {
    fuse::ecs::Registry reg;
    reg.init(8);

    fuse::u32 seen = 0;
    reg.each_query<fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::Transform&) { ++seen; });
    expectEq(seen, 0u, "each_query on empty registry visits zero entities");

    reg.each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID, fuse::ecs::Transform&, fuse::ecs::RigidBody&) { ++seen; },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectEq(seen, 0u, "each_query With/Without on empty registry visits zero entities");

    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(2);
    std::atomic<fuse::u32> parallelSeen{0};
    reg.each_query_parallel<fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::Transform&) {
            parallelSeen.fetch_add(1u, std::memory_order_relaxed);
        },
        1);
    fuse::jobs::JobScheduler::instance().shutdown();
    expectEq(parallelSeen.load(std::memory_order_relaxed), 0u,
             "each_query_parallel on empty registry visits zero entities");
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

void testEachQueryEmptyMatch() {
    fuse::ecs::Registry reg;
    reg.init(64);

    for (int i = 0; i < 3; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        reg.add<fuse::ecs::Transform>(id);
        reg.add<fuse::ecs::RigidBody>(id);
        reg.add<fuse::ecs::TagStatic>(id);
    }

    fuse::u32 seen = 0;
    reg.each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID, fuse::ecs::Transform&, fuse::ecs::RigidBody&) { ++seen; },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectEq(seen, 0u, "each_query With/Without visits zero entities when all are excluded");
}

void testEachWithWithoutMatchesEachQuery() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID dynamicBody = reg.create();
    const fuse::ecs::EntityID staticBody = reg.create();

    reg.add<fuse::ecs::Transform>(dynamicBody);
    reg.add<fuse::ecs::RigidBody>(dynamicBody);
    reg.add<fuse::ecs::Transform>(staticBody);
    reg.add<fuse::ecs::RigidBody>(staticBody);
    reg.add<fuse::ecs::TagStatic>(staticBody);

    fuse::u32 eachCount = 0;
    reg.each<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            expectTrue(id == dynamicBody, "each With/Without skips static bodies");
            ++eachCount;
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    fuse::u32 queryCount = 0;
    reg.each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            expectTrue(id == dynamicBody, "each_query With/Without skips static bodies");
            ++queryCount;
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    expectEq(eachCount, 1u, "each With/Without visits one entity");
    expectEq(queryCount, 1u, "each_query With/Without visits one entity");
}

void testEachParallelWithWithoutMatchesEachQueryParallel() {
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

    std::atomic<fuse::u32> eachParallelCount{0};
    std::vector<std::atomic<bool>> eachSeen(4);
    for (auto& slot : eachSeen) {
        slot.store(false, std::memory_order_relaxed);
    }

    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(4);
    reg.each_parallel<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            if (id.index < eachSeen.size()) {
                eachSeen[id.index].store(true, std::memory_order_relaxed);
            }
            eachParallelCount.fetch_add(1u, std::memory_order_relaxed);
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{},
        1);

    std::atomic<fuse::u32> queryParallelCount{0};
    std::vector<std::atomic<bool>> querySeen(4);
    for (auto& slot : querySeen) {
        slot.store(false, std::memory_order_relaxed);
    }
    reg.each_query_parallel<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            if (id.index < querySeen.size()) {
                querySeen[id.index].store(true, std::memory_order_relaxed);
            }
            queryParallelCount.fetch_add(1u, std::memory_order_relaxed);
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{},
        1);
    fuse::jobs::JobScheduler::instance().shutdown();

    expectEq(eachParallelCount.load(std::memory_order_relaxed), 2u, "each_parallel With/Without finds two bodies");
    expectEq(queryParallelCount.load(std::memory_order_relaxed), 2u, "each_query_parallel With/Without finds two bodies");

    std::vector<fuse::u32> eachParallelIndices;
    std::vector<fuse::u32> queryParallelIndices;
    for (fuse::u32 i = 0; i < eachSeen.size(); ++i) {
        if (eachSeen[i].load(std::memory_order_relaxed)) {
            eachParallelIndices.push_back(i);
        }
        if (querySeen[i].load(std::memory_order_relaxed)) {
            queryParallelIndices.push_back(i);
        }
    }
    std::sort(eachParallelIndices.begin(), eachParallelIndices.end());
    std::sort(queryParallelIndices.begin(), queryParallelIndices.end());
    expectTrue(eachParallelIndices == queryParallelIndices,
               "each_parallel With/Without matches each_query_parallel coverage");
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

    std::atomic<fuse::u32> parallelCount{0};
    std::vector<std::atomic<bool>> seen(4);
    for (auto& slot : seen) {
        slot.store(false, std::memory_order_relaxed);
    }

    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(4);
    reg.each_query_parallel<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            if (id.index < seen.size()) {
                seen[id.index].store(true, std::memory_order_relaxed);
            }
            parallelCount.fetch_add(1u, std::memory_order_relaxed);
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{},
        1);
    fuse::jobs::JobScheduler::instance().shutdown();

    expectEq(static_cast<fuse::u32>(serialIndices.size()), 2u, "serial query finds two dynamic bodies");
    expectEq(parallelCount.load(std::memory_order_relaxed), 2u, "parallel query finds two dynamic bodies");

    std::sort(serialIndices.begin(), serialIndices.end());

    std::vector<fuse::u32> parallelIndices;
    for (fuse::u32 i = 0; i < seen.size(); ++i) {
        if (seen[i].load(std::memory_order_relaxed)) {
            parallelIndices.push_back(i);
        }
    }
    std::sort(parallelIndices.begin(), parallelIndices.end());
    expectTrue(serialIndices == parallelIndices, "each_query_parallel matches serial coverage");
}

void testQueryFilterEqual() {
    const fuse::ecs::QueryFilter lhs =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
                                     fuse::ecs::Without<fuse::ecs::TagStatic>{});
    const fuse::ecs::QueryFilter rhs =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::RigidBody, fuse::ecs::Transform>{},
                                     fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectTrue(fuse::ecs::query_filter_equal(lhs, rhs), "query_filter_equal ignores With/Without order");

    const fuse::ecs::QueryFilter differentWithout =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
                                     fuse::ecs::Without<fuse::ecs::TagPlayer>{});
    expectTrue(!fuse::ecs::query_filter_equal(lhs, differentWithout),
               "query_filter_equal rejects different Without sets");

    const fuse::ecs::QueryFilter withOnly =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::query_filter_equal(lhs, withOnly),
               "query_filter_equal rejects missing Without constraints");
}

void testCountMatchingEntities() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});
    dynamicBody.append_entity(fuse::ecs::EntityID{1, 1});

    fuse::ecs::Archetype staticBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });
    staticBody.append_entity(fuse::ecs::EntityID{2, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody, staticBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    expectEq(fuse::ecs::count_matching_archetypes(table, filter), 1u,
             "count_matching_archetypes counts one matching signature");
    expectEq(fuse::ecs::count_matching_entities(table, filter), 2u,
             "count_matching_entities sums rows in matching archetypes only");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectEq(fuse::ecs::count_matching_entities(table, conflicting), 0u,
             "count_matching_entities returns zero for conflicting filters");
}

void testEmptyFilterMatchesAllArchetypes() {
    const fuse::ecs::Archetype empty;
    const fuse::ecs::Archetype typed = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });

    const fuse::ecs::QueryFilter emptyFilter = fuse::ecs::make_query_filter();
    const std::vector<fuse::ecs::Archetype> table = {empty, typed};

    expectTrue(fuse::ecs::query_filter_empty(emptyFilter), "empty filter has no constraints");
    expectEq(fuse::ecs::count_matching_archetypes(table, emptyFilter), 2u,
             "empty filter matches every archetype signature");
    expectTrue(fuse::ecs::archetype_matches(empty, emptyFilter), "empty filter matches empty archetype");
    expectTrue(fuse::ecs::archetype_matches(typed, emptyFilter), "empty filter matches typed archetype");
}

void testArchetypeMatchesMultipleWithoutTypes() {
    const fuse::ecs::Archetype playerTagged = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::TagPlayer)),
    });
    const fuse::ecs::Archetype staticTagged = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });
    const fuse::ecs::Archetype clean = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });

    const fuse::ecs::QueryFilter excludeTags = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{},
        fuse::ecs::Without<fuse::ecs::TagStatic, fuse::ecs::TagPlayer>{});

    expectTrue(fuse::ecs::archetype_matches(clean, excludeTags),
               "multiple Without accepts archetype with none of the excluded tags");
    expectTrue(!fuse::ecs::archetype_matches(staticTagged, excludeTags),
               "multiple Without rejects static tag");
    expectTrue(!fuse::ecs::archetype_matches(playerTagged, excludeTags),
               "multiple Without rejects player tag");
}

void testCountMatchingEntitiesMatchesEachQuery() {
    fuse::ecs::Registry reg;
    reg.init(64);

    for (int i = 0; i < 3; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        reg.add<fuse::ecs::Transform>(id);
        reg.add<fuse::ecs::RigidBody>(id);
        if (i == 0) {
            reg.add<fuse::ecs::TagStatic>(id);
        }
    }

    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    const fuse::u32 queryCount = fuse::ecs::detail::count_each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        reg, fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectEq(queryCount, 2u, "count_each_query With/Without tallies dynamic bodies");

    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(4);
    expectTrue(fuse::ecs::detail::each_query_with_without_parallel_matches_serial<fuse::ecs::Transform,
                                                                                   fuse::ecs::RigidBody>(
                   reg, fuse::ecs::Without<fuse::ecs::TagStatic>{}, 1),
               "each_query With/Without parity helper matches serial and parallel counts");
    fuse::jobs::JobScheduler::instance().shutdown();

    expectEq(queryCount, 2u, "registry each_query count aligns with filter expectation");
    expectTrue(fuse::ecs::query_filter_equal(
                   filter,
                   fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
                                                fuse::ecs::Without<fuse::ecs::TagStatic>{})),
               "runtime filter matches compile-time make_query_filter");
}

void testEachQueryEmptyRegistryWithoutOnlyArchetype() {
    fuse::ecs::Registry reg;
    reg.init(8);

    expectEq(reg.archetype_count(), 1u, "empty registry retains empty archetype table entry");

    const fuse::ecs::QueryFilter withoutStatic =
        fuse::ecs::make_query_filter(fuse::ecs::With<>{}, fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectTrue(!fuse::ecs::query_filter_empty(withoutStatic), "Without-only filter is not empty");
    expectEq(fuse::ecs::detail::count_each_query<fuse::ecs::Transform>(reg, fuse::ecs::Without<fuse::ecs::TagStatic>{}),
             0u,
             "Without-only query on empty registry visits zero entities");
}

void testQueryFilterIsRunnable() {
    const fuse::ecs::QueryFilter withOnly =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    expectTrue(fuse::ecs::query_filter_is_runnable(withOnly), "With-only filter is runnable");

    const fuse::ecs::QueryFilter withoutOnly =
        fuse::ecs::make_query_filter(fuse::ecs::With<>{}, fuse::ecs::Without<fuse::ecs::TagStatic>{});
    expectTrue(fuse::ecs::query_filter_is_runnable(withoutOnly), "Without-only filter is runnable");

    const fuse::ecs::QueryFilter empty = fuse::ecs::make_query_filter();
    expectTrue(fuse::ecs::query_filter_is_runnable(empty), "empty filter is runnable");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::query_filter_is_runnable(conflicting),
               "conflicting filter is not runnable");
    expectTrue(fuse::ecs::query_filter_has_conflict(conflicting),
               "conflicting filter reports has_conflict");
}

void testHasMatchingArchetypesEmptyTableAndConflict() {
    const fuse::ecs::Archetype typed = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    const std::vector<fuse::ecs::Archetype> table = {typed};
    const fuse::ecs::QueryFilter filter =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});

    expectTrue(fuse::ecs::has_matching_archetypes(table, filter),
               "has_matching_archetypes accepts matching signature");
    expectTrue(!fuse::ecs::has_matching_archetypes({}, filter),
               "has_matching_archetypes rejects empty archetype table");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::has_matching_archetypes(table, conflicting),
               "has_matching_archetypes rejects conflicting filter even with matching archetype");
}

void testCountMatchingArchetypesConflictingFilter() {
    const fuse::ecs::Archetype typed = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    const std::vector<fuse::ecs::Archetype> table = {typed};

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectEq(fuse::ecs::count_matching_archetypes(table, conflicting), 0u,
             "count_matching_archetypes returns zero for conflicting filters");
}

void testCountMatchingEntitiesEmptyTable() {
    const fuse::ecs::QueryFilter filter =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});

    expectEq(fuse::ecs::count_matching_archetypes({}, filter), 0u,
             "count_matching_archetypes returns zero for empty archetype table");
    expectEq(fuse::ecs::count_matching_entities({}, filter), 0u,
             "count_matching_entities returns zero for empty archetype table");
    expectTrue(!fuse::ecs::has_matching_archetypes({}, filter),
               "has_matching_archetypes returns false for empty archetype table");
}

void testHasMatchingEntitiesDistinguishesZeroRowArchetypes() {
    fuse::ecs::Archetype zeroRow = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    fuse::ecs::Archetype populated = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    populated.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {zeroRow, populated};
    const fuse::ecs::QueryFilter bodyFilter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{});
    const fuse::ecs::QueryFilter transformFilter =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});

    expectTrue(fuse::ecs::has_matching_archetypes(table, bodyFilter),
               "zero-row archetype still counts as matching signature");
    expectTrue(!fuse::ecs::has_matching_entities(table, bodyFilter),
               "zero-row matching archetype yields no matching entities");
    expectTrue(fuse::ecs::has_matching_entities(table, transformFilter),
               "populated matching archetype yields matching entities");
}

void testPreflightQueryFilterBundlesGuardsAndCounts() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});
    dynamicBody.append_entity(fuse::ecs::EntityID{1, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    const fuse::ecs::QueryFilterPreflight preflight = fuse::ecs::preflight_query_filter(table, filter);
    expectTrue(preflight.runnable, "preflight marks runnable filter");
    expectTrue(!preflight.has_conflict, "preflight clears conflict flag for runnable filter");
    expectTrue(!preflight.empty_table, "preflight sees non-empty archetype table");
    expectTrue(!preflight.skipped, "preflight not skipped when entities are present");
    expectEq(preflight.matching_archetypes, 1u, "preflight counts matching archetype");
    expectEq(preflight.matching_entities, 2u, "preflight sums matching entity rows");
    expectTrue(preflight.can_iterate(), "preflight can_iterate when entities are present");
    expectTrue(preflight.can_count(), "preflight can_count on populated table");
    expectTrue(preflight.has_signature_match(), "preflight has_signature_match when archetype matches");

    const fuse::ecs::QueryFilterPreflight emptyTable = fuse::ecs::preflight_query_filter({}, filter);
    expectTrue(emptyTable.runnable, "preflight keeps runnable flag on empty table");
    expectTrue(!emptyTable.has_conflict, "preflight keeps conflict clear on empty table");
    expectTrue(emptyTable.empty_table, "preflight marks empty archetype table");
    expectTrue(emptyTable.skipped, "preflight marks skipped on empty table");
    expectEq(emptyTable.matching_archetypes, 0u, "preflight skips counts on empty table");
    expectEq(emptyTable.matching_entities, 0u, "preflight skips entity sum on empty table");
    expectTrue(!emptyTable.can_iterate(), "preflight can_iterate false on empty table");
    expectTrue(!emptyTable.can_count(), "preflight can_count false on empty table");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    const fuse::ecs::QueryFilterPreflight conflictPreflight = fuse::ecs::preflight_query_filter(table, conflicting);
    expectTrue(!conflictPreflight.runnable, "preflight marks conflicting filter as not runnable");
    expectTrue(conflictPreflight.has_conflict, "preflight marks conflicting filter");
    expectTrue(conflictPreflight.skipped, "preflight marks conflicting filter as skipped");
    expectEq(conflictPreflight.matching_archetypes, 0u, "preflight zeroes counts for conflicting filter");
    expectEq(conflictPreflight.matching_entities, 0u, "preflight zeroes entity sum for conflicting filter");
    expectTrue(!conflictPreflight.can_iterate(), "preflight can_iterate false for conflicting filter");
}

void testPreflightAlignsWithCountHelpers() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    fuse::ecs::Archetype staticBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });
    staticBody.append_entity(fuse::ecs::EntityID{1, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody, staticBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    const fuse::ecs::QueryFilterPreflight preflight = fuse::ecs::preflight_query_filter(table, filter);
    expectEq(preflight.matching_archetypes, fuse::ecs::count_matching_archetypes(table, filter),
             "preflight archetype count matches count_matching_archetypes");
    expectEq(preflight.matching_entities, fuse::ecs::count_matching_entities(table, filter),
             "preflight entity count matches count_matching_entities");
    expectTrue(preflight.can_iterate() == fuse::ecs::has_matching_entities(table, filter),
               "preflight can_iterate matches has_matching_entities");
    expectTrue((preflight.matching_archetypes > 0) == fuse::ecs::has_matching_archetypes(table, filter),
               "preflight archetype count aligns with has_matching_archetypes");
}

void testHasMatchingArchetypesAlignsWithCounts() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    fuse::ecs::Archetype staticBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });
    staticBody.append_entity(fuse::ecs::EntityID{1, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody, staticBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    expectTrue(fuse::ecs::query_filter_is_runnable(filter), "dynamic-body filter is runnable");
    expectTrue(fuse::ecs::has_matching_archetypes(table, filter),
               "has_matching_archetypes true when count_matching_archetypes is non-zero");
    expectEq(fuse::ecs::count_matching_archetypes(table, filter), 1u,
             "count_matching_archetypes finds one matching signature");
    expectEq(fuse::ecs::count_matching_entities(table, filter), 1u,
             "count_matching_entities sums rows from matching signature only");

    const fuse::ecs::QueryFilter missingWith =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::TagPlayer>{});
    expectTrue(fuse::ecs::query_filter_is_runnable(missingWith),
               "missing With filter remains runnable");
    expectTrue(!fuse::ecs::has_matching_archetypes(table, missingWith),
               "has_matching_archetypes false when no signature satisfies With set");
    expectEq(fuse::ecs::count_matching_archetypes(table, missingWith), 0u,
             "count_matching_archetypes returns zero when With set is unsatisfied");
    expectEq(fuse::ecs::count_matching_entities(table, missingWith), 0u,
             "count_matching_entities returns zero when With set is unsatisfied");
}

void testPreflightQueryFilterWithoutArchetypeTable() {
    const fuse::ecs::QueryFilter runnable =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    const fuse::ecs::QueryFilterPreflight runnablePreflight = fuse::ecs::preflight_query_filter(runnable);
    expectTrue(runnablePreflight.runnable, "filter-only preflight marks runnable filter");
    expectTrue(!runnablePreflight.has_conflict, "filter-only preflight clears conflict for runnable filter");
    expectTrue(runnablePreflight.empty_table, "filter-only preflight treats table as empty");
    expectTrue(runnablePreflight.skipped, "filter-only preflight marks skipped without a table");
    expectEq(runnablePreflight.matching_archetypes, 0u, "filter-only preflight leaves archetype count zero");
    expectEq(runnablePreflight.matching_entities, 0u, "filter-only preflight leaves entity count zero");
    expectTrue(!runnablePreflight.can_match(), "filter-only preflight cannot match without a table");
    expectTrue(!runnablePreflight.can_iterate(), "filter-only preflight cannot iterate without a table");
    expectTrue(runnablePreflight.should_skip(), "filter-only preflight should skip iteration");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    const fuse::ecs::QueryFilterPreflight conflictPreflight = fuse::ecs::preflight_query_filter(conflicting);
    expectTrue(!conflictPreflight.runnable, "filter-only preflight marks conflicting filter as not runnable");
    expectTrue(conflictPreflight.has_conflict, "filter-only preflight marks conflicting filter");
    expectTrue(conflictPreflight.skipped, "filter-only preflight skips conflicting filter");
    expectTrue(conflictPreflight.should_skip(), "filter-only preflight should skip conflicting filter");
}

void testPreflightCanMatchVersusCanIterate() {
    fuse::ecs::Archetype zeroRow = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    fuse::ecs::Archetype populated = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    populated.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {zeroRow, populated};
    const fuse::ecs::QueryFilter bodyFilter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{});
    const fuse::ecs::QueryFilter transformFilter =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});

    const fuse::ecs::QueryFilterPreflight bodyPreflight = fuse::ecs::preflight_query_filter(table, bodyFilter);
    expectTrue(bodyPreflight.can_match(), "zero-row matching signature still can_match");
    expectTrue(!bodyPreflight.can_iterate(), "zero-row matching signature cannot iterate");
    expectTrue(bodyPreflight.should_skip(), "zero-row matching signature should skip iteration");

    const fuse::ecs::QueryFilterPreflight transformPreflight =
        fuse::ecs::preflight_query_filter(table, transformFilter);
    expectTrue(transformPreflight.can_match(), "populated matching signature can_match");
    expectTrue(transformPreflight.can_iterate(), "populated matching signature can_iterate");
    expectTrue(!transformPreflight.should_skip(), "populated matching signature should not skip iteration");
}

void testShouldSkipQueryIteration() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    expectTrue(!fuse::ecs::should_skip_query_iteration(table, filter),
               "should_skip_query_iteration false when entities match");
    expectTrue(fuse::ecs::should_skip_query_iteration({}, filter),
               "should_skip_query_iteration true for empty archetype table");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(fuse::ecs::should_skip_query_iteration(table, conflicting),
               "should_skip_query_iteration true for conflicting filter");

    const fuse::ecs::QueryFilter missingWith =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::TagPlayer>{});
    expectTrue(fuse::ecs::should_skip_query_iteration(table, missingWith),
               "should_skip_query_iteration true when no signature satisfies With set");
}

void testPreflightCanMatchAlignsWithHasMatchingArchetypes() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    const fuse::ecs::QueryFilterPreflight preflight = fuse::ecs::preflight_query_filter(table, filter);
    expectTrue(preflight.can_match() == fuse::ecs::has_matching_archetypes(table, filter),
               "preflight can_match matches has_matching_archetypes");
    expectTrue(preflight.should_skip() == fuse::ecs::should_skip_query_iteration(table, filter),
               "preflight should_skip matches should_skip_query_iteration");
}

void testPreflightSkippedWhenNoMatchingSignaturesOrEntities() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody};
    const fuse::ecs::QueryFilter missingWith =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::TagPlayer>{});
    const fuse::ecs::QueryFilterPreflight missingPreflight = fuse::ecs::preflight_query_filter(table, missingWith);
    expectTrue(missingPreflight.skipped, "preflight skipped when no signature satisfies With set");
    expectTrue(!missingPreflight.has_signature_match(), "missing With yields no signature match");
    expectTrue(!missingPreflight.can_iterate(), "missing With cannot iterate");

    fuse::ecs::Archetype staticBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
        std::type_index(typeid(fuse::ecs::TagStatic)),
    });
    staticBody.append_entity(fuse::ecs::EntityID{1, 1});
    const std::vector<fuse::ecs::Archetype> staticTable = {staticBody};
    const fuse::ecs::QueryFilter excluded = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});
    const fuse::ecs::QueryFilterPreflight excludedPreflight = fuse::ecs::preflight_query_filter(staticTable, excluded);
    expectTrue(excludedPreflight.skipped, "Without exclusion skips tagged archetype");
    expectTrue(!excludedPreflight.has_signature_match(), "excluded archetype yields no signature match");
    expectEq(excludedPreflight.matching_entities, 0u, "excluded archetype yields zero entity rows");

    fuse::ecs::Archetype zeroRow = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    const std::vector<fuse::ecs::Archetype> zeroRowTable = {zeroRow};
    const fuse::ecs::QueryFilter bodyFilter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{});
    const fuse::ecs::QueryFilterPreflight zeroRowPreflight = fuse::ecs::preflight_query_filter(zeroRowTable, bodyFilter);
    expectTrue(zeroRowPreflight.has_signature_match(), "zero-row archetype still has signature match");
    expectTrue(zeroRowPreflight.skipped, "zero-row signature match is still skipped");
    expectTrue(!zeroRowPreflight.can_iterate(), "zero-row signature match cannot iterate");
}

void testShouldSkipAndCanIterateQueryFilterGuards() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    const fuse::ecs::QueryFilterPreflight preflight = fuse::ecs::preflight_query_filter(table, filter);
    expectTrue(!fuse::ecs::should_skip_query_iteration(table, filter),
               "should_skip_query_iteration false when entities match");
    expectTrue(fuse::ecs::can_iterate_query_filter(table, filter),
               "can_iterate_query_filter true when entities match");
    expectTrue(fuse::ecs::can_iterate_query_filter(table, filter) == preflight.can_iterate(),
               "can_iterate_query_filter matches preflight can_iterate");
    expectTrue(fuse::ecs::should_skip_query_iteration(table, filter) == preflight.skipped,
               "should_skip_query_iteration matches preflight skipped");

    expectTrue(fuse::ecs::should_skip_query_iteration({}, filter),
               "should_skip_query_iteration true for empty table");
    expectTrue(!fuse::ecs::can_iterate_query_filter({}, filter),
               "can_iterate_query_filter false for empty table");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(fuse::ecs::should_skip_query_iteration(table, conflicting),
               "should_skip_query_iteration true for conflicting filter");
    expectTrue(!fuse::ecs::can_iterate_query_filter(table, conflicting),
               "can_iterate_query_filter false for conflicting filter");
}

void testPreflightConflictGuardShortCircuitsCounts() {
    fuse::ecs::Archetype typed = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    typed.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {typed};
    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});

    const fuse::ecs::QueryFilterPreflight preflight = fuse::ecs::preflight_query_filter(table, conflicting);
    expectTrue(preflight.has_conflict, "conflict guard marks has_conflict");
    expectTrue(preflight.skipped, "conflict guard short-circuits to skipped");
    expectTrue(preflight.should_skip_match(), "conflict guard skips signature match");
    expectTrue(!preflight.can_count(), "conflict guard prevents counting");
    expectTrue(preflight.has_conflict == fuse::ecs::query_filter_has_conflict(conflicting),
               "preflight has_conflict matches query_filter_has_conflict");
    expectEq(fuse::ecs::count_matching_archetypes(table, conflicting), 0u,
             "count_matching_archetypes short-circuits on conflict");
    expectEq(fuse::ecs::count_matching_entities(table, conflicting), 0u,
             "count_matching_entities short-circuits on conflict");
}

void testPreflightExposesConflictGuard() {
    const fuse::ecs::QueryFilter runnable =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform>{});
    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});

    const fuse::ecs::QueryFilterPreflight runnablePreflight = fuse::ecs::preflight_query_filter(runnable);
    expectTrue(!runnablePreflight.has_conflict, "runnable filter has no conflict");
    expectTrue(runnablePreflight.runnable, "runnable filter remains runnable");
    expectTrue(runnablePreflight.should_skip_match(), "filter-only preflight skips signature match");

    const fuse::ecs::QueryFilterPreflight conflictPreflight = fuse::ecs::preflight_query_filter(conflicting);
    expectTrue(conflictPreflight.has_conflict, "conflicting filter reports has_conflict");
    expectTrue(!conflictPreflight.runnable, "conflicting filter is not runnable");
    expectTrue(conflictPreflight.should_skip_match(), "conflicting filter skips signature match");
    expectTrue(conflictPreflight.should_skip(), "conflicting filter skips iteration");

    fuse::ecs::Archetype typed = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    typed.append_entity(fuse::ecs::EntityID{0, 1});
    const std::vector<fuse::ecs::Archetype> table = {typed};

    const fuse::ecs::QueryFilterPreflight tableConflict = fuse::ecs::preflight_query_filter(table, conflicting);
    expectTrue(tableConflict.has_conflict, "table preflight surfaces conflict before scanning");
    expectEq(tableConflict.matching_archetypes, 0u, "conflict short-circuits archetype count");
    expectEq(tableConflict.matching_entities, 0u, "conflict short-circuits entity count");
    expectTrue(tableConflict.should_skip_match(), "table conflict skips signature match");
    expectTrue(fuse::ecs::query_filter_has_conflict(conflicting) == tableConflict.has_conflict,
               "preflight has_conflict matches query_filter_has_conflict");
}

void testShouldSkipQueryMatch() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    expectTrue(!fuse::ecs::should_skip_query_match(table, filter),
               "should_skip_query_match false when a signature matches");
    expectTrue(fuse::ecs::should_skip_query_match({}, filter),
               "should_skip_query_match true for empty archetype table");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(fuse::ecs::should_skip_query_match(table, conflicting),
               "should_skip_query_match true for conflicting filter");

    const fuse::ecs::QueryFilter missingWith =
        fuse::ecs::make_query_filter(fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::TagPlayer>{});
    expectTrue(fuse::ecs::should_skip_query_match(table, missingWith),
               "should_skip_query_match true when no signature satisfies With set");

    fuse::ecs::Archetype zeroRow = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    const std::vector<fuse::ecs::Archetype> zeroRowTable = {zeroRow};
    expectTrue(!fuse::ecs::should_skip_query_match(zeroRowTable, filter),
               "should_skip_query_match false for zero-row matching signature");
    expectTrue(fuse::ecs::should_skip_query_iteration(zeroRowTable, filter),
               "should_skip_query_iteration true for zero-row matching signature");
}

void testConflictGuardAlignsAcrossCountHelpers() {
    fuse::ecs::Archetype typed = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
    });
    typed.append_entity(fuse::ecs::EntityID{0, 1});
    const std::vector<fuse::ecs::Archetype> table = {typed};

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});

    expectEq(fuse::ecs::count_matching_archetypes(table, conflicting), 0u,
             "count_matching_archetypes returns zero under conflict guard");
    expectEq(fuse::ecs::count_matching_entities(table, conflicting), 0u,
             "count_matching_entities returns zero under conflict guard");
    expectTrue(!fuse::ecs::has_matching_archetypes(table, conflicting),
               "has_matching_archetypes false under conflict guard");
    expectTrue(!fuse::ecs::has_matching_entities(table, conflicting),
               "has_matching_entities false under conflict guard");
    expectTrue(fuse::ecs::should_skip_query_match(table, conflicting),
               "should_skip_query_match true under conflict guard");
    expectTrue(fuse::ecs::should_skip_query_iteration(table, conflicting),
               "should_skip_query_iteration true under conflict guard");
}

void testCanMatchQueryFilterGuard() {
    fuse::ecs::Archetype dynamicBody = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    dynamicBody.append_entity(fuse::ecs::EntityID{0, 1});

    fuse::ecs::Archetype zeroRow = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });

    const std::vector<fuse::ecs::Archetype> table = {dynamicBody, zeroRow};
    const fuse::ecs::QueryFilter filter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{},
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    expectTrue(fuse::ecs::can_match_query_filter(table, filter),
               "can_match_query_filter true when a signature matches");
    expectTrue(fuse::ecs::can_match_query_filter(table, filter) ==
                   fuse::ecs::has_matching_archetypes(table, filter),
               "can_match_query_filter matches has_matching_archetypes");
    expectTrue(!fuse::ecs::can_match_query_filter({}, filter),
               "can_match_query_filter false for empty archetype table");

    fuse::ecs::QueryFilter conflicting = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform>{}, fuse::ecs::Without<fuse::ecs::Transform>{});
    expectTrue(!fuse::ecs::can_match_query_filter(table, conflicting),
               "can_match_query_filter false for conflicting filter");
}

void testSignatureMatchVersusIterationSkipGuards() {
    fuse::ecs::Archetype zeroRow = makeArchetypeWithComponents({
        std::type_index(typeid(fuse::ecs::Transform)),
        std::type_index(typeid(fuse::ecs::RigidBody)),
    });
    const std::vector<fuse::ecs::Archetype> zeroRowTable = {zeroRow};
    const fuse::ecs::QueryFilter bodyFilter = fuse::ecs::make_query_filter(
        fuse::ecs::With<fuse::ecs::Transform, fuse::ecs::RigidBody>{});

    const fuse::ecs::QueryFilterPreflight preflight = fuse::ecs::preflight_query_filter(zeroRowTable, bodyFilter);
    expectTrue(preflight.has_signature_match(), "zero-row archetype still has signature match");
    expectTrue(!preflight.should_skip_match(), "zero-row signature match does not skip match guard");
    expectTrue(preflight.should_skip(), "zero-row signature match still skips iteration");
    expectTrue(preflight.should_skip_match() == fuse::ecs::should_skip_query_match(zeroRowTable, bodyFilter),
               "preflight should_skip_match matches should_skip_query_match");
    expectTrue(preflight.should_skip() == fuse::ecs::should_skip_query_iteration(zeroRowTable, bodyFilter),
               "preflight should_skip matches should_skip_query_iteration");
}

} // namespace

int main() {
    testArchetypeMatchesWithRequiredComponents();
    testArchetypeMatchesWithoutExcludedComponents();
    testArchetypeMatchesEmptyArchetype();
    testCompileTimeArchetypeMatches();
    testArchetypeMatchesMultipleWithout();
    testQueryFilterEmptyAndConflict();
    testArchetypeMatchesConflictingFilter();
    testArchetypeMatchesWithoutOnly();
    testArchetypeMatchesZeroEntityArchetype();
    testEachQueryEmptyRegistry();
    testEachQueryWithWithoutFilters();
    testEachQueryEmptyMatch();
    testEachWithWithoutMatchesEachQuery();
    testEachParallelWithWithoutMatchesEachQueryParallel();
    testEachQueryParallelMatchesSerial();
    testQueryFilterEqual();
    testCountMatchingEntities();
    testEmptyFilterMatchesAllArchetypes();
    testArchetypeMatchesMultipleWithoutTypes();
    testCountMatchingEntitiesMatchesEachQuery();
    testEachQueryEmptyRegistryWithoutOnlyArchetype();
    testQueryFilterIsRunnable();
    testHasMatchingArchetypesEmptyTableAndConflict();
    testCountMatchingArchetypesConflictingFilter();
    testCountMatchingEntitiesEmptyTable();
    testHasMatchingEntitiesDistinguishesZeroRowArchetypes();
    testPreflightQueryFilterBundlesGuardsAndCounts();
    testPreflightSkippedWhenNoMatchingSignaturesOrEntities();
    testShouldSkipAndCanIterateQueryFilterGuards();
    testPreflightConflictGuardShortCircuitsCounts();
    testPreflightExposesConflictGuard();
    testShouldSkipQueryMatch();
    testConflictGuardAlignsAcrossCountHelpers();
    testCanMatchQueryFilterGuard();
    testSignatureMatchVersusIterationSkipGuards();
    testPreflightAlignsWithCountHelpers();
    testHasMatchingArchetypesAlignsWithCounts();
    testPreflightQueryFilterWithoutArchetypeTable();
    testPreflightCanMatchVersusCanIterate();
    testShouldSkipQueryIteration();
    testPreflightCanMatchAlignsWithHasMatchingArchetypes();

    if (g_failures == 0) {
        std::printf("fuse_ecs_query_filter_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_query_filter_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
