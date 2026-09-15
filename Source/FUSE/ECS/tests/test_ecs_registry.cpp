#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

#include <cstdio>
#include <cstdlib>

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

void testCreateDestroy() {
    fuse::ecs::Registry reg;
    reg.init(1024);

    const fuse::ecs::EntityID a = reg.create();
    const fuse::ecs::EntityID b = reg.create();

    expectTrue(a.valid(), "created entity is valid");
    expectTrue(b.valid(), "second entity is valid");
    expectEq(reg.count(), 2u, "two live entities");

    reg.destroy_entity(a);
    expectTrue(!reg.alive(a), "destroyed entity is not alive");
    expectTrue(reg.alive(b), "sibling entity remains alive");
    expectEq(reg.count(), 1u, "one live entity after destroy");
}

void testStaleHandle() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID id = reg.create();
    reg.destroy_entity(id);

    const fuse::ecs::EntityID recycled = reg.create();
    expectTrue(recycled.index == id.index, "entity index recycled");
    expectTrue(recycled.generation != id.generation, "generation bumped on recycle");
    expectTrue(!reg.alive(id), "stale handle rejected");
    expectTrue(reg.alive(recycled), "new generation accepted");
}

void testAddGetRemove() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID id = reg.create();
    expectTrue(!reg.has<fuse::ecs::Transform>(id), "no transform initially");

    fuse::ecs::Transform& transform = reg.add<fuse::ecs::Transform>(id);
    transform.position.x = 3.f;
    transform.position.y = 4.f;

    expectTrue(reg.has<fuse::ecs::Transform>(id), "transform added");
    expectTrue(reg.get<fuse::ecs::Transform>(id)->position.x == 3.f, "transform value stored");

    reg.add<fuse::ecs::RigidBody>(id);
    expectTrue(reg.has<fuse::ecs::RigidBody>(id), "rigidbody added alongside transform");

    reg.remove<fuse::ecs::RigidBody>(id);
    expectTrue(!reg.has<fuse::ecs::RigidBody>(id), "rigidbody removed");
    expectTrue(reg.has<fuse::ecs::Transform>(id), "transform survives rigidbody removal");
}

void testArchetypeGrouping() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID with_body = reg.create();
    const fuse::ecs::EntityID transform_only = reg.create();

    reg.add<fuse::ecs::Transform>(with_body);
    reg.add<fuse::ecs::RigidBody>(with_body);
    reg.add<fuse::ecs::Transform>(transform_only);

    expectTrue(reg.archetype_count() >= 3u, "empty + two component signatures");

    reg.add<fuse::ecs::TagStatic>(with_body);
    expectTrue(reg.has<fuse::ecs::TagStatic>(with_body), "tag marker added");
}

void testEachIteration() {
    fuse::ecs::Registry reg;
    reg.init(64);

    fuse::ecs::EntityID ids[3];
    for (int i = 0; i < 3; ++i) {
        ids[i] = reg.create();
        fuse::ecs::Transform& t = reg.add<fuse::ecs::Transform>(ids[i]);
        t.position.x = static_cast<float>(i + 1);
    }

    fuse::u32 seen = 0;
    reg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID id, fuse::ecs::Transform& transform) {
        expectTrue(reg.alive(id), "each visits live entity");
        expectTrue(transform.position.x >= 1.f, "each sees transform data");
        ++seen;
    });
    expectEq(seen, 3u, "each visits all transform entities");
}

} // namespace

int main() {
    testCreateDestroy();
    testStaleHandle();
    testAddGetRemove();
    testArchetypeGrouping();
    testEachIteration();

    if (g_failures == 0) {
        std::printf("fuse_ecs_registry_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_registry_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
