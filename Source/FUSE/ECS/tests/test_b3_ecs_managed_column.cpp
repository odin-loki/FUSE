// B3 managed-memory ECS column, CPU side (runs everywhere). Registry::set_column_memory_resource<Transform>
// routes every Transform column through a std::pmr::memory_resource — in the device gate a
// fuse::alloc::GPUManagedMemoryResource (cudaMallocManaged); here a bookkeeping resource that records the
// blocks it hands out. Proves on the CPU:
//   - every archetype's Transform column lives in the resource, other components' columns do not;
//   - that holds through archetype migrations (add/remove), swap-removes, column growth, archetype-vector
//     growth, registry copies, serialiser save/load, and rebinding (set -> null -> set) keeps the data;
//   - the single-source check (ecs_managed_column_check.hpp) finds every one of the 10k gate positions,
//     reading each_chunk<Transform> spans exactly as the CUDA kernel reads the managed column.

#include "ecs_managed_column_check.hpp"

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/registry_serialiser.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory_resource>
#include <span>
#include <vector>

namespace {

using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace ecs = fuse::ecs;
namespace gate = fuse::ecs_gate;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// new/delete resource that remembers every live block (the CPU stand-in for CUDA managed memory).
class TrackingResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] bool owns(const void* p, usize bytes) const {
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        auto it = m_blocks.upper_bound(addr);
        if (it == m_blocks.begin()) {
            return false;
        }
        --it;
        return addr >= it->first && addr + bytes <= it->first + it->second;
    }
    [[nodiscard]] usize live_blocks() const { return m_blocks.size(); }
    [[nodiscard]] u64 allocations() const { return m_allocations; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        m_blocks[reinterpret_cast<std::uintptr_t>(p)] = bytes;
        ++m_allocations;
        return p;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        m_blocks.erase(reinterpret_cast<std::uintptr_t>(p));
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

    std::map<std::uintptr_t, usize> m_blocks;
    u64 m_allocations = 0;
};

ecs::Transform gateTransform(u32 index) {
    ecs::Transform t{};
    gate::expectedPosition(index, t.position.x, t.position.y, t.position.z);
    t.position.w = 1.f;
    return t;
}

/// Runs the device kernel's per-row check on the host over each_chunk<Transform> spans.
struct ScanResult {
    u64 rows = 0;
    u64 mismatches = 0;
    u64 outside = 0; ///< rows whose Transform is not inside `resource`
    f64 checksum = 0.0;
};

ScanResult scan(ecs::Registry& registry, const TrackingResource* resource) {
    ScanResult r{};
    registry.each_chunk<ecs::Transform>([&](std::span<const ecs::EntityID> ids, std::span<ecs::Transform> column) {
        if (resource != nullptr && !resource->owns(column.data(), column.size_bytes())) {
            r.outside += column.size();
        }
        for (usize row = 0; row < column.size(); ++row) {
            r.mismatches += gate::positionMatches(column[row], ids[row].index) ? 0u : 1u;
            r.checksum += gate::positionChecksum(column[row]);
        }
        r.rows += column.size();
    });
    return r;
}

f64 expectedChecksum(ecs::Registry& registry) {
    f64 sum = 0.0;
    registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) {
        sum += gate::positionChecksum(gateTransform(id.index));
    });
    return sum;
}

void testManagedColumn() {
    TrackingResource resource;
    ecs::Registry registry;
    registry.init();
    registry.set_column_memory_resource<ecs::Transform>(&resource);
    expectTrue(registry.column_memory_resource(std::type_index(typeid(ecs::Transform))) == &resource,
               "resource recorded for Transform");

    std::vector<ecs::EntityID> ids;
    for (u32 i = 0; i < gate::kGateEntities; ++i) {
        const ecs::EntityID id = registry.create();
        ids.push_back(id);
        registry.add<ecs::Transform>(id, gateTransform(id.index));
        if (i % 3u == 0u) {
            registry.add<ecs::Mesh>(id);
        }
        if (i % 5u == 0u) {
            registry.add<ecs::RigidBody>(id);
        }
    }
    // Migrations and swap-removes after the positions are written.
    for (u32 i = 0; i < gate::kGateEntities; i += 7u) {
        registry.remove<ecs::RigidBody>(ids[i]);
    }
    for (u32 i = 1; i < gate::kGateEntities; i += 11u) {
        registry.destroy_entity(ids[i]);
    }
    for (u32 i = 0; i < 64u; ++i) {
        const ecs::EntityID id = registry.create();
        registry.add<ecs::Transform>(id, gateTransform(id.index));
    }
    expectTrue(registry.archetype_count() >= 4u, "gate scene spans several archetypes");

    const ScanResult r = scan(registry, &resource);
    std::printf("managed-column scan: %llu rows, %llu mismatches, %llu outside the resource, %llu resource allocations\n",
                static_cast<unsigned long long>(r.rows), static_cast<unsigned long long>(r.mismatches),
                static_cast<unsigned long long>(r.outside), static_cast<unsigned long long>(resource.allocations()));
    expectTrue(r.rows == registry.count(), "every live entity's Transform visited");
    expectTrue(r.mismatches == 0u, "every Transform position matches the gate (single-source check)");
    expectTrue(r.outside == 0u, "every Transform column lives in the column memory resource");
    expectTrue(r.checksum == expectedChecksum(registry), "checksum matches");

    bool meshOutside = true;
    registry.each_chunk<ecs::Mesh>([&](std::span<const ecs::EntityID>, std::span<ecs::Mesh> column) {
        meshOutside = meshOutside && !resource.owns(column.data(), 1u);
    });
    expectTrue(meshOutside, "other components keep new/delete storage");

    // Copies keep the resource.
    {
        ecs::Registry copy = registry;
        const ScanResult c = scan(copy, &resource);
        expectTrue(c.rows == r.rows && c.mismatches == 0u && c.outside == 0u, "registry copy keeps Transform in the resource");
    }

    // Rebinding moves the bytes and keeps the data.
    const usize blocksBound = resource.live_blocks();
    registry.set_column_memory_resource<ecs::Transform>(nullptr);
    expectTrue(resource.live_blocks() == 0u, "unset: every Transform column left the resource");
    const ScanResult unbound = scan(registry, nullptr);
    expectTrue(unbound.rows == r.rows && unbound.mismatches == 0u, "data intact after moving out of the resource");
    registry.set_column_memory_resource<ecs::Transform>(&resource);
    expectTrue(resource.live_blocks() > 0u && resource.live_blocks() <= blocksBound, "set again: columns moved back");
    const ScanResult rebound = scan(registry, &resource);
    expectTrue(rebound.rows == r.rows && rebound.mismatches == 0u && rebound.outside == 0u,
               "data intact after moving back into the resource");

    // Serialiser load into a registry with the resource set.
    const char* path = "fuse_b3_ecs_managed_column.fecs";
    const ecs::RegistrySerialiseResult saved = ecs::RegistrySerialiser::save(registry, path);
    expectTrue(saved.ok, "save gate scene");
    if (saved.ok) {
        ecs::Registry loaded;
        loaded.set_column_memory_resource<ecs::Transform>(&resource);
        const ecs::RegistrySerialiseResult result = ecs::RegistrySerialiser::load(path, loaded);
        expectTrue(result.ok, "load gate scene");
        const ScanResult l = scan(loaded, &resource);
        expectTrue(l.rows == r.rows && l.mismatches == 0u && l.outside == 0u,
                   "serialiser load places Transform columns in the resource");
    }
    std::remove(path);

    registry.destroy();
    expectTrue(resource.live_blocks() == 0u, "registry destroy returns every column block");
}

} // namespace

int main() {
    testManagedColumn();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b3_ecs_managed_column: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b3_ecs_managed_column: all checks passed\n");
    return EXIT_SUCCESS;
}
