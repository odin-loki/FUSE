#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/camera.hpp>
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

void testCreateAtRevivesSameId() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID a = reg.create();
    const fuse::ecs::EntityID b = reg.create();
    const fuse::ecs::EntityID c = reg.create();
    reg.add(b, fuse::ecs::Transform{});
    expectTrue(!reg.can_create_at(b) && !reg.create_at(b).valid(), "create_at rejects a live id");

    reg.destroy_entity(b);
    reg.destroy_entity(a);
    const fuse::ecs::EntityID stale{b.index, b.generation + 1u};
    expectTrue(!reg.create_at(stale).valid(), "create_at rejects a generation that was never issued");
    expectTrue(!reg.create_at(fuse::ecs::EntityID{99u, 1u}).valid(), "create_at rejects unknown slots");
    expectTrue(!reg.create_at(fuse::ecs::EntityID::null()).valid(), "create_at rejects null");

    const fuse::ecs::EntityID revived = reg.create_at(b);
    expectTrue(revived == b && reg.alive(b), "create_at revives the exact id");
    expectEq(static_cast<fuse::u32>(reg.count()), 2u, "revival counted as live");
    expectTrue(!reg.has<fuse::ecs::Transform>(b), "revived entity starts with no components");
    reg.add(b, fuse::ecs::Transform{});
    expectTrue(reg.has<fuse::ecs::Transform>(b), "revived entity accepts components");

    // The revived slot left the free list: the next create() takes a's slot, then a fresh index.
    const fuse::ecs::EntityID next = reg.create();
    expectTrue(next.index == a.index && next.generation == a.generation + 1u, "create() recycles the remaining free slot");
    const fuse::ecs::EntityID fresh = reg.create();
    expectTrue(fresh.index == 3u, "revived index is never handed out twice");
    expectTrue(!reg.create_at(a).valid(), "create_at fails once the slot was reused");

    // Destroy + revive + destroy + recycle keeps generations monotonic.
    reg.destroy_entity(b);
    const fuse::ecs::EntityID recycled = reg.create();
    expectTrue(recycled.index == b.index && recycled.generation == b.generation + 1u && !reg.alive(b),
               "slot recycles normally after the revived entity dies again");
    expectTrue(reg.alive(c), "unrelated entity untouched");

    // Reserved destroy: create() cannot reuse the slot, create_at can, release returns it.
    reg.destroy_entity_reserved(c);
    expectTrue(!reg.alive(c) && reg.is_reserved(c), "reserved destroy kills the entity and reserves the slot");
    const fuse::ecs::EntityID other = reg.create();
    expectTrue(other.index != c.index, "create() skips a reserved slot");
    expectTrue(reg.create_at(c) == c && reg.alive(c) && !reg.is_reserved(c), "create_at revives a reserved slot");
    reg.destroy_entity_reserved(c);
    reg.release_reserved(c);
    expectTrue(!reg.is_reserved(c) && reg.can_create_at(c), "released slot is free again (still revivable)");
    reg.release_reserved(c); // second release is a no-op (no duplicate free-list entry)
    const fuse::ecs::EntityID reuse = reg.create();
    expectTrue(reuse.index == c.index && reuse.generation == c.generation + 1u, "released slot recycles");
    const fuse::ecs::EntityID after = reg.create();
    expectTrue(after.index != c.index, "released slot was on the free list exactly once");
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

void testUninitializedRegistrySafe() {
    fuse::ecs::Registry reg;
    const fuse::ecs::EntityID id = reg.create();
    expectTrue(id.valid(), "lazy init allows create without explicit init()");
    expectTrue(reg.alive(id), "entity alive after lazy init create");
    expectTrue(!reg.has_all<fuse::ecs::Transform, fuse::ecs::Mesh>(id),
               "has_all safe on entity without components");
}

void testHasAllComponents() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID meshEntity = reg.create();
    const fuse::ecs::EntityID transformOnly = reg.create();

    reg.add<fuse::ecs::Transform>(meshEntity);
    reg.add<fuse::ecs::Mesh>(meshEntity);
    reg.add<fuse::ecs::Transform>(transformOnly);

    expectTrue(reg.has_all<fuse::ecs::Transform, fuse::ecs::Mesh>(meshEntity),
               "has_all matches each<> component set");
    expectTrue(!reg.has_all<fuse::ecs::Transform, fuse::ecs::Mesh>(transformOnly),
               "has_all rejects partial component sets");
    expectTrue(reg.has<fuse::ecs::Transform>(transformOnly), "single-type has still works");
}

void testCameraThenTransformAdd() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID camera_entity = reg.create();
    fuse::ecs::Camera camera{};
    camera.is_active = true;
    reg.add(camera_entity, camera);
    expectTrue(reg.get<fuse::ecs::Camera>(camera_entity) != nullptr &&
                   reg.get<fuse::ecs::Camera>(camera_entity)->is_active,
               "camera active flag stored on first add");

    fuse::ecs::Transform transform{};
    transform.position = {0.f, 0.f, -10.f, 1.f};
    reg.add(camera_entity, transform);

    expectTrue(reg.has<fuse::ecs::Camera>(camera_entity), "camera survives transform add");
    expectTrue(reg.has<fuse::ecs::Transform>(camera_entity), "transform added after camera");
    const fuse::ecs::Camera* camera_ptr = reg.get<fuse::ecs::Camera>(camera_entity);
    const fuse::ecs::Transform* transform_ptr = reg.get<fuse::ecs::Transform>(camera_entity);
    expectTrue(camera_ptr != nullptr, "camera get after transform add");
    expectTrue(transform_ptr != nullptr, "transform get after camera add");
}

} // namespace

int main() {
    testCreateDestroy();
    testStaleHandle();
    testCreateAtRevivesSameId();
    testAddGetRemove();
    testCameraThenTransformAdd();
    testArchetypeGrouping();
    testEachIteration();
    testUninitializedRegistrySafe();
    testHasAllComponents();

    if (g_failures == 0) {
        std::printf("fuse_ecs_registry_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_registry_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
