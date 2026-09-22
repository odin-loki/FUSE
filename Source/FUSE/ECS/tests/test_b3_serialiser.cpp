// B3.9 serialisation gate rows (master plan, B3.7):
//  - scene save/load round-trips 10k entities with zero data loss — byte-identical component arrays
//  - async scene load completes and calls its callback on the main thread
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry_serialiser.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using namespace fuse::ecs;

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

/// 10k entities over several archetypes, with parent links, tags and some destroyed slots.
std::vector<EntityID> populate(Registry& reg) {
    std::mt19937 rng(10'000u);
    std::uniform_real_distribution<float> f(-100.f, 100.f);
    std::vector<EntityID> ids;
    for (fuse::u32 i = 0; i < 10'500u; ++i) {
        const EntityID id = reg.create();
        Transform t{};
        t.position = {f(rng), f(rng), f(rng), 1.f};
        t.scale = {1.f + (i % 3), 1.f, 1.f, 0.f};
        t.dirty = (i % 2u) == 0u;
        if (i > 0u && i % 5u == 0u) {
            t.parent = ids[i - 1u]; // links must survive the round trip
        }
        reg.add(id, t);
        switch (i % 4u) {
        case 0: {
            Mesh m{};
            m.index_count = i;
            m.material_id = i * 7u;
            m.aabb_min = {-f(rng), -1.f, -1.f, 0.f};
            reg.add(id, m);
            break;
        }
        case 1: {
            RigidBody rb{};
            rb.mass = 1.f + static_cast<float>(i);
            reg.add(id, rb);
            reg.add<TagStatic>(id);
            break;
        }
        case 2: {
            SDFObject s{};
            s.params = {f(rng), f(rng), f(rng), 0.f};
            reg.add(id, s);
            break;
        }
        default:
            break; // Transform only
        }
        ids.push_back(id);
    }
    for (fuse::u32 i = 3; i < ids.size(); i += 21u) {
        reg.destroy_entity(ids[i]); // leaves generations + free list to preserve
    }
    reg.create(); // an entity with no components (empty archetype)
    return ids;
}

/// Per component name: concatenated raw column bytes, in (entity index) order.
std::map<std::string, std::vector<unsigned char>> snapshot(Registry& reg, std::vector<EntityID>& aliveOut) {
    std::map<std::string, std::vector<unsigned char>> out;
    auto grab = [&](const char* name, const void* p, std::size_t size) {
        auto& v = out[name];
        const auto* b = static_cast<const unsigned char*>(p);
        v.insert(v.end(), b, b + size);
    };
    aliveOut.clear();
    for (fuse::u32 index = 0; index < 20'000u; ++index) {
        for (fuse::u32 gen = 1; gen <= 3u; ++gen) {
            const EntityID id{index, gen};
            if (!reg.alive(id)) {
                continue;
            }
            aliveOut.push_back(id);
            if (const auto* c = reg.get<Transform>(id)) grab("Transform", c, sizeof(*c));
            if (const auto* c = reg.get<Mesh>(id)) grab("Mesh", c, sizeof(*c));
            if (const auto* c = reg.get<RigidBody>(id)) grab("RigidBody", c, sizeof(*c));
            if (const auto* c = reg.get<SDFObject>(id)) grab("SDFObject", c, sizeof(*c));
            if (reg.has<TagStatic>(id)) grab("TagStatic", &index, sizeof(index));
        }
    }
    return out;
}

void testRoundTrip10k() {
    Registry original;
    original.init(kMaxEntities);
    const std::vector<EntityID> ids = populate(original);
    std::vector<EntityID> aliveBefore;
    const auto before = snapshot(original, aliveBefore);
    expectTrue(original.count() >= 10'000u, "at least 10k live entities");

    const std::string path = tempPath("fuse_b3_scene_roundtrip.fecs");
    const RegistrySerialiseResult saved = RegistrySerialiser::save(original, path);
    expectTrue(saved.ok, "save ok");
    if (!saved.ok) {
        std::fprintf(stderr, "  save error: %s\n", saved.error.c_str());
    }

    Registry loaded;
    const RegistrySerialiseResult result = RegistrySerialiser::load(path, loaded);
    expectTrue(result.ok, "load ok");
    if (!result.ok) {
        std::fprintf(stderr, "  load error: %s\n", result.error.c_str());
        return;
    }
    std::vector<EntityID> aliveAfter;
    const auto after = snapshot(loaded, aliveAfter);
    std::printf("serialiser: %zu entities, %zu archetype blocks, %ju bytes\n", result.entityCount,
                result.archetypeCount, static_cast<std::uintmax_t>(std::filesystem::file_size(path)));
    expectTrue(loaded.count() == original.count(), "entity count preserved");
    expectTrue(aliveAfter == aliveBefore, "identical entity ids (index + generation)");
    expectTrue(after == before, "byte-identical component arrays for every component type");

    bool deadStayDead = true;
    for (fuse::u32 i = 3; i < ids.size(); i += 21u) {
        deadStayDead = deadStayDead && !loaded.alive(ids[i]);
    }
    expectTrue(deadStayDead, "destroyed entities stay dead after load");
    const EntityID recycled = loaded.create();
    bool reusedFromFreeList = false;
    for (fuse::u32 i = 3; i < ids.size(); i += 21u) {
        reusedFromFreeList = reusedFromFreeList || (recycled.index == ids[i].index && recycled.generation > ids[i].generation);
    }
    expectTrue(reusedFromFreeList || recycled.index == 10'500u, "free list restored (recycled slot or next new index)");

    // Iteration works on the restored archetypes.
    std::size_t meshes = 0;
    loaded.each<Mesh, Transform>([&](EntityID, Mesh&, Transform&) { ++meshes; });
    std::size_t meshesBefore = 0;
    original.each<Mesh, Transform>([&](EntityID, Mesh&, Transform&) { ++meshesBefore; });
    expectTrue(meshes == meshesBefore, "each<> over restored archetypes");
    std::filesystem::remove(path);
}

void testRejectsCorruptFiles() {
    Registry reg;
    reg.init(64);
    reg.add<Transform>(reg.create());
    const std::string path = tempPath("fuse_b3_scene_corrupt.fecs");
    expectTrue(RegistrySerialiser::save(reg, path).ok, "save small scene");

    std::vector<char> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
    }
    Registry target;
    expectTrue(!RegistrySerialiser::load(path, target).ok, "truncated file rejected");

    bytes[0] ^= 0x5A;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const RegistrySerialiseResult badMagic = RegistrySerialiser::load(path, target);
    expectTrue(!badMagic.ok && badMagic.error.find("magic") != std::string::npos, "bad magic rejected");
    std::filesystem::remove(path);
}

void testAsyncLoadCallsBackOnMainThread() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    Registry source;
    source.init(kMaxEntities);
    populate(source);
    const std::string path = tempPath("fuse_b3_scene_async.fecs");
    expectTrue(RegistrySerialiser::save(source, path).ok, "save for async load");

    Registry target;
    RegistryLoadQueue queue;
    const std::thread::id mainThread = std::this_thread::get_id();
    bool called = false;
    bool onMain = false;
    bool ok = false;
    queue.load_async(path, [&](const RegistrySerialiseResult& result) {
        called = true;
        onMain = std::this_thread::get_id() == mainThread;
        ok = result.ok;
    });
    expectTrue(!called, "callback not invoked synchronously by load_async");

    // Game loop: pump each frame until the worker has finished parsing.
    for (int frame = 0; frame < 5000 && !called; ++frame) {
        queue.pump(target);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectTrue(called, "async load completes");
    expectTrue(onMain, "completion callback runs on the main (pumping) thread");
    expectTrue(ok && target.count() == source.count(), "async-loaded registry matches the saved one");
    expectTrue(queue.pending() == 0u, "no requests left pending");
    scheduler.shutdown();
    std::filesystem::remove(path);
}

} // namespace

int main() {
    testRoundTrip10k();
    testRejectsCorruptFiles();
    testAsyncLoadCallsBackOnMainThread();

    if (g_failures == 0) {
        std::printf("fuse_b3_serialiser: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b3_serialiser: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
