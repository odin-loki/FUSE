// E02 SceneRenderer CPU gates (CPU-only GpuScene; also run in the stub tree):
//   registry       engine ids / asset ids -> GpuScene mesh rows (procedural, .fusemeshlet bytes, .fusemeshlet file),
//                  holes map to kInvalidMesh, flush with nothing queued is a no-op, re-registering repoints the remap,
//                  bad bytes are rejected
//   material_feed  MaterialSystem rows -> GpuScene material table: every row once, then only rows whose version
//                  moved (several edits between two syncs upload once), the rows equal MaterialSystem::gpuRow, the
//                  commit uploads exactly the changed rows, an unchanged table makes no heap allocation
//   extract        ECS Transform + Mesh through GpuSceneEcsExtractor with the registry's remap: instance mesh rows
//                  are the remapped rows; replacing a mesh repoints existing instances at the next extract
//   procedural     cube / plane / sphere: triangle counts, analytic bounds, deterministic bytes, invalid descs fail
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_ecs.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/scene_renderer/mesh_registry.hpp>
#include <fuse/renderer/scene_renderer/procedural_meshes.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::scene_renderer;
using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
namespace ecs = fuse::ecs;

int g_failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool initCpuScene(gpu_scene::GpuScene& scene) {
    gpu_scene::GpuSceneDesc d{};
    d.instanceCapacity = 16;
    d.meshCapacity = 4;
    d.materialCapacity = 4;
    d.lightCapacity = 4;
    return scene.init(d) && !scene.gpuEnabled();
}

geometry::MeshletMesh sphereMesh() {
    ProceduralMeshDesc d{};
    d.shape = ProceduralShape::Sphere;
    d.radius = 0.5f;
    geometry::MeshletMesh m;
    buildProceduralMesh(d, m);
    return m;
}

int runRegistry() {
    gpu_scene::GpuScene scene;
    expect(initCpuScene(scene), "CPU-only GpuScene");
    MeshRegistry reg;
    scene.beginFrame(1);
    ProceduralMeshDesc cube{};
    ProceduralMeshDesc plane{};
    plane.shape = ProceduralShape::Plane;
    plane.size = 8.f;
    plane.segments = 4;
    expect(reg.registerProcedural(0, cube), "procedural cube registered");
    expect(reg.registerProcedural(3, plane), "procedural plane registered");
    const std::vector<u8> bytes = geometry::serialize_meshlet_mesh(sphereMesh());
    std::string error;
    expect(reg.registerMeshletBytes(7, bytes.data(), bytes.size(), &error), ".fusemeshlet bytes registered");
    std::filesystem::create_directories(FUSE_SR_TEST_TMP_DIR);
    const std::string path = std::string(FUSE_SR_TEST_TMP_DIR) + "/sphere.fusemeshlet";
    expect(geometry::write_meshlet_file(path, sphereMesh(), &error), "write .fusemeshlet");
    const u32 assetEngine = reg.engineIdForAsset(0xA55E7ull);
    expect(assetEngine == MeshRegistry::kFirstAssetEngineId, "first asset engine id");
    expect(reg.engineIdForAsset(0xA55E7ull) == assetEngine, "asset id maps to the same engine id");
    expect(reg.findAsset(0xB00ull) == MeshRegistry::kInvalidMesh, "unknown asset");
    expect(reg.registerMeshletFile(assetEngine, path, &error), ".fusemeshlet file registered");
    // Bad bytes: rejected, nothing queued.
    std::vector<u8> bad = bytes;
    bad[bad.size() / 2] ^= 0x5Au;
    expect(!reg.registerMeshletBytes(9, bad.data(), bad.size(), &error), "corrupt .fusemeshlet rejected");
    expect(!reg.registerMeshletBytes(9, nullptr, 0, &error), "null bytes rejected");
    expect(reg.pending() == 4u, "4 meshes queued");
    expect(reg.gpuMesh(0) == MeshRegistry::kInvalidMesh, "not flushed: no GpuScene row yet");

    const u64 v0 = reg.remapVersion();
    expect(reg.flush(scene) == 4u, "flush uploads 4 meshes");
    expect(scene.meshCount() == 4u, "GpuScene has 4 meshes");
    expect(reg.gpuMesh(0) == 0u && reg.gpuMesh(3) == 1u && reg.gpuMesh(7) == 2u && reg.gpuMesh(assetEngine) == 3u,
           "engine ids -> GpuScene rows in registration order");
    expect(reg.gpuMesh(1) == MeshRegistry::kInvalidMesh && reg.gpuMesh(9) == MeshRegistry::kInvalidMesh,
           "holes and rejected ids map to kInvalidMesh");
    expect(reg.remapCount() == assetEngine + 1u && reg.remap()[3] == 1u, "remap table covers every engine id");
    expect(reg.remapVersion() != v0, "flush bumps the remap version");
    expect(scene.mesh(1).triangleCount == 2u * 4u * 4u, "plane row carries the plane's triangles");
    const u64 v1 = reg.remapVersion();
    expect(reg.flush(scene) == 0u && reg.remapVersion() == v1 && scene.meshCount() == 4u, "empty flush is a no-op");
    f32 lo[3], hi[3];
    expect(reg.localBounds(3, lo, hi) && lo[0] == -4.f && hi[2] == 4.f && lo[1] == 0.f && hi[1] == 0.f,
           "plane analytic bounds");
    expect(reg.localBounds(7, lo, hi) && lo[0] < -0.45f && hi[0] > 0.45f && hi[0] < 0.55f, "sphere bounds from meshlet boxes");
    expect(!reg.localBounds(1, lo, hi), "no bounds for a hole");

    // Re-register id 0 (a sphere now): new row, remap repointed.
    expect(reg.registerMesh(0, sphereMesh()), "re-register id 0");
    expect(reg.flush(scene) == 1u && reg.gpuMesh(0) == 4u && reg.remapVersion() != v1, "replacement gets a new row");
    expect(reg.stats().registered == 5u && reg.stats().uploaded == 5u && reg.stats().failed == 2u, "registry stats");
    std::printf("registry: %u meshes, remap %u entries, asset engine id %u\n", scene.meshCount(), reg.remapCount(), assetEngine);
    return 0;
}

Material makeMaterial(f32 r, f32 g, f32 b, f32 metallic) {
    Material m{};
    m.baseColor = fuse::math::Vec3{r, g, b};
    m.metallic = metallic;
    m.roughness = 0.4f;
    return m;
}

int runMaterialFeed() {
    gpu_scene::GpuScene scene;
    expect(initCpuScene(scene), "CPU-only GpuScene");
    MaterialSystem mats;
    mats.initStandalone();
    expect(mats.isReady(), "standalone MaterialSystem");
    mats.registerMaterial(makeMaterial(0.8f, 0.2f, 0.2f, 0.f));
    mats.registerMaterial(makeMaterial(0.2f, 0.8f, 0.2f, 0.5f));
    mats.registerMaterial(makeMaterial(0.2f, 0.2f, 0.8f, 1.f));
    expect(mats.version(0) == 1u && mats.version(2) == 1u && mats.changeSerial() == 3u, "versions start at 1");

    gpu_scene::GpuSceneMaterialFeed feed;
    auto rowsEqual = [&]() {
        const gpu_scene::TableBytes t = scene.tableBytes(gpu_scene::GpuSceneTable::Materials);
        if (t.count < mats.materialCount()) {
            return false;
        }
        for (u32 i = 0; i < mats.materialCount(); ++i) {
            const Material::GPUMaterial want = mats.gpuRow(i);
            if (std::memcmp(t.data + static_cast<std::size_t>(i) * t.stride, &want, sizeof(want)) != 0) {
                return false;
            }
        }
        return true;
    };

    scene.beginFrame(1);
    gpu_scene::MaterialFeedStats s = feed.sync(mats, scene);
    expect(s.rowsWritten == 3u, "first sync writes every row");
    expect(rowsEqual(), "GpuScene rows == MaterialSystem::gpuRow");
    gpu_scene::GpuSceneCommitStats c = scene.commit();
    const u32 matTable = static_cast<u32>(gpu_scene::GpuSceneTable::Materials);
    expect(c.tables[matTable].dirtyRows == 3u, "commit uploads the 3 material rows");

    scene.beginFrame(2);
    t_allocations = 0;
    t_count = true;
    s = feed.sync(mats, scene);
    t_count = false;
    expect(s.rowsWritten == 0u && s.rowsChecked == 0u, "unchanged table: no row checked, none written");
    expect(t_allocations == 0u, "unchanged table: no heap allocation");
    c = scene.commit();
    expect(c.tables[matTable].dirtyRows == 0u, "unchanged table: no material upload");

    // Two edits of row 1 between syncs: one re-upload.
    mats.updateMaterial(1, makeMaterial(0.9f, 0.9f, 0.1f, 0.25f));
    mats.updateMaterial(1, makeMaterial(0.1f, 0.9f, 0.9f, 0.75f));
    expect(mats.version(1) == 3u, "two edits: version 3");
    scene.beginFrame(3);
    s = feed.sync(mats, scene);
    expect(s.rowsWritten == 1u && feed.syncedVersion(1) == 3u, "two edits upload the row once");
    expect(rowsEqual(), "edited row == gpuRow");
    c = scene.commit();
    expect(c.tables[matTable].dirtyRows == 1u, "commit uploads the one changed row");

    // Same bytes written again (version moved, contents equal): the feed writes, the mirror diff uploads nothing.
    mats.updateMaterial(0, mats.get(0));
    scene.beginFrame(4);
    s = feed.sync(mats, scene);
    c = scene.commit();
    expect(s.rowsWritten == 1u && c.tables[matTable].dirtyRows == 0u, "identical row: no upload");

    // New material.
    mats.registerMaterial(makeMaterial(1.f, 1.f, 1.f, 0.f));
    scene.beginFrame(5);
    s = feed.sync(mats, scene);
    c = scene.commit();
    expect(s.rowsWritten == 1u && scene.materialCount() == 4u && rowsEqual(), "new material appended");
    expect(c.tables[matTable].dirtyRows == 1u, "new material: one row uploaded");
    std::printf("material_feed: 4 rows, versions %u %u %u %u\n", mats.version(0), mats.version(1), mats.version(2), mats.version(3));
    return 0;
}

int runExtract() {
    gpu_scene::GpuScene scene;
    expect(initCpuScene(scene), "CPU-only GpuScene");
    MeshRegistry reg;
    ProceduralMeshDesc cube{};
    ProceduralMeshDesc plane{};
    plane.shape = ProceduralShape::Plane;
    reg.registerProcedural(5, plane);
    reg.registerProcedural(2, cube);
    scene.beginFrame(1);
    reg.flush(scene); // id 5 -> row 0, id 2 -> row 1

    ecs::Registry registry;
    registry.init(64);
    auto spawn = [&](u32 engineMesh, u32 material, f32 x) {
        const ecs::EntityID e = registry.create();
        ecs::Transform t{};
        t.local_to_world.data[12] = x;
        registry.add<ecs::Transform>(e, t);
        ecs::Mesh m{};
        m.vertex_buffer = ecs::MeshVertexBufferHandle(engineMesh, 1);
        m.material_id = material;
        registry.add<ecs::Mesh>(e, m);
        return e;
    };
    const ecs::EntityID a = spawn(5, 0, 0.f);
    const ecs::EntityID b = spawn(2, 1, 2.f);
    const ecs::EntityID c = spawn(9, 1, 4.f); // no mesh registered under 9

    gpu_scene::GpuSceneEcsExtractor ex;
    gpu_scene::EcsExtractDesc d{};
    d.backend = fuse::kernel::Backend::CpuReference;
    d.meshRemap = reg.remap();
    d.meshRemapCount = reg.remapCount();
    ex.init(d);
    gpu_scene::EcsExtractStats st = ex.extract(registry, scene);
    expect(st.added == 3u, "3 instances added");
    auto meshOf = [&](ecs::EntityID e) { return scene.instance(ex.instanceOf(e).slot).mesh; };
    expect(meshOf(a) == 0u && meshOf(b) == 1u, "instances use the remapped GpuScene rows");
    expect(meshOf(c) == gpu_scene::kInvalidIndex, "unregistered engine mesh: no mesh row");
    expect(scene.instance(ex.instanceOf(b).slot).material == 1u, "material id passes through");
    scene.commit();

    // Replace engine mesh 2 (-> row 2) and register 9 (-> row 3): existing instances follow at the next extract.
    scene.beginFrame(2);
    reg.registerMesh(2, sphereMesh());
    reg.registerProcedural(9, cube);
    reg.flush(scene);
    ex.setMeshRemap(reg.remap(), reg.remapCount());
    st = ex.extract(registry, scene);
    expect(st.added == 0u && st.instanceWrites == 2u, "two instance rows rewritten");
    expect(meshOf(a) == 0u && meshOf(b) == 2u && meshOf(c) == 3u, "instances repointed to the new rows");
    scene.commit();
    std::printf("extract: meshes a=%u b=%u c=%u\n", meshOf(a), meshOf(b), meshOf(c));
    return 0;
}

int runProcedural() {
    struct Case {
        ProceduralMeshDesc desc;
        u32 triangles;
        f32 lo[3];
        f32 hi[3];
    };
    ProceduralMeshDesc cube{};
    cube.halfExtent = 0.5f;
    ProceduralMeshDesc plane{};
    plane.shape = ProceduralShape::Plane;
    plane.size = 10.f;
    plane.segments = 3;
    ProceduralMeshDesc sphere{};
    sphere.shape = ProceduralShape::Sphere;
    sphere.radius = 2.f;
    sphere.rings = 6;
    sphere.segments = 8;
    const Case cases[3] = {{cube, 12u, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                           {plane, 18u, {-5.f, 0.f, -5.f}, {5.f, 0.f, 5.f}},
                           {sphere, 2u * 6u * 8u - 2u * 8u, {-2.f, -2.f, -2.f}, {2.f, 2.f, 2.f}}};
    for (const Case& k : cases) {
        geometry::MeshletMesh a;
        geometry::MeshletMesh b;
        f32 lo[3], hi[3];
        std::string error;
        expect(buildProceduralMesh(k.desc, a, lo, hi, &error), "procedural build");
        expect(buildProceduralMesh(k.desc, b), "procedural rebuild");
        expect(a.triangle_count() == k.triangles, "triangle count");
        expect(geometry::meshlet_mesh_equal(a, b), "deterministic bytes");
        expect(geometry::validate_meshlet_mesh(a, &error), "valid meshlet mesh");
        for (u32 i = 0; i < 3u; ++i) {
            expect(lo[i] == k.lo[i] && hi[i] == k.hi[i], "analytic bounds");
        }
    }
    ProceduralMeshDesc bad{};
    bad.halfExtent = 0.f;
    geometry::MeshletMesh m;
    expect(!buildProceduralMesh(bad, m), "zero cube rejected");
    bad.shape = ProceduralShape::Sphere;
    bad.segments = 2;
    expect(!buildProceduralMesh(bad, m), "degenerate sphere rejected");
    std::printf("procedural: cube / plane / sphere ok\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "registry" || suite == "all") {
        runRegistry();
    }
    if (suite == "material_feed" || suite == "all") {
        runMaterialFeed();
    }
    if (suite == "extract" || suite == "all") {
        runExtract();
    }
    if (suite == "procedural" || suite == "all") {
        runProcedural();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
