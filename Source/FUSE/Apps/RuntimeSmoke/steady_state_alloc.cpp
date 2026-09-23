// Engine-wide steady-state heap audit (FUSE_MASTER_PLAN B1.8: "Zero heap allocations (new/malloc)
// anywhere in engine code — verified via overloaded operators").
//
// Drives one representative engine frame per iteration — the same subsystems a game loop runs —
// and counts every heap allocation made on ANY thread (game thread + job workers) during the
// measured steady-state frames:
//
//   input      InputState::beginFrame + synthetic key/mouse events
//   profiler   profiler::beginFrame / ProfileScope / endFrame
//   jobs       parallel_for over 4096 items through the fiber job system
//   frame-mem  FrameAllocator per-frame allocations + advanceFrame
//   scene      SceneManager::update (TransformSystem, CameraSystem, spatial BVH refit) + buildFrame
//   hybrid     HybridComposer::tick (World2D/World3D snapshot + parallel cull) + render
//   physics    physics-enabled World3D + World2D tick (PhysicsPipeline step)
//   animation  Animator::tick (clip sample, FK, skinning palette)
//   audio      AudioEngine::update (spatial mix of looping sources)
//   vfx        ParticleSystem::update (continuous emitters)
//   log        one fuse::log::info line per frame
//
// Global operator new/new[] (all overloads) and, on glibc, the C malloc family are replaced with
// counters. While measuring, every allocation's call stack is captured (execinfo backtrace) into
// a fixed table so the report can name the top allocation sites (file:line via addr2line).
//
// Enforcement: phases listed in kEnforcedPhases must be allocation-free (the test fails
// otherwise). The remaining phases are reported with their allocation sites; they are owned by
// other work streams (physics pipeline, logging ring, Vulkan upload) — see the plan row.

#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/animation/animator.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/audio/audio_engine.hpp>
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/profiler/profiler.hpp>
#include <fuse/scene/scene_manager.hpp>
#include <fuse/vfx/particle_system.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#define FUSE_STEADY_HAVE_MALLOC_HOOK 1
#include <dlfcn.h>
#include <execinfo.h>
extern "C" {
// Linker-provided bounds of this executable's text: the engine's static libraries live here.
extern char __executable_start;
extern char etext;
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);
}
#else
#define FUSE_STEADY_HAVE_MALLOC_HOOK 0
#endif

// ---- allocation recorder ------------------------------------------------------------------------

namespace {

constexpr int kStackDepth = 32;
constexpr int kSkipFrames = 2; // record() + the replaced allocation function
constexpr unsigned kSiteTableSize = 4096;

enum class Kind : unsigned char { CxxNew = 0, CMalloc = 1 };

struct Site {
    std::atomic<std::uint64_t> hash{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> bytes{0};
    int phase = -1;
    Kind kind = Kind::CxxNew;
    int depth = 0;
    void* frames[kStackDepth] = {};
};

Site g_sites[kSiteTableSize];
std::atomic<unsigned> g_droppedSites{0};
std::atomic<bool> g_measuring{false};
std::atomic<int> g_phase{-1};
std::atomic<std::uint64_t> g_newCount{0};
std::atomic<std::uint64_t> g_mallocCount{0};
thread_local bool t_inHook = false;

constexpr int kMaxPhases = 16;
constexpr int kForeignPhase = 15; // allocations on non-engine threads (driver worker threads)
std::atomic<std::uint64_t> g_phaseNew[kMaxPhases];
std::atomic<std::uint64_t> g_phaseMalloc[kMaxPhases];

void record(Kind kind, std::size_t size) {
    if (!g_measuring.load(std::memory_order_relaxed) || t_inHook) {
        return;
    }
    t_inHook = true;
    int phase = g_phase.load(std::memory_order_relaxed);
#if FUSE_STEADY_HAVE_MALLOC_HOOK
    void* frames[kStackDepth + kSkipFrames] = {};
    const int n = ::backtrace(frames, kStackDepth + kSkipFrames);
    // A stack with no frame inside this executable belongs to a foreign thread (the Vulkan
    // driver's rasterizer/compiler threads): it is not engine code and runs asynchronously to the
    // phase markers, so it gets its own bucket.
    bool engineFrame = false;
    for (int i = kSkipFrames; i < n && !engineFrame; ++i) {
        const char* f = static_cast<const char*>(frames[i]);
        engineFrame = f >= &__executable_start && f < &etext;
    }
    if (!engineFrame) {
        phase = kForeignPhase;
    }
#endif
    (kind == Kind::CxxNew ? g_newCount : g_mallocCount).fetch_add(1u, std::memory_order_relaxed);
    if (phase >= 0 && phase < kMaxPhases) {
        (kind == Kind::CxxNew ? g_phaseNew : g_phaseMalloc)[phase].fetch_add(1u, std::memory_order_relaxed);
    }
#if FUSE_STEADY_HAVE_MALLOC_HOOK
    std::uint64_t h = 1469598103934665603ull ^ static_cast<std::uint64_t>(phase) ^
                      (static_cast<std::uint64_t>(kind) << 8);
    for (int i = kSkipFrames; i < n; ++i) {
        h = (h ^ reinterpret_cast<std::uintptr_t>(frames[i])) * 1099511628211ull;
    }
    h |= 1u; // 0 marks an empty slot
    for (unsigned probe = 0; probe < kSiteTableSize; ++probe) {
        Site& s = g_sites[(h + probe) & (kSiteTableSize - 1u)];
        std::uint64_t cur = s.hash.load(std::memory_order_acquire);
        if (cur == 0u) {
            if (s.hash.compare_exchange_strong(cur, h, std::memory_order_acq_rel)) {
                // This thread claimed the slot: it alone fills in the stack.
                s.phase = phase;
                s.kind = kind;
                const int depth = std::max(0, n - kSkipFrames);
                std::memcpy(s.frames, frames + kSkipFrames, sizeof(void*) * static_cast<std::size_t>(depth));
                s.depth = depth;
                cur = h;
            }
        }
        if (cur == h) {
            s.count.fetch_add(1u, std::memory_order_relaxed);
            s.bytes.fetch_add(size, std::memory_order_relaxed);
            t_inHook = false;
            return;
        }
    }
    g_droppedSites.fetch_add(1u, std::memory_order_relaxed);
#endif
    t_inHook = false;
}

void* rawMalloc(std::size_t size) {
#if FUSE_STEADY_HAVE_MALLOC_HOOK
    return __libc_malloc(size);
#else
    return std::malloc(size);
#endif
}

void* rawAligned(std::size_t align, std::size_t size) {
#if FUSE_STEADY_HAVE_MALLOC_HOOK
    return __libc_memalign(align, size);
#else
    return std::aligned_alloc(align, (size + align - 1u) / align * align);
#endif
}

void rawFree(void* p) {
#if FUSE_STEADY_HAVE_MALLOC_HOOK
    __libc_free(p);
#else
    std::free(p);
#endif
}

void* countedNew(std::size_t size) {
    record(Kind::CxxNew, size);
    if (void* p = rawMalloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* countedNewAligned(std::size_t size, std::align_val_t alignment) {
    record(Kind::CxxNew, size);
    if (void* p = rawAligned(static_cast<std::size_t>(alignment), size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t size) { return countedNew(size); }
void* operator new[](std::size_t size) { return countedNew(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    record(Kind::CxxNew, size);
    return rawMalloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t alignment) { return countedNewAligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return countedNewAligned(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    record(Kind::CxxNew, size);
    return rawAligned(static_cast<std::size_t>(alignment), size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* p) noexcept { rawFree(p); }
void operator delete[](void* p) noexcept { rawFree(p); }
void operator delete(void* p, std::size_t) noexcept { rawFree(p); }
void operator delete[](void* p, std::size_t) noexcept { rawFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { rawFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { rawFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { rawFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { rawFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { rawFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { rawFree(p); }

#if FUSE_STEADY_HAVE_MALLOC_HOOK
// C allocation family (third-party C code, libc, Lua, drivers). operator new above calls
// __libc_malloc directly, so a C++ allocation is counted exactly once.
extern "C" {
void* malloc(std::size_t size) {
    record(Kind::CMalloc, size);
    return __libc_malloc(size);
}
void* calloc(std::size_t n, std::size_t size) {
    record(Kind::CMalloc, n * size);
    return __libc_calloc(n, size);
}
void* realloc(void* p, std::size_t size) {
    record(Kind::CMalloc, size);
    return __libc_realloc(p, size);
}
void free(void* p) { __libc_free(p); }
void* memalign(std::size_t align, std::size_t size) {
    record(Kind::CMalloc, size);
    return __libc_memalign(align, size);
}
void* aligned_alloc(std::size_t align, std::size_t size) {
    record(Kind::CMalloc, size);
    return __libc_memalign(align, size);
}
int posix_memalign(void** out, std::size_t align, std::size_t size) {
    record(Kind::CMalloc, size);
    void* p = __libc_memalign(align, size);
    if (p == nullptr) {
        return 12; // ENOMEM
    }
    *out = p;
    return 0;
}
}
#endif

// ---- frame -------------------------------------------------------------------------------------

namespace {

namespace anim = fuse::animation;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

enum Phase : int {
    kInput = 0,
    kProfiler,
    kJobs,
    kFrameMem,
    kScene,
    kHybrid,
    kPhysics,
    kAnimation,
    kAudio,
    kVfx,
    kLog,
    kPhaseCount
};

const char* const kPhaseNames[kPhaseCount] = {"input",   "profiler", "jobs",      "frame-mem", "scene", "hybrid",
                                              "physics", "animation", "audio",    "vfx",       "log"};

/// Phases whose steady state is required to be allocation-free by this gate. Physics (being
/// de-allocated by the PhysicsPipeline work stream), hybrid render (Vulkan driver / upload) and
/// log (async-ring rewrite in flight) are reported, not enforced.
const char* phaseName(int phase) {
    if (phase == kForeignPhase) {
        return "driver-threads";
    }
    return phase >= 0 && phase < kPhaseCount ? kPhaseNames[phase] : "?";
}

constexpr Phase kEnforcedPhases[] = {kInput, kProfiler, kJobs, kFrameMem, kScene, kAnimation, kAudio, kVfx};

constexpr fuse::u32 kWarmupFrames = 32;
constexpr fuse::u32 kMeasuredFrames = 120;
constexpr float kDt = 1.f / 60.f;

struct Engine {
    fuse::platform::InputState input;
    fuse::alloc::FrameAllocator frameMemory{256u * 1024u, "steady.frame"};
    std::vector<fuse::u32> jobOut = std::vector<fuse::u32>(4096u, 0u);

    fuse::scene::SceneManager scene;
    fuse::ecs::SceneData sceneData;

    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D hudWorld2D;
    fuse::world3d::World3D hudWorld3D;
    std::vector<std::unique_ptr<fuse::SceneObject2D>> sprites;
    std::vector<std::unique_ptr<fuse::SceneObject3D>> objects;

    fuse::world3d::World3D physicsWorld3D;
    fuse::world2d::World2D physicsWorld2D;
    std::vector<std::unique_ptr<fuse::SceneObject3D>> bodies3D;
    std::vector<std::unique_ptr<fuse::SceneObject2D>> bodies2D;

    anim::Skeleton skeleton;
    anim::AnimationClip clip;
    anim::Animator animator;

    fuse::audio::AudioEngine audio;
    fuse::audio::AudioRegistry audioRegistry;

    fuse::vfx::ParticleSystem vfx;

    fuse::frame::FrameCtx ctx;
};

void setupScene(Engine& e) {
    fuse::scene::SceneManagerDesc desc{};
    desc.hasVoxels = false;
    e.scene.init(desc);
    std::mt19937 rng(7u);
    std::uniform_real_distribution<float> pos(-60.f, 60.f);
    auto& reg = e.scene.registry();
    for (fuse::u32 i = 0; i < 512u; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {pos(rng), pos(rng), pos(rng), 1.f};
        t.dirty = true;
        reg.add(id, t);
        if ((i & 1u) == 0u) {
            fuse::ecs::Mesh mesh{};
            mesh.index_count = 36;
            mesh.aabb_min = {-0.5f, -0.5f, -0.5f, 1.f};
            mesh.aabb_max = {0.5f, 0.5f, 0.5f, 1.f};
            reg.add(id, mesh);
        } else {
            fuse::ecs::SDFObject sdf{};
            sdf.params = {0.75f, 0.f, 0.f, 0.f};
            reg.add(id, sdf);
        }
    }
    const fuse::ecs::EntityID camera = e.scene.createCamera(70.f, true);
    fuse::ecs::Transform* t = reg.get<fuse::ecs::Transform>(camera);
    t->position = {0.f, 0.f, -80.f, 1.f};
    t->dirty = true;
}

void setupHybrid(Engine& e) {
    for (int i = 0; i < 48; ++i) {
        e.sprites.push_back(std::make_unique<fuse::SceneObject2D>("sprite" + std::to_string(i)));
        e.sprites.back()->setPosition(static_cast<float>(i * 4 - 96), static_cast<float>((i % 8) * 10 - 40));
        e.hudWorld2D.addSprite(e.sprites.back().get());
    }
    for (int i = 0; i < 24; ++i) {
        e.objects.push_back(std::make_unique<fuse::SceneObject3D>("object" + std::to_string(i)));
        e.objects.back()->setPosition(static_cast<float>(i), static_cast<float>(-i));
        e.objects.back()->setZ(static_cast<float>(i * 10 - 120));
        e.hudWorld3D.addObject(e.objects.back().get());
    }
    e.hudWorld3D.setClearColor(0.1f, 0.15f, 0.25f);
    e.composer.attachWorld2D(&e.hudWorld2D);
    e.composer.attachWorld3D(&e.hudWorld3D);
}

void setupPhysics(Engine& e) {
    constexpr int kGrid = 6;
    e.physicsWorld3D.setPhysicsEnabled(true);
    e.physicsWorld3D.physics().addStaticPlane({0.f, 1.f, 0.f}, 0.f);
    for (int i = 0; i < kGrid * kGrid * kGrid; ++i) {
        e.bodies3D.push_back(std::make_unique<fuse::SceneObject3D>("body" + std::to_string(i)));
        e.bodies3D.back()->setPosition(static_cast<float>(i % kGrid) * 0.9f,
                                       1.f + static_cast<float>((i / kGrid) % kGrid) * 0.9f);
        e.bodies3D.back()->setZ(static_cast<float>(i / (kGrid * kGrid)) * 0.9f);
        e.physicsWorld3D.addObject(e.bodies3D.back().get());
    }
    e.physicsWorld2D.setPhysicsEnabled(true);
    for (int i = 0; i < 200; ++i) {
        e.bodies2D.push_back(std::make_unique<fuse::SceneObject2D>("b2d" + std::to_string(i)));
        fuse::SceneObject2D& s = *e.bodies2D.back();
        s.setPhysicsEnabled(true);
        s.setPhysicsShape(fuse::PhysicsShape2D::Circle);
        s.setPhysicsRadius(0.5f);
        s.setCollisionLayer(1);
        s.setPosition(static_cast<float>(i % 20) * 0.9f, static_cast<float>(i / 20) * 0.9f);
        e.physicsWorld2D.addSprite(&s);
    }
}

void setupAnimation(Engine& e) {
    constexpr fuse::u32 kBones = 24;
    for (fuse::u32 i = 0; i < kBones; ++i) {
        anim::Bone bone{};
        std::snprintf(bone.name, sizeof(bone.name), "bone%u", i);
        bone.parent_index = static_cast<fuse::s32>(i) - 1;
        bone.local_transform = anim::mat4::identity();
        bone.local_transform.data[13] = 0.25f;
        e.skeleton.bones.push_back(bone);
    }
    e.skeleton.bone_count = kBones;
    e.clip.duration = 2.f;
    e.clip.looping = true;
    for (fuse::u32 b = 0; b < kBones; ++b) {
        anim::AnimationClip::BoneChannels ch{};
        ch.bone_index = b;
        ch.rotation.times = {0.f, 1.f, 2.f};
        ch.rotation.values_quat = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.3826834f, 0.f, 0.9238795f}, {0.f, 0.f, 0.f, 1.f}};
        ch.position.times = {0.f, 2.f};
        ch.position.values_vec3 = {{0.f, 0.25f, 0.f, 0.f}, {0.f, 0.5f, 0.f, 0.f}};
        e.clip.bone_channels.push_back(ch);
    }
    e.animator.state_machine = std::make_unique<anim::AnimStateMachine>();
    auto node = std::make_unique<anim::ClipNode>();
    node->clip = &e.clip;
    e.animator.state_machine->add_state("loop", std::move(node));
}

void setupAudio(Engine& e) {
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    e.audio.init(desc);
    std::vector<float> pcm(48000, 0.25f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clipHandle = e.audio.register_clip(std::move(clip));
    e.audioRegistry.set_listener(e.audioRegistry.create_entity());
    for (int i = 0; i < 8; ++i) {
        const fuse::audio::EntityId id = e.audioRegistry.create_entity();
        e.audioRegistry.set_position(id, fuse::audio::Vec3{static_cast<float>(i) - 4.f, 0.f, -5.f});
        fuse::audio::AudioSourceDesc sd;
        sd.clip = clipHandle;
        sd.spatial = (i & 1) == 0;
        sd.looping = true;
        fuse::audio::AudioSource* src = e.audioRegistry.add_source(id, sd);
        src->playing = true;
    }
}

void setupVfx(Engine& e) {
    e.vfx.init({});
    for (int i = 0; i < 4; ++i) {
        fuse::vfx::ParticleEmitterDesc desc{};
        desc.max_particles = 512;
        desc.emit_rate = 200.f;
        desc.lifetime_min = 0.5f;
        desc.lifetime_max = 1.5f;
        e.vfx.create_emitter(desc);
    }
}

struct PhaseScope {
    explicit PhaseScope(Phase p) { g_phase.store(p, std::memory_order_relaxed); }
    ~PhaseScope() { g_phase.store(-1, std::memory_order_relaxed); }
};

void runFrame(Engine& e, fuse::u32 frame) {
    e.ctx.frameIndex = frame + 1u;
    e.ctx.time = static_cast<float>(frame) * kDt;
    e.ctx.dt = kDt;
    {
        PhaseScope p(kInput);
        e.input.beginFrame();
        fuse::platform::PlatformEvent ev{};
        ev.type = (frame & 1u) != 0u ? fuse::platform::PlatformEventType::KeyDown
                                     : fuse::platform::PlatformEventType::KeyUp;
        ev.keyCode = 'W';
        e.input.apply(ev);
        ev.type = fuse::platform::PlatformEventType::RawMouseDelta;
        ev.mouseX = 3;
        ev.mouseY = -2;
        e.input.apply(ev);
    }
    {
        PhaseScope p(kProfiler);
        fuse::profiler::beginFrame();
        {
            fuse::profiler::ProfileScope scope("steady.frame");
            fuse::profiler::ProfileScope inner("steady.inner");
        }
        fuse::profiler::endFrame();
    }
    {
        PhaseScope p(kJobs);
        fuse::u32* out = e.jobOut.data();
        fuse::jobs::parallel_for(0u, 4096u, 256u, [out, frame](fuse::u32 i) { out[i] = i * 3u + frame; });
    }
    {
        PhaseScope p(kFrameMem);
        for (int i = 0; i < 64; ++i) {
            void* block = e.frameMemory.allocate(256u + static_cast<fuse::u32>(i), 16u);
            if (block != nullptr) {
                std::memset(block, i, 256u);
            }
        }
        e.frameMemory.advanceFrame();
    }
    {
        PhaseScope p(kScene);
        // Move a few entities so the transform system and BVH refit have work every frame.
        auto& reg = e.scene.registry();
        int moved = 0;
        reg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform& t) {
            if (moved++ < 32) {
                t.position.x += ((frame & 1u) != 0u) ? 0.01f : -0.01f;
                t.dirty = true;
            }
        });
        e.scene.update(kDt);
        e.scene.buildFrame(e.sceneData);
    }
    {
        PhaseScope p(kHybrid);
        e.composer.tick(e.ctx);
        e.composer.render(e.ctx);
    }
    {
        PhaseScope p(kPhysics);
        e.physicsWorld3D.tick(e.ctx);
        e.physicsWorld2D.tick(e.ctx);
    }
    {
        PhaseScope p(kAnimation);
        e.animator.tick(e.skeleton, e.ctx);
    }
    {
        PhaseScope p(kAudio);
        e.audio.update(e.audioRegistry, kDt);
    }
    {
        PhaseScope p(kVfx);
        e.vfx.update(kDt);
    }
    {
        PhaseScope p(kLog);
        fuse::log::info("steady frame %u", frame);
    }
}

// ---- report ------------------------------------------------------------------------------------

#if FUSE_STEADY_HAVE_MALLOC_HOOK
std::string symbolize(void* addr) {
    Dl_info info{};
    if (::dladdr(addr, &info) == 0 || info.dli_fname == nullptr) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", addr);
        return buf;
    }
    // Return addresses point after the call; step back one byte so addr2line names the call line.
    const auto offset = reinterpret_cast<std::uintptr_t>(addr) - reinterpret_cast<std::uintptr_t>(info.dli_fbase) - 1u;
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "addr2line -C -f -i -e '%s' 0x%lx 2>/dev/null", info.dli_fname,
                  static_cast<unsigned long>(offset));
    std::string out;
    if (FILE* pipe = ::popen(cmd, "r")) {
        char line[1024];
        std::string fn;
        std::string loc;
        if (std::fgets(line, sizeof(line), pipe) != nullptr) {
            fn = line;
        }
        if (std::fgets(line, sizeof(line), pipe) != nullptr) {
            loc = line;
        }
        ::pclose(pipe);
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
                s.pop_back();
            }
        };
        trim(fn);
        trim(loc);
        if (fn.size() > 110) {
            fn = fn.substr(0, 107) + "...";
        }
        if (!fn.empty() && fn != "??") {
            out = fn + "  @ " + loc;
        }
    }
    if (out.empty()) {
        const char* base = std::strrchr(info.dli_fname, '/');
        out = std::string(info.dli_sname != nullptr ? info.dli_sname : "??") + " (" +
              (base != nullptr ? base + 1 : info.dli_fname) + ")";
    }
    return out;
}

/// Frames inside the C++ runtime / allocator plumbing are skipped so the printed "site" is the
/// first engine (or third-party) frame that asked for memory.
bool isPlumbing(const std::string& sym) {
    static const char* const kSkip[] = {"operator new", "std::__new_allocator", "__gnu_cxx::new_allocator",
                                        "std::allocator", "std::allocator_traits", "std::_Vector_base",
                                        "std::__cxx11::basic_string<", "std::_Function_base",
                                        "std::_Hashtable_alloc", "std::__detail::_Hashtable_alloc",
                                        "std::__allocate_guarded", "std::_Sp_counted"};
    for (const char* k : kSkip) {
        if (sym.find(k) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void printTopSites(unsigned limit) {
    std::vector<const Site*> sites;
    for (const Site& s : g_sites) {
        if (s.hash.load() != 0 && s.count.load() != 0) {
            sites.push_back(&s);
        }
    }
    // Engine-thread sites first (those are actionable), then driver threads; by count within each.
    std::sort(sites.begin(), sites.end(), [](const Site* a, const Site* b) {
        const bool fa = a->phase == kForeignPhase;
        const bool fb = b->phase == kForeignPhase;
        return fa != fb ? fb : a->count.load() > b->count.load();
    });
    if (const char* only = std::getenv("FUSE_STEADY_PHASE")) {
        sites.erase(std::remove_if(sites.begin(), sites.end(),
                                   [only](const Site* s) { return std::strcmp(phaseName(s->phase), only) != 0; }),
                    sites.end());
    }
    std::printf("steady-state alloc: %zu distinct allocating call stacks (dropped %u)\n", sites.size(),
                g_droppedSites.load());
    unsigned printed = 0;
    for (const Site* s : sites) {
        if (printed++ >= limit) {
            break;
        }
        const double perFrame = static_cast<double>(s->count.load()) / kMeasuredFrames;
        std::printf("\n  #%u [%s] %s  %llu allocs (%.2f/frame), %llu bytes\n", printed,
                    phaseName(s->phase), s->kind == Kind::CxxNew ? "operator new" : "malloc",
                    static_cast<unsigned long long>(s->count.load()), perFrame,
                    static_cast<unsigned long long>(s->bytes.load()));
        int shown = 0;
        bool reachedEngine = false;
        for (int i = 0; i < s->depth && shown < 7; ++i) {
            const std::string sym = symbolize(s->frames[i]);
            if (!reachedEngine && isPlumbing(sym)) {
                continue;
            }
            reachedEngine = true;
            std::printf("      %s\n", sym.c_str());
            ++shown;
        }
    }
}
#endif

void measure() {
    Engine e;
    setupScene(e);
    setupHybrid(e);
    setupPhysics(e);
    setupAnimation(e);
    setupAudio(e);
    setupVfx(e);
    fuse::profiler::setEnabled(true);

#if FUSE_STEADY_HAVE_MALLOC_HOOK
    // Prime backtrace(): its first call loads libgcc_s (which allocates).
    void* prime[4];
    (void)::backtrace(prime, 4);
#endif

    for (fuse::u32 frame = 0; frame < kWarmupFrames; ++frame) {
        runFrame(e, frame);
    }
    g_measuring.store(true);
    for (fuse::u32 frame = kWarmupFrames; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
        runFrame(e, frame);
    }
    g_measuring.store(false);

    const std::uint64_t totalNew = g_newCount.load();
    const std::uint64_t totalMalloc = g_mallocCount.load();
    std::printf("steady-state alloc: %u job worker(s), %u warm-up + %u measured frames, malloc hook %s\n",
                fuse::jobs::JobScheduler::instance().workerCount(), kWarmupFrames, kMeasuredFrames,
                FUSE_STEADY_HAVE_MALLOC_HOOK ? "on" : "off");
    std::printf("steady-state alloc: %-10s %12s %12s %10s\n", "phase", "operator new", "malloc", "per frame");
    for (int p = 0; p < kPhaseCount; ++p) {
        const std::uint64_t n = g_phaseNew[p].load();
        const std::uint64_t m = g_phaseMalloc[p].load();
        bool enforced = false;
        for (Phase q : kEnforcedPhases) {
            enforced = enforced || q == p;
        }
        std::printf("steady-state alloc: %-10s %12llu %12llu %10.2f%s\n", kPhaseNames[p],
                    static_cast<unsigned long long>(n), static_cast<unsigned long long>(m),
                    static_cast<double>(n + m) / kMeasuredFrames, enforced ? "  [enforced]" : "  [reported]");
    }
    std::printf("steady-state alloc: %-10s %12llu %12llu %10.2f  [reported: non-engine threads, e.g. lavapipe]\n",
                phaseName(kForeignPhase), static_cast<unsigned long long>(g_phaseNew[kForeignPhase].load()),
                static_cast<unsigned long long>(g_phaseMalloc[kForeignPhase].load()),
                static_cast<double>(g_phaseNew[kForeignPhase].load() + g_phaseMalloc[kForeignPhase].load()) /
                    kMeasuredFrames);
    std::printf("steady-state alloc: TOTAL      %12llu %12llu %10.2f\n", static_cast<unsigned long long>(totalNew),
                static_cast<unsigned long long>(totalMalloc),
                static_cast<double>(totalNew + totalMalloc) / kMeasuredFrames);

#if FUSE_STEADY_HAVE_MALLOC_HOOK
    if (totalNew + totalMalloc != 0u) {
        printTopSites(std::getenv("FUSE_STEADY_TOP") != nullptr
                          ? static_cast<unsigned>(std::atoi(std::getenv("FUSE_STEADY_TOP")))
                          : 25u);
    }
#endif

    for (Phase p : kEnforcedPhases) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "steady-state '%s' phase performs zero heap allocations (new + malloc)",
                      kPhaseNames[p]);
        expectTrue(g_phaseNew[p].load() + g_phaseMalloc[p].load() == 0u, msg);
    }
    if (std::getenv("FUSE_STEADY_STRICT") != nullptr) {
        expectTrue(totalNew + totalMalloc == 0u, "FUSE_STEADY_STRICT: whole frame performs zero heap allocations");
    }
    expectTrue(e.animator.tick_count == kWarmupFrames + kMeasuredFrames, "animator ticked every frame");
    expectTrue(e.vfx.alive_particle_count() > 0u, "particles alive");
    e.audio.destroy();
    e.vfx.destroy();
    e.scene.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    measure();
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_runtime_steady_state_alloc: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_runtime_steady_state_alloc: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
