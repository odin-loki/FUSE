// WP-1.1 GPU scene CPU gates (docs/unification/RENDERER-EXECUTION.md). GpuScene runs in CPU-only
// mode with an upload sink that replays every committed range into a shadow copy ("the GPU"), so
// the delta logic is checked without a device; the Lavapipe twin is test_rp_gpu_scene.cpp.
//
//   table          DirtySet / MirrorTable / SlotAllocator unit checks; dense-scan ranges == sort ranges
//   delta_scaling  upload bytes per frame at 1k / 10k / 100k instances with 1% / 10% dirty, through the
//                  API and through the ECS extractor: bytes == 48 x (moved this frame + moved last
//                  frame) exactly, i.e. proportional to the changed instances and independent of N
//   extract        ECS extractor: CpuReference == CpuParallel (mirrors and ranges, 0/2/4 workers),
//                  TagStatic, lights, mesh remap, component removal, entity death, index reuse
//   churn          200 frames of random add / remove / update against a reference model (every third
//                  frame commits twice); shadow == mirror after every commit; stale handles rejected;
//                  no same-frame slot reuse; prev == last frame's cur
//   meshlets       GpuMeshlet == MSHL chunk bytes; geometry layout; mesh bounds contain every meshlet
//   zero_alloc     steady-state frames (extract + updates + commit) make 0 operator-new calls
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_ecs.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_meshlets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <random>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
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

using namespace fuse::renderer::gpu_scene;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
namespace ecs = fuse::ecs;
namespace kernel = fuse::kernel;
namespace geometry = fuse::renderer::geometry;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// Shadow "GPU": the upload sink replays every committed range here.
struct Shadow {
    std::vector<u8> tables[kGpuSceneTableCount + 1];
    u64 bytes = 0;
    u32 ranges = 0;

    static void sink(void* user, u32 table, u64 offset, const void* data, u64 size) {
        Shadow& s = *static_cast<Shadow*>(user);
        std::vector<u8>& dst = s.tables[table];
        if (dst.size() < offset + size) {
            dst.resize(offset + size, 0xCDu); // bytes never uploaded stay poisoned
        }
        std::memcpy(dst.data() + offset, data, size);
        s.bytes += size;
        ++s.ranges;
    }

    bool matches(const GpuScene& scene) const {
        for (u32 t = 0; t < kGpuSceneTableCount; ++t) {
            const TableBytes b = scene.tableBytes(static_cast<GpuSceneTable>(t));
            const u64 n = static_cast<u64>(b.count) * b.stride;
            if (n == 0) {
                continue;
            }
            if (tables[t].size() < n || std::memcmp(tables[t].data(), b.data, n) != 0) {
                std::fprintf(stderr, "  shadow mismatch in table %u\n", t);
                return false;
            }
        }
        const std::vector<u8>& h = tables[kGpuSceneTableCount];
        return h.size() == sizeof(GpuSceneHeader) && std::memcmp(h.data(), &scene.header(), sizeof(GpuSceneHeader)) == 0;
    }
};

GpuSceneDesc cpuDesc(Shadow* shadow, u32 instances = 1024) {
    GpuSceneDesc d{};
    d.instanceCapacity = instances;
    d.uploadSink = shadow != nullptr ? &Shadow::sink : nullptr;
    d.uploadSinkUser = shadow;
    return d;
}

GpuTransform translation(f32 x, f32 y, f32 z) {
    GpuTransform t{};
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

// --- table ---------------------------------------------------------------------------------------

int runTable() {
    DirtySet set;
    set.resize(1000);
    expect(set.mark(5) && !set.mark(5) && set.count() == 1u, "mark dedupes");
    std::vector<RowRange> ranges;
    ranges.reserve(1000);
    set.mark(6);
    set.mark(8);
    set.mark(3);
    set.buildRangesSorted(ranges, 0);
    expect(ranges.size() == 3u && ranges[0].first == 3u && ranges[1].first == 5u && ranges[1].count == 2u &&
               ranges[2].first == 8u,
           "sorted exact ranges");
    set.buildRangesSorted(ranges, 1);
    expect(ranges.size() == 1u && ranges[0].first == 3u && ranges[0].count == 6u, "gaps of 1 row merged");
    set.clear();
    expect(set.count() == 0u && !set.isMarked(5), "clear resets marks");

    std::mt19937 rng(7);
    for (u32 trial = 0; trial < 200; ++trial) {
        const u32 dirty = 1u + rng() % 900u;
        const u32 gap = rng() % 4u;
        for (u32 i = 0; i < dirty; ++i) {
            set.mark(rng() % 1000u);
        }
        std::vector<RowRange> dense;
        dense.reserve(1000);
        set.buildRanges(dense, gap, 1000); // dense scan when > 1/16 dirty
        std::vector<RowRange> sorted;
        sorted.reserve(1000);
        set.buildRangesSorted(sorted, gap);
        bool same = dense.size() == sorted.size();
        for (std::size_t i = 0; same && i < dense.size(); ++i) {
            same = dense[i].first == sorted[i].first && dense[i].count == sorted[i].count;
        }
        expect(same, "dense-scan ranges equal sorted ranges");
        u32 covered = 0;
        for (const RowRange& r : sorted) {
            covered += r.count;
        }
        expect(gap != 0u || covered == set.count(), "exact ranges cover exactly the dirty rows");
        set.clear();
    }

    MirrorTable<GpuTransform> table;
    expect(table.ensureCount(10) && table.capacity() == 64u && table.dirty().count() == 10u,
           "rows entering [0, count) are dirty (GPU never saw them)");
    table.dirty().clear();
    expect(!table.write(3, GpuTransform{}), "writing identical bytes is not a change");
    expect(table.write(3, translation(1, 2, 3)) && table.dirty().count() == 1u, "a real change marks the row");
    expect(table.ensureCount(100) && table.capacity() == 128u, "growth doubles capacity");

    SlotAllocator slots;
    slots.reserve(8);
    const SlotAllocator::Handle a = slots.allocate();
    const SlotAllocator::Handle b = slots.allocate();
    expect(a.slot == 0u && b.slot == 1u && slots.alive(a) && slots.live() == 2u, "slots allocate from the high-water mark");
    expect(slots.release(a) && !slots.alive(a) && !slots.release(a), "release invalidates the handle once");
    const SlotAllocator::Handle c = slots.allocate();
    expect(c.slot == 2u, "a slot freed this frame is not reused this frame");
    slots.beginFrame();
    const SlotAllocator::Handle d = slots.allocate();
    expect(d.slot == 0u && d.generation != a.generation && !slots.alive(a) && slots.alive(d),
           "after beginFrame the slot is reused with a new generation");
    return 0;
}

// --- delta scaling -------------------------------------------------------------------------------

struct DeltaRow {
    u32 instances = 0;
    u32 percent = 0;
    u32 changed = 0;
    u64 bytes = 0;
    u32 ranges = 0;
};

/// Frames of `changed` random moves each; returns the steady-state (3rd+ frame) bytes per frame.
DeltaRow measureApi(u32 instances, u32 percent, bool viaEcs, u32 mergeGapBytes, u64& totalBytes) {
    Shadow shadow;
    GpuSceneDesc desc = cpuDesc(&shadow, instances);
    desc.mergeGapBytes = mergeGapBytes;
    GpuScene scene;
    scene.init(desc);
    GpuMesh mesh{};
    mesh.meshletCount = 1;
    scene.addMesh(mesh);
    scene.setMaterial(0, GpuMaterial{});

    ecs::Registry registry;
    GpuSceneEcsExtractor extractor;
    std::vector<ecs::EntityID> entities;
    std::vector<InstanceHandle> handles;
    if (viaEcs) {
        registry.init(instances + 16u);
        EcsExtractDesc ed{};
        ed.entityCapacity = instances + 16u;
        ed.extractLights = false;
        extractor.init(ed);
        entities.reserve(instances);
        for (u32 i = 0; i < instances; ++i) {
            const ecs::EntityID e = registry.create();
            ecs::Transform t{};
            t.local_to_world.data[12] = static_cast<f32>(i);
            registry.add<ecs::Transform>(e, t);
            ecs::Mesh m{};
            m.vertex_buffer = ecs::MeshVertexBufferHandle(0, 1);
            registry.add<ecs::Mesh>(e, m);
            entities.push_back(e);
        }
    }
    handles.reserve(instances);
    std::mt19937 rng(instances * 31u + percent);
    const u32 changed = std::max(1u, instances * percent / 100u);
    std::vector<u32> pick(instances);
    DeltaRow row{instances, percent, changed, 0, 0};
    u64 serial = 0;
    for (u32 frame = 0; frame < 6; ++frame) {
        scene.beginFrame(++serial);
        if (frame == 0) {
            if (viaEcs) {
                extractor.extract(registry, scene);
            } else {
                for (u32 i = 0; i < instances; ++i) {
                    InstanceDesc d{};
                    d.mesh = 0;
                    d.material = 0;
                    d.transform = translation(static_cast<f32>(i), 0, 0);
                    handles.push_back(scene.addInstance(d));
                }
            }
        } else {
            for (u32 i = 0; i < instances; ++i) {
                pick[i] = i;
            }
            for (u32 i = 0; i < changed; ++i) { // partial Fisher-Yates: `changed` distinct instances
                std::swap(pick[i], pick[i + rng() % (instances - i)]);
            }
            for (u32 i = 0; i < changed; ++i) {
                const u32 k = pick[i];
                const f32 y = static_cast<f32>(frame * 1000u + k);
                if (viaEcs) {
                    registry.get<ecs::Transform>(entities[k])->local_to_world.data[13] = y;
                } else {
                    scene.setTransform(handles[k], translation(static_cast<f32>(k), y, 0));
                }
            }
            if (viaEcs) {
                const EcsExtractStats es = extractor.extract(registry, scene);
                expect(es.transformWrites == changed && es.instanceWrites == 0u && es.added == 0u,
                       "extractor rewrites exactly the moved transforms");
            }
        }
        const GpuSceneCommitStats stats = scene.commit();
        expect(stats.ok, "commit ok");
        expect(shadow.matches(scene), "shadow equals mirror after commit");
        if (frame >= 2) { // steady state: this frame's moves + last frame's (prev catches up)
            row.bytes = stats.bytes;
            row.ranges = stats.ranges;
            if (mergeGapBytes == 0u) {
                const u64 expected = static_cast<u64>(sizeof(GpuTransform)) * 2u * changed;
                const u32 moved = stats.tables[static_cast<u32>(GpuSceneTable::Transforms)].uploadedRows;
                const u32 prev = stats.tables[static_cast<u32>(GpuSceneTable::PrevTransforms)].uploadedRows;
                expect(moved == changed && prev == changed, "cur rows == moved, prev rows == moved last frame");
                expect(stats.bytes == expected, "delta bytes == 48 x (moved now + moved last frame)");
            } else {
                expect(stats.bytes <= static_cast<u64>(changed) * 2u * (sizeof(GpuTransform) + mergeGapBytes),
                       "merged-gap delta bytes bounded by changed x (row + gap)");
            }
        }
    }
    totalBytes = static_cast<u64>(instances) * (sizeof(GpuInstance) + 2u * sizeof(GpuTransform));
    return row;
}

int runDeltaScaling() {
    for (const bool viaEcs : {false, true}) {
        for (const u32 gap : {0u, 256u}) {
            if (viaEcs && gap != 0u) {
                continue;
            }
            std::printf("%s, merge gap %u bytes:\n", viaEcs ? "ECS extractor" : "GpuScene API", gap);
            std::printf("  %8s %5s %8s %12s %10s %14s %8s\n", "N", "dirty", "changed", "bytes/frame", "bytes/chg",
                        "instance data", "ratio");
            for (const u32 percent : {1u, 10u}) {
                f32 perChangedAt1k = 0.f;
                for (const u32 n : {1000u, 10000u, 100000u}) {
                    u64 total = 0;
                    const DeltaRow r = measureApi(n, percent, viaEcs, gap, total);
                    const f32 perChanged = static_cast<f32>(r.bytes) / static_cast<f32>(r.changed);
                    std::printf("  %8u %4u%% %8u %12llu %10.1f %14llu %7.4f\n", n, percent, r.changed,
                                static_cast<unsigned long long>(r.bytes), static_cast<double>(perChanged),
                                static_cast<unsigned long long>(total),
                                static_cast<double>(r.bytes) / static_cast<double>(total));
                    if (n == 1000u) {
                        perChangedAt1k = perChanged;
                    }
                    // Scaling: bytes per changed instance does not grow with N (exact at gap 0).
                    expect(perChanged <= perChangedAt1k * 1.0001f + (gap != 0u ? 64.f : 0.f),
                           "bytes per changed instance independent of the instance count");
                    expect(static_cast<f64>(r.bytes) < static_cast<f64>(total) * (2.5 * percent / 100.0) + 256.0,
                           "delta is a small fraction of the scene");
                }
            }
        }
    }
    // Instance-record deltas (material swaps) scale the same way: 32 bytes per change.
    Shadow shadow;
    GpuScene scene;
    scene.init(cpuDesc(&shadow, 10000));
    std::vector<InstanceHandle> handles;
    scene.beginFrame(1);
    for (u32 i = 0; i < 10000; ++i) {
        InstanceDesc d{};
        d.mesh = 0;
        d.material = 0;
        handles.push_back(scene.addInstance(d));
    }
    scene.commit();
    scene.beginFrame(2);
    for (u32 i = 0; i < 10000; i += 100) {
        scene.setInstanceMaterial(handles[i], 1);
    }
    const GpuSceneCommitStats s = scene.commit();
    expect(s.bytes == 100u * sizeof(GpuInstance) && s.ranges == 100u, "100 material swaps upload 100 x 32 bytes");
    expect(shadow.matches(scene), "material swap shadow");
    return 0;
}

// --- extract -------------------------------------------------------------------------------------

struct World {
    ecs::Registry registry;
    std::vector<ecs::EntityID> entities;
};

void buildWorld(World& w, u32 count, u32 seed) {
    w.registry.init(count * 2u + 64u);
    std::mt19937 rng(seed);
    for (u32 i = 0; i < count; ++i) {
        const ecs::EntityID e = w.registry.create();
        ecs::Transform t{};
        for (u32 k = 0; k < 16; ++k) {
            t.local_to_world.data[k] = static_cast<f32>(rng() % 1000u) * 0.25f;
        }
        w.registry.add<ecs::Transform>(e, t);
        ecs::Mesh m{};
        m.vertex_buffer = ecs::MeshVertexBufferHandle(i % 7u, 1);
        m.material_id = i % 5u;
        m.visible = (i % 3u) != 0u;
        m.cast_shadow = (i % 4u) != 0u;
        w.registry.add<ecs::Mesh>(e, m);
        if (i % 10u == 0u) {
            w.registry.add<ecs::TagStatic>(e);
        }
        if (i % 50u == 0u) {
            ecs::PointLight p{};
            p.intensity = static_cast<f32>(i);
            w.registry.add<ecs::PointLight>(e, p);
        }
        w.entities.push_back(e);
    }
}

void mutateWorld(World& w, u32 frame) {
    for (std::size_t i = frame % 3u; i < w.entities.size(); i += 17u) {
        if (ecs::Transform* t = w.registry.get<ecs::Transform>(w.entities[i])) {
            t->local_to_world.data[12] += 1.f;
        }
    }
}

bool mirrorsEqual(const GpuScene& a, const GpuScene& b) {
    for (u32 t = 0; t < kGpuSceneTableCount; ++t) {
        const TableBytes x = a.tableBytes(static_cast<GpuSceneTable>(t));
        const TableBytes y = b.tableBytes(static_cast<GpuSceneTable>(t));
        if (x.count != y.count || std::memcmp(x.data, y.data, static_cast<std::size_t>(x.count) * x.stride) != 0) {
            return false;
        }
    }
    return true;
}

int runExtract() {
    fuse::jobs::JobScheduler& jobs = fuse::jobs::JobScheduler::instance();
    for (const u32 workers : {0u, 2u, 4u}) {
        jobs.shutdown();
        jobs.initialize(workers);
        World wa, wb;
        buildWorld(wa, 3000, 11);
        buildWorld(wb, 3000, 11);
        Shadow sa, sb;
        GpuScene a, b;
        a.init(cpuDesc(&sa, 256)); // small capacity: growth happens during extraction
        b.init(cpuDesc(&sb, 256));
        GpuSceneEcsExtractor ea, eb;
        EcsExtractDesc da{};
        da.backend = kernel::Backend::CpuReference;
        ea.init(da);
        EcsExtractDesc db{};
        db.backend = kernel::Backend::CpuParallel;
        eb.init(db);
        for (u32 frame = 0; frame < 5; ++frame) {
            a.beginFrame(frame + 1u);
            b.beginFrame(frame + 1u);
            mutateWorld(wa, frame);
            mutateWorld(wb, frame);
            const EcsExtractStats xa = ea.extract(wa.registry, a);
            const EcsExtractStats xb = eb.extract(wb.registry, b);
            const GpuSceneCommitStats ca = a.commit();
            const GpuSceneCommitStats cb = b.commit();
            expect(xa.transformWrites == xb.transformWrites && xa.added == xb.added, "extract stats equal");
            expect(ca.bytes == cb.bytes && ca.ranges == cb.ranges, "CpuReference and CpuParallel upload the same ranges");
            expect(mirrorsEqual(a, b), "CpuReference and CpuParallel mirrors are bit-identical");
            expect(sa.matches(a) && sb.matches(b), "shadows equal mirrors");
            if (frame == 0) {
                expect(xa.added == 3000u && a.liveInstances() == 3000u && xa.lightsAdded == 60u, "first frame adds all");
            } else {
                expect(xa.added == 0u && xa.transformWrites > 0u && xa.instanceWrites == 0u, "later frames only move");
            }
        }
        const kernel::LaunchRecord last = kernel::last_launch();
        expect(last.name != nullptr && std::strcmp(last.name, "gpu_scene_extract") == 0 &&
                   last.backend == kernel::Backend::CpuParallel,
               "kernel stats name and backend");
    }
    jobs.shutdown();

    // Semantics on a hand-built world.
    World w;
    w.registry.init(64);
    const ecs::EntityID e0 = w.registry.create();
    ecs::Transform t0{};
    t0.local_to_world.data[12] = 5.f;
    w.registry.add<ecs::Transform>(e0, t0);
    ecs::Mesh m0{};
    m0.vertex_buffer = ecs::MeshVertexBufferHandle(2, 1);
    m0.material_id = 9;
    m0.receive_shadow = false;
    w.registry.add<ecs::Mesh>(e0, m0);
    const ecs::EntityID e1 = w.registry.create();
    w.registry.add<ecs::Transform>(e1, ecs::Transform{});
    w.registry.add<ecs::Mesh>(e1, ecs::Mesh{});
    w.registry.add<ecs::TagStatic>(e1);
    const ecs::EntityID lamp = w.registry.create();
    ecs::Transform lt{};
    lt.local_to_world.data[14] = 3.f;
    w.registry.add<ecs::Transform>(lamp, lt);
    ecs::SpotLight spot{};
    spot.outer_cone_deg = 60.f;
    w.registry.add<ecs::SpotLight>(lamp, spot);

    const u32 remap[3] = {40u, 41u, 42u};
    Shadow shadow;
    GpuScene scene;
    scene.init(cpuDesc(&shadow));
    GpuSceneEcsExtractor ex;
    EcsExtractDesc ed{};
    ed.meshRemap = remap;
    ed.meshRemapCount = 3;
    ex.init(ed);
    scene.beginFrame(1);
    ex.extract(w.registry, scene);
    scene.commit();
    const InstanceHandle h0 = ex.instanceOf(e0);
    const InstanceHandle h1 = ex.instanceOf(e1);
    expect(scene.alive(h0) && scene.alive(h1), "both mesh entities extracted");
    const GpuInstance& i0 = scene.instance(h0.slot);
    expect(i0.mesh == 42u && i0.material == 9u && i0.entityIndex == e0.index && i0.entityGeneration == e0.generation,
           "mesh remap, material and entity id");
    expect((i0.flags & kInstanceReceiveShadow) == 0u && (i0.flags & kInstanceValid) != 0u &&
               (i0.flags & kInstanceStatic) == 0u,
           "instance flags from Mesh");
    expect((scene.instance(h1.slot).flags & kInstanceStatic) != 0u && scene.instance(h1.slot).mesh == kInvalidIndex,
           "TagStatic flag; invalid vertex buffer -> invalid mesh");
    expect(scene.transform(h0.slot).rows[0][3] == 5.f && scene.prevTransform(h0.slot).rows[0][3] == 5.f,
           "column-major -> row-major translation; prev == cur on add");
    const LightHandle lh = ex.lightOf(lamp);
    expect(scene.lightAlive(lh) && scene.light(lh.slot).type == static_cast<u32>(GpuLightType::Spot) &&
               scene.light(lh.slot).position[2] == 3.f && scene.light(lh.slot).direction[2] == -1.f &&
               std::fabs(scene.light(lh.slot).cosOuter - 0.5f) < 1e-6f,
           "spot light packed");

    // Component removal, death and index reuse.
    scene.beginFrame(2);
    w.registry.remove<ecs::Mesh>(e1);
    w.registry.destroy_entity(e0);
    const ecs::EntityID e2 = w.registry.create(); // may reuse e0's index with a new generation
    w.registry.add<ecs::Transform>(e2, ecs::Transform{});
    w.registry.add<ecs::Mesh>(e2, ecs::Mesh{});
    w.registry.remove<ecs::SpotLight>(lamp);
    const EcsExtractStats st = ex.extract(w.registry, scene);
    scene.commit();
    expect(!scene.alive(h0) && !scene.alive(h1) && scene.alive(ex.instanceOf(e2)), "removed and re-added");
    expect(st.removed == 2u && st.added == 1u && st.lightsRemoved == 1u && !scene.lightAlive(lh), "removal stats");
    expect(scene.instance(h0.slot).flags == 0u && scene.instance(h1.slot).flags == 0u, "freed slots read as free");
    expect(ex.instanceOf(e2).slot != h0.slot && ex.instanceOf(e2).slot != h1.slot,
           "a slot freed this frame is not reused this frame");
    expect(shadow.matches(scene), "shadow after removal");
    return 0;
}

// --- churn ---------------------------------------------------------------------------------------

struct Expected {
    GpuInstance instance{};
    GpuTransform cur{};
    GpuTransform prevFrameCur{}; ///< cur at the end of the previous frame
    bool addedThisFrame = true;
};

int runChurn() {
    Shadow shadow;
    GpuScene scene;
    scene.init(cpuDesc(&shadow, 64)); // grows several times
    std::mt19937 rng(1234);
    std::map<u64, std::pair<InstanceHandle, Expected>> live;
    std::vector<InstanceHandle> dead;
    std::vector<u32> freedThisFrame;
    auto key = [](InstanceHandle h) { return (static_cast<u64>(h.slot) << 32) | h.generation; };
    u32 grown = 0;
    for (u32 frame = 1; frame <= 200; ++frame) {
        scene.beginFrame(frame);
        freedThisFrame.clear();
        for (auto& [k, v] : live) {
            (void)k;
            v.second.prevFrameCur = v.second.cur;
            v.second.addedThisFrame = false;
        }
        const u32 adds = rng() % 40u;
        const u32 removes = live.empty() ? 0u : rng() % std::min<u32>(30u, static_cast<u32>(live.size()));
        const u32 moves = live.empty() ? 0u : rng() % std::min<u32>(50u, static_cast<u32>(live.size()));
        for (u32 i = 0; i < removes && !live.empty(); ++i) {
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            expect(scene.removeInstance(it->second.first), "remove live instance");
            freedThisFrame.push_back(it->second.first.slot);
            dead.push_back(it->second.first);
            live.erase(it);
        }
        for (u32 i = 0; i < adds; ++i) {
            InstanceDesc d{};
            d.mesh = rng() % 8u;
            d.material = rng() % 16u;
            d.userData = rng();
            d.transform = translation(static_cast<f32>(rng() % 100u), static_cast<f32>(frame), 0);
            const InstanceHandle h = scene.addInstance(d);
            expect(std::find(freedThisFrame.begin(), freedThisFrame.end(), h.slot) == freedThisFrame.end(),
                   "no slot reuse in the frame that freed it");
            Expected e{};
            e.instance = scene.instance(h.slot);
            e.cur = d.transform;
            e.prevFrameCur = d.transform;
            live[key(h)] = {h, e};
        }
        if (frame % 3u == 0u) {
            // A second commit in the frame: deltas and prev bookkeeping must accumulate.
            expect(scene.commit().ok && shadow.matches(scene), "mid-frame commit");
        }
        for (u32 i = 0; i < moves && !live.empty(); ++i) {
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            const GpuTransform t = translation(static_cast<f32>(rng() % 100u), static_cast<f32>(frame), 1);
            const bool teleport = (rng() % 8u) == 0u;
            expect(scene.setTransform(it->second.first, t, teleport), "move live instance");
            it->second.second.cur = t;
            if (teleport) {
                it->second.second.prevFrameCur = t;
            }
            if ((rng() % 4u) == 0u) {
                const u32 material = rng() % 16u;
                scene.setInstanceMaterial(it->second.first, material);
                it->second.second.instance.material = material;
            }
        }
        for (u32 i = 0; i < 5 && !dead.empty(); ++i) {
            const InstanceHandle stale = dead[rng() % dead.size()];
            expect(!scene.setTransform(stale, GpuTransform{}) && !scene.removeInstance(stale), "stale handles rejected");
        }
        const GpuSceneCommitStats stats = scene.commit();
        grown += stats.tables[0].reallocated ? 1u : 0u;
        expect(stats.ok && shadow.matches(scene), "churn: shadow equals mirror after every commit");
        for (const auto& [k, v] : live) {
            (void)k;
            const u32 slot = v.first.slot;
            expect(std::memcmp(&scene.instance(slot), &v.second.instance, sizeof(GpuInstance)) == 0,
                   "live instance record");
            expect(std::memcmp(&scene.transform(slot), &v.second.cur, sizeof(GpuTransform)) == 0, "live transform");
            expect(std::memcmp(&scene.prevTransform(slot), &v.second.prevFrameCur, sizeof(GpuTransform)) == 0,
                   "prev == last frame's cur (or teleport / add)");
        }
        for (u32 slot = 0; slot < scene.instanceHighWater(); ++slot) {
            const bool isLive = (scene.instance(slot).flags & kInstanceValid) != 0u;
            const bool expectLive = live.count((static_cast<u64>(slot) << 32) | scene.slotGeneration(slot)) != 0u;
            expect(isLive == expectLive, "free slots read as free, live as live");
        }
        expect(scene.liveInstances() == live.size() && scene.header().liveInstances == live.size(), "live count");
    }
    std::printf("churn: 200 frames, %zu live, high water %u, %u reallocations\n", live.size(), scene.instanceHighWater(),
                grown);
    expect(grown >= 2u, "churn exercised table growth");
    return 0;
}

// --- meshlets ------------------------------------------------------------------------------------

bool buildSphere(geometry::MeshletMesh& out) {
    const u32 rings = 24, segments = 32;
    std::vector<f32> pos;
    std::vector<u32> idx;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = 3.14159265f * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = 6.2831853f * static_cast<f32>(s) / static_cast<f32>(segments);
            pos.push_back(std::sin(phi) * std::cos(theta) * 2.f + 1.f);
            pos.push_back(std::cos(phi) * 2.f);
            pos.push_back(std::sin(phi) * std::sin(theta) * 2.f - 3.f);
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    geometry::MeshletSource src{};
    src.positions = pos.data();
    src.vertex_count = static_cast<u32>(pos.size() / 3u);
    src.indices = idx.data();
    src.index_count = static_cast<u32>(idx.size());
    std::string error;
    return geometry::build_meshlets(src, geometry::MeshletBuildOptions{}, out, &error);
}

int runMeshlets() {
    geometry::MeshletMesh mesh;
    if (!buildSphere(mesh)) {
        std::fprintf(stderr, "FAIL: meshlet build\n");
        return 1;
    }
    // MSHL chunk bytes from the cooked file == our GPU records.
    const std::vector<u8> file = geometry::serialize_meshlet_mesh(mesh);
    u32 chunkCount = 0;
    std::memcpy(&chunkCount, file.data() + 16, 4);
    bool found = false;
    for (u32 c = 0; c < chunkCount; ++c) {
        const u8* entry = file.data() + 64 + c * 32u;
        u32 fourcc = 0, elementBytes = 0, count = 0;
        u64 offset = 0;
        std::memcpy(&fourcc, entry, 4);
        std::memcpy(&elementBytes, entry + 4, 4);
        std::memcpy(&count, entry + 8, 4);
        std::memcpy(&offset, entry + 16, 8);
        if (std::memcmp(&fourcc, "MSHL", 4) != 0) {
            continue;
        }
        found = true;
        expect(elementBytes == sizeof(GpuMeshlet) && count == mesh.meshlets.size(), "MSHL element size and count");
        bool same = true;
        for (u32 i = 0; i < count && same; ++i) {
            const GpuMeshlet g = packGpuMeshlet(mesh.meshlets[i]);
            same = std::memcmp(&g, file.data() + offset + static_cast<u64>(i) * elementBytes, sizeof(g)) == 0;
        }
        expect(same, "GpuMeshlet is byte-identical to the MSHL chunk element");
    }
    expect(found, "MSHL chunk present");

    std::vector<u8> blob;
    const MeshletGeometryLayout l = packMeshletGeometry(mesh, blob);
    expect(blob.size() == l.totalBytes && l.meshlets == 0u && l.positions % 16u == 0u && l.uvs % 16u == 0u,
           "sections 16-byte aligned");
    expect(std::memcmp(blob.data() + l.meshletTriangles, mesh.meshlet_triangles.data(),
                       mesh.meshlet_triangles.size() * 4u) == 0 &&
               std::memcmp(blob.data() + l.positions, mesh.positions.data(), mesh.positions.size() * 2u) == 0,
           "streams copied verbatim");
    const GpuMesh g = makeGpuMesh(mesh, l, 0x10000u, 0u);
    expect(g.meshlets == 0x10000u && g.positions == 0x10000u + l.positions && g.meshletCount == mesh.meshlets.size() &&
               g.triangleCount == mesh.triangle_count() && g.vertexCount == mesh.vertex_count(),
           "GpuMesh addresses and counts");
    geometry::DecodedVertices decoded;
    geometry::decode_vertices(mesh, decoded, kernel::Backend::CpuReference);
    bool contains = !decoded.positions.empty();
    for (std::size_t v = 0; v + 2 < decoded.positions.size(); v += 3) {
        f64 d2 = 0;
        for (u32 a = 0; a < 3; ++a) {
            const f64 d = static_cast<f64>(decoded.positions[v + a]) - g.boundsCenter[a];
            d2 += d * d;
        }
        contains = contains && std::sqrt(d2) <= static_cast<f64>(g.boundsRadius);
    }
    std::printf("mesh bounds: center (%.3f %.3f %.3f) radius %.4f, %zu meshlets, %u vertices\n",
                static_cast<double>(g.boundsCenter[0]), static_cast<double>(g.boundsCenter[1]),
                static_cast<double>(g.boundsCenter[2]), static_cast<double>(g.boundsRadius), mesh.meshlets.size(),
                mesh.vertex_count());
    expect(contains && g.boundsRadius > 1.99f && g.boundsRadius < 2.01f, "mesh sphere contains every decoded vertex");

    GpuScene scene;
    scene.init(cpuDesc(nullptr));
    const u32 index = scene.addMeshletMesh(mesh);
    expect(index == 0u && scene.mesh(0).meshletCount == mesh.meshlets.size() && scene.mesh(0).meshlets == 0u,
           "CPU-only addMeshletMesh: counts, no addresses");
    return 0;
}

// --- zero alloc ----------------------------------------------------------------------------------

int runZeroAlloc() {
    fuse::jobs::JobScheduler::instance().initialize(2);
    constexpr u32 kEntities = 20000;
    World w;
    buildWorld(w, kEntities, 5);
    GpuSceneDesc desc = cpuDesc(nullptr, kEntities + 1024u);
    desc.mergeGapBytes = 64;
    GpuScene scene;
    scene.init(desc);
    GpuSceneEcsExtractor ex;
    EcsExtractDesc ed{};
    ed.entityCapacity = kEntities * 2u + 64u;
    ex.init(ed);
    std::vector<InstanceHandle> extra;
    extra.reserve(64);
    unsigned long long allocations = 0;
    for (u32 frame = 1; frame <= 80; ++frame) {
        const bool measure = frame > 16;
        t_allocations = 0;
        t_count = measure;
        scene.beginFrame(frame);
        mutateWorld(w, frame);
        ex.extract(w.registry, scene);
        // Churn through the API as well: add 8, remove 8 (slots recycle after beginFrame).
        for (u32 i = 0; i < 8; ++i) {
            InstanceDesc d{};
            d.transform = translation(static_cast<f32>(frame), static_cast<f32>(i), 0);
            if (extra.size() < 64u) {
                extra.push_back(scene.addInstance(d));
            }
        }
        for (u32 i = 0; i < 8 && !extra.empty(); ++i) {
            scene.removeInstance(extra.back());
            extra.pop_back();
        }
        const GpuSceneCommitStats stats = scene.commit();
        t_count = false;
        expect(stats.ok, "commit ok");
        if (measure) {
            allocations += t_allocations;
        }
    }
    std::printf("zero_alloc: %llu operator-new calls over 64 steady-state frames (%u entities, CpuParallel, 2 workers)\n",
                allocations, kEntities);
    expect(allocations == 0u, "steady-state frames make no heap allocations");
    fuse::jobs::JobScheduler::instance().shutdown();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (suite == "table" || suite == "all") {
        rc |= runTable();
    }
    if (suite == "delta_scaling" || suite == "all") {
        rc |= runDeltaScaling();
    }
    if (suite == "extract" || suite == "all") {
        rc |= runExtract();
    }
    if (suite == "churn" || suite == "all") {
        rc |= runChurn();
    }
    if (suite == "meshlets" || suite == "all") {
        rc |= runMeshlets();
    }
    if (suite == "zero_alloc" || suite == "all") {
        rc |= runZeroAlloc();
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
