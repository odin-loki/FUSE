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
//   script     Lua ScriptRuntime tick: PhysicsManager step + collision dispatch + on_update of 32
//              behaviours doing Entity.get/set_position, get_rotation/set_rotation_euler,
//              Physics.ray_cast/get_velocity/apply_impulse every frame (Lua heap on a FUSE pool)
//   net        server + client exchanging a snapshot delta (compute -> serialize -> send -> poll ->
//              deserialize -> apply -> checksum verify) and an ack every frame over localhost ENet
//              (LoopbackTransport when ENet is not built); ENet allocates from a FUSE pool
//   editor     headless EditorHost::gameTick over a 256-entity edit scene with 8 entities moved
//              per frame (command drain, runtime viewport mirror, headless GPU present)
//
// Global operator new/new[] (all overloads) and, on glibc, the C malloc family are replaced with
// counters. While measuring, every allocation's call stack is captured (execinfo backtrace) into
// a fixed table so the report can name the top allocation sites (file:line via addr2line).
//
// Attribution: an allocation is "FUSE" when the first frame above the allocator hook (skipping
// libc / libstdc++ / libgcc) lies in this executable — i.e. engine code or a library the engine
// statically links (Lua, ENet) asked for memory — and "driver" when it lies in another shared
// object (the Vulkan loader, lavapipe's libvulkan_lvp.so / LLVM). Allocations on threads with no
// engine frame at all (lavapipe rasterizer / compiler threads) form the driver-threads bucket.
//
// Enforcement (malloc hook available): every phase must make zero FUSE-attributed allocations.
// Driver-internal allocations (lavapipe allocating inside vkCmd* / vkQueueSubmit called from the
// hybrid and editor render paths, and its worker threads) are reported, not enforced: they are
// the software Vulkan driver's own heap use, outside engine control. Without the hook (ASan
// builds) the per-phase totals of the CPU-only phases are enforced.

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

#ifndef FUSE_STEADY_HAS_SCRIPT
#define FUSE_STEADY_HAS_SCRIPT 0
#endif
#ifndef FUSE_STEADY_HAS_NET
#define FUSE_STEADY_HAS_NET 0
#endif
#ifndef FUSE_STEADY_HAS_EDITOR
#define FUSE_STEADY_HAS_EDITOR 0
#endif

#if FUSE_STEADY_HAS_SCRIPT
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/script/script_physics_bridge.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_vm.hpp>
#endif
#if FUSE_STEADY_HAS_NET
#include <fuse/net/checksum.hpp>
#include <fuse/net/serializer.hpp>
#include <fuse/net/snapshot_delta.hpp>
#include <fuse/net/transport.hpp>
#endif
#if FUSE_STEADY_HAS_EDITOR
#include <fuse/editor/editor_host.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
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

// Only filled/read with the glibc malloc hook (backtrace attribution); unused elsewhere.
[[maybe_unused]] Site g_sites[kSiteTableSize];
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
#else
    (void)size;
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
#elif defined(_WIN32)
    // The Windows CRT has no aligned_alloc; _aligned_malloc blocks go back through rawAlignedFree.
    return _aligned_malloc(size == 0 ? 1 : size, align);
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

void rawAlignedFree(void* p) {
#if defined(_WIN32) && !FUSE_STEADY_HAVE_MALLOC_HOOK
    _aligned_free(p);
#else
    rawFree(p);
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
void operator delete(void* p, std::align_val_t) noexcept { rawAlignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { rawAlignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { rawAlignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { rawAlignedFree(p); }
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
    kScript,
    kNet,
    kEditor,
    kPhaseCount
};
static_assert(kPhaseCount <= kForeignPhase, "phase ids must stay below the driver-threads bucket");

const char* const kPhaseNames[kPhaseCount] = {"input", "profiler", "jobs",   "frame-mem", "scene",
                                              "hybrid", "physics", "animation", "audio",  "vfx",
                                              "log",   "script",  "net",       "editor"};

const char* phaseName(int phase) {
    if (phase == kForeignPhase) {
        return "driver-threads";
    }
    return phase >= 0 && phase < kPhaseCount ? kPhaseNames[phase] : "?";
}

/// Phases whose GPU work runs inside the (software) Vulkan driver on the engine thread. Without a
/// backtrace hook their driver-internal allocations cannot be told apart from engine ones, so
/// only the other phases are enforced in that mode.
bool phaseCallsVulkanDriver(int phase) { return phase == kHybrid || phase == kEditor; }

/// Per-phase allocation counts split by attribution (see the file comment).
struct PhaseSplit {
    std::uint64_t engine = 0;
    std::uint64_t driver = 0;
};

/// Whether a phase actually ran (its subsystem is built / came up in this configuration).
bool g_phaseActive[kPhaseCount] = {true, true, true, true, true, true, true, true, true, true, true,
                                   false, false, false};

// Warm-up covers one full bounce cycle of the script world's physics bodies (first ground contacts,
// kicks, landings) so every per-step buffer has reached its working size before measuring.
constexpr fuse::u32 kWarmupFrames = 120;
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

// Script / net / editor worlds are defined below; Engine holds them by pointer.
struct ScriptWorld;
struct NetWorld;
struct EditorWorld;

struct OptionalWorlds {
    ScriptWorld* script = nullptr;
    NetWorld* net = nullptr;
    EditorWorld* editor = nullptr;
};
OptionalWorlds g_worlds;

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

// ---- script -------------------------------------------------------------------------------------

#if FUSE_STEADY_HAS_SCRIPT
/// Lua behaviours on a physics-driven ECS world: every frame each actor queries its transform and
/// velocity, casts a ray at the ground, kicks itself up when resting and spins; movers write their
/// position along a circle. Every Entity/Physics call returns fresh Lua tables (GC'd garbage).
constexpr const char* kActorScript = R"lua(
function on_start(self)
    self.t = 0
    self.hits = 0
    self.contacts = 0
    self.kicks = 0
end

function on_update(self, dt)
    self.t = self.t + dt
    if not Entity.alive(self) then return end
    local p = Entity.get_position(self)
    local v = Physics.get_velocity(self)
    local hit = Physics.ray_cast({x = p.x, y = p.y + 2.0, z = p.z}, {x = 0, y = -1, z = 0}, 100)
    if hit ~= nil then
        self.hits = self.hits + 1
        self.ground = hit.distance
    end
    if v ~= nil and p.y < 0.56 and v.y <= 0.1 then
        Physics.apply_impulse(self, {x = 0, y = 3.0, z = 0})
        self.kicks = self.kicks + 1
    end
    local r = Entity.get_rotation(self)
    self.w = r.w
    Entity.set_rotation_euler(self, {x = 0, y = self.t * 45, z = 0})
end

function on_collision(self, other, point)
    self.contacts = self.contacts + 1
    self.last_contact_y = point.y
end
)lua";

constexpr const char* kMoverScript = R"lua(
function on_start(self)
    self.t = (self.entity % 7) * 0.5
end

function on_update(self, dt)
    self.t = self.t + dt
    local p = Entity.get_position(self)
    Entity.set_position(self, {x = math.cos(self.t) * 6, y = p.y, z = math.sin(self.t) * 6})
end
)lua";

struct ScriptWorld {
    fuse::ecs::Registry registry;
    fuse::physics::PhysicsManager physics;
    fuse::physics::PhysicsStreamManager streams{};
    std::unique_ptr<fuse::script::PhysicsManagerScriptBackend> backend;
    fuse::script::ScriptVM vm;
    fuse::script::ScriptRuntime runtime; // after vm: shut down (and released) first
    std::vector<fuse::ecs::EntityID> actors;
    std::vector<fuse::ecs::EntityID> movers;
};

fuse::ecs::EntityID spawnScriptBody(fuse::ecs::Registry& reg, float x, float y, float z, fuse::u32 shape,
                                    float radius, bool isStatic) {
    const fuse::ecs::EntityID id = reg.create();
    fuse::ecs::Transform t{};
    t.position = {x, y, z, 1.f};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.is_static = isStatic;
    rb.restitution = 0.2f;
    reg.add(id, rb);
    fuse::ecs::Collider c{};
    c.shape = shape;
    c.params = shape == fuse::ecs::Collider::Plane ? fuse::ecs::vec3{0.f, 1.f, 0.f, 0.f}
                                                   : fuse::ecs::vec3{radius, 0.f, 0.f, 0.f};
    reg.add(id, c);
    return id;
}

bool setupScript(ScriptWorld& s) {
    s.registry.init(256);
    (void)spawnScriptBody(s.registry, 0.f, 0.f, 0.f, fuse::ecs::Collider::Plane, 0.f, true);
    for (int i = 0; i < 24; ++i) {
        const float x = static_cast<float>(i % 6) * 1.5f - 4.f;
        const float z = static_cast<float>(i / 6) * 1.5f - 3.f;
        s.actors.push_back(spawnScriptBody(s.registry, x, 1.f + static_cast<float>(i % 3), z,
                                           fuse::ecs::Collider::Sphere, 0.5f, false));
    }
    for (int i = 0; i < 8; ++i) {
        const fuse::ecs::EntityID id = s.registry.create();
        fuse::ecs::Transform t{};
        t.position = {static_cast<float>(i), 4.f, 0.f, 1.f};
        s.registry.add(id, t);
        s.movers.push_back(id);
    }
    s.physics.init({});
    s.physics.step(s.registry, kDt, s.streams);
    s.backend = std::make_unique<fuse::script::PhysicsManagerScriptBackend>(s.physics, s.registry);

    fuse::script::ScriptVMDesc desc;
    desc.memory_limit_bytes = 32u * 1024u * 1024u;
    desc.instruction_budget = 5'000'000u;
    if (!s.vm.init(desc) || !s.vm.has_lua_backend()) {
        return false;
    }
    fuse::script::ScriptEngineBindings bindings;
    bindings.registry = &s.registry;
    bindings.physics = s.backend.get();
    if (!s.runtime.init(s.vm, bindings) || !s.runtime.load_module_source("actor", kActorScript).ok() ||
        !s.runtime.load_module_source("mover", kMoverScript).ok()) {
        return false;
    }
    for (fuse::ecs::EntityID id : s.actors) {
        s.runtime.attach(id, "actor");
    }
    for (fuse::ecs::EntityID id : s.movers) {
        s.runtime.attach(id, "mover");
    }
    return true;
}

void tickScript(ScriptWorld& s) {
    s.physics.step(s.registry, kDt, s.streams);
    fuse::script::dispatch_physics_events(s.physics.lastEvents(), s.runtime);
    s.runtime.update(kDt);
}

double scriptField(ScriptWorld& s, fuse::ecs::EntityID id, const char* field) {
    fuse::script::bind::ScriptValue value;
    if (!s.runtime.get_instance_field(id, field, value) || !fuse::script::bind::is_number(value)) {
        return -1.0;
    }
    return fuse::script::bind::to_number(value);
}
#endif

// ---- net ----------------------------------------------------------------------------------------

#if FUSE_STEADY_HAS_NET
constexpr fuse::u32 kNetEntities = 48; // < 64: rows replicate as masked entity patches

struct NetEntity {
    fuse::ecs::vec3 position{};
    fuse::ecs::quat rotation{};
    fuse::ecs::vec3 velocity{};
};

struct NetWorld {
    bool enet = false;
    std::unique_ptr<fuse::net::Transport> server;
    std::unique_ptr<fuse::net::Transport> client;
    fuse::u32 serverToClient = 0; // server-side peer id of the client
    fuse::u32 clientToServer = 0; // client-side peer id of the server

    NetEntity entities[kNetEntities];
    fuse::net::GameSnapshot serverSnap[2];
    fuse::u32 serverCur = 0;
    fuse::net::SnapshotDelta txDelta;
    fuse::net::SnapshotDeltaWorkspace txWorkspace;
    fuse::net::NetSerializer txBuffer;

    fuse::net::GameSnapshot clientSnap[2];
    fuse::u32 clientCur = 0;
    fuse::net::SnapshotDelta rxDelta;
    fuse::net::SnapshotDeltaWorkspace rxWorkspace;
    fuse::net::NetSerializer rxBuffer;
    fuse::net::NetSerializer ackBuffer;

    fuse::u32 frame = 0;
    fuse::u64 deltasSent = 0;
    fuse::u64 deltasApplied = 0;
    fuse::u64 patchDeltas = 0;
    fuse::u64 rejected = 0;
    fuse::u64 checksumFailures = 0;
    fuse::u64 acksReceived = 0;
    fuse::u32 lastAckFrame = 0;
};

void appendRaw(std::vector<fuse::net::byte>& out, const void* data, std::size_t n) {
    const std::size_t offset = out.size();
    out.resize(offset + n);
    std::memcpy(out.data() + offset, data, n);
}
void appendU32(std::vector<fuse::net::byte>& out, fuse::u32 v) { appendRaw(out, &v, sizeof(v)); }
void appendF32(std::vector<fuse::net::byte>& out, float v) { appendRaw(out, &v, sizeof(v)); }
void appendVec3(std::vector<fuse::net::byte>& out, const fuse::ecs::vec3& v) {
    appendF32(out, v.x);
    appendF32(out, v.y);
    appendF32(out, v.z);
}

/// Server snapshot in the fuse_net wire layout (ECS rows, then physics rows), capacity reused.
void buildNetSnapshot(NetWorld& n, fuse::net::GameSnapshot& snap, fuse::u32 frame) {
    snap.frame = frame;
    snap.ecs_state.clear();
    snap.physics_state.clear();
    for (fuse::u32 i = 0; i < kNetEntities; ++i) {
        const NetEntity& e = n.entities[i];
        appendU32(snap.ecs_state, i);
        appendU32(snap.ecs_state, 1u);
        appendVec3(snap.ecs_state, e.position);
        appendF32(snap.ecs_state, e.rotation.x);
        appendF32(snap.ecs_state, e.rotation.y);
        appendF32(snap.ecs_state, e.rotation.z);
        appendF32(snap.ecs_state, e.rotation.w);
        appendVec3(snap.ecs_state, {1.f, 1.f, 1.f, 0.f});
        appendU32(snap.physics_state, i);
        appendU32(snap.physics_state, 1u);
        appendVec3(snap.physics_state, e.velocity);
        appendVec3(snap.physics_state, {0.f, 0.f, 0.f, 0.f});
        appendF32(snap.physics_state, 1.f);
    }
    snap.checksum = fuse::net::compute_snapshot_checksum(snap);
}

void onClientPacket(NetWorld& n, const fuse::net::Packet& packet) {
    n.rxBuffer.buffer.assign(packet.data.begin(), packet.data.end());
    n.rxBuffer.reset_read();
    fuse::net::deserialize_snapshot_delta(n.rxBuffer, n.rxDelta, n.rxWorkspace);
    const fuse::net::GameSnapshot& base = n.clientSnap[n.clientCur];
    if (!fuse::net::can_apply_snapshot_delta(base, n.rxDelta)) {
        ++n.rejected;
        return;
    }
    fuse::net::GameSnapshot& next = n.clientSnap[n.clientCur ^ 1u];
    fuse::net::apply_snapshot_delta(base, n.rxDelta, next, n.rxWorkspace);
    if (!fuse::net::verify_snapshot_checksum(next)) {
        ++n.checksumFailures;
        return;
    }
    n.clientCur ^= 1u;
    ++n.deltasApplied;
    n.patchDeltas += n.rxDelta.kind == fuse::net::SnapshotDeltaKind::EntityPatch ? 1u : 0u;
    n.ackBuffer.clear();
    n.ackBuffer.write_u32(next.frame);
    (void)n.client->send(n.clientToServer, n.ackBuffer.buffer.data(), n.ackBuffer.buffer.size(),
                         fuse::net::PacketChannel::Unreliable);
}

void onServerPacket(NetWorld& n, const fuse::net::Packet& packet) {
    if (packet.data.size() >= sizeof(fuse::u32)) {
        std::memcpy(&n.lastAckFrame, packet.data.data(), sizeof(fuse::u32));
        ++n.acksReceived;
    }
}

bool connectENet(NetWorld& n) {
#if defined(FUSE_NET_HAS_ENET)
    auto server = std::make_unique<fuse::net::ENetTransport>();
    auto client = std::make_unique<fuse::net::ENetTransport>();
    if (!server->init(0) || !client->init(0)) {
        std::printf("steady-state alloc: net: ENet init failed (%s) - falling back to loopback\n",
                    server->last_error());
        return false;
    }
    fuse::net::ENetTransport* serverRaw = server.get();
    serverRaw->set_connection_callback([&n](fuse::u32 id, bool connected) {
        if (connected) {
            n.serverToClient = id;
        }
    });
    n.clientToServer = client->connect_peer("127.0.0.1", server->bound_port());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto noop = [](const fuse::net::Packet&) {};
    while (std::chrono::steady_clock::now() < deadline &&
           (n.serverToClient == 0u || !client->is_connected(n.clientToServer))) {
        server->poll(noop);
        client->poll(noop);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (n.serverToClient == 0u || !client->is_connected(n.clientToServer)) {
        std::printf("steady-state alloc: net: ENet localhost handshake timed out - falling back to loopback\n");
        return false;
    }
    n.server = std::move(server);
    n.client = std::move(client);
    return true;
#else
    (void)n;
    return false;
#endif
}

void setupNet(NetWorld& n) {
    for (fuse::u32 i = 0; i < kNetEntities; ++i) {
        n.entities[i].position = {static_cast<float>(i % 8) * 2.f, 0.f, static_cast<float>(i / 8) * 2.f, 0.f};
        n.entities[i].rotation = {0.f, 0.f, 0.f, 1.f};
        n.entities[i].velocity = {0.5f + 0.01f * static_cast<float>(i), 0.f, 0.25f, 0.f};
    }
    n.enet = connectENet(n);
    if (!n.enet) {
        auto server = std::make_unique<fuse::net::LoopbackTransport>();
        auto client = std::make_unique<fuse::net::LoopbackTransport>();
        server->init(1);
        client->init(2);
        fuse::net::LoopbackTransport::link_peers(*server, *client);
        n.serverToClient = 1;
        n.clientToServer = 1;
        n.server = std::move(server);
        n.client = std::move(client);
    }
    n.txWorkspace.reserve(kNetEntities);
    n.rxWorkspace.reserve(kNetEntities);
    buildNetSnapshot(n, n.serverSnap[0], 0u);
    n.clientSnap[0] = n.serverSnap[0];
}

void tickNet(NetWorld& n) {
    ++n.frame;
    // A quarter of the entities move each frame; every 8th one also changes velocity now and then.
    for (fuse::u32 i = 0; i < kNetEntities; ++i) {
        NetEntity& e = n.entities[i];
        if (((i + n.frame) & 3u) == 0u) {
            e.position.x += e.velocity.x * kDt;
            e.position.z += e.velocity.z * kDt;
        }
        if ((i & 7u) == 0u && (n.frame % 16u) == i / 8u) {
            e.velocity.x = -e.velocity.x;
        }
    }
    const fuse::u32 prev = n.serverCur;
    n.serverCur ^= 1u;
    buildNetSnapshot(n, n.serverSnap[n.serverCur], n.frame);
    fuse::net::compute_snapshot_delta(n.serverSnap[prev], n.serverSnap[n.serverCur], n.txDelta, n.txWorkspace);
    n.txBuffer.clear();
    fuse::net::serialize_snapshot_delta(n.txDelta, n.txBuffer);
    if (n.server->send(n.serverToClient, n.txBuffer.buffer.data(), n.txBuffer.buffer.size(),
                       fuse::net::PacketChannel::Reliable)) {
        ++n.deltasSent;
    }
    // Single-pointer captures fit std::function's small buffer: polling does not allocate.
    NetWorld* world = &n;
    n.server->poll([world](const fuse::net::Packet& p) { onServerPacket(*world, p); });
    n.client->poll([world](const fuse::net::Packet& p) { onClientPacket(*world, p); });
}
#endif

// ---- editor -------------------------------------------------------------------------------------

#if FUSE_STEADY_HAS_EDITOR
struct EditorWorld {
    std::unique_ptr<fuse::editor::EditorHost> host;
    std::vector<fuse::ecs::EntityID> moved;
};

void setupEditor(EditorWorld& w) {
    w.host = std::make_unique<fuse::editor::EditorHost>();
    fuse::ecs::Registry& reg = w.host->editorScene().registry();
    for (fuse::u32 i = 0; i < 256u; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        fuse::ecs::Transform t{};
        t.position = {static_cast<float>(i % 16) * 2.f, 0.f, static_cast<float>(i / 16) * 2.f, 1.f};
        reg.add(id, t);
        if ((i & 1u) == 0u) {
            fuse::ecs::SDFObject sdf{};
            sdf.type = fuse::ecs::SDFPrimitive::Sphere;
            sdf.params = {0.5f, 0.f, 0.f, 0.f};
            reg.add(id, sdf);
        } else {
            fuse::ecs::Mesh mesh{};
            mesh.aabb_min = {-0.5f, -0.5f, -0.5f, 0.f};
            mesh.aabb_max = {0.5f, 0.5f, 0.5f, 0.f};
            reg.add(id, mesh);
        }
        if (i % 32u == 0u) {
            w.moved.push_back(id);
        }
    }
    w.host->runtimeViewport().setProjectLabel("steady_state_alloc");
    w.host->runtimeViewport().requestResize(320, 180);
}

void tickEditor(EditorWorld& w, fuse::u32 frame) {
    // An entity being dragged in the viewport: its transform changes every frame.
    fuse::ecs::Registry& reg = w.host->editorScene().registry();
    for (fuse::ecs::EntityID id : w.moved) {
        if (fuse::ecs::Transform* t = reg.get<fuse::ecs::Transform>(id)) {
            t->position.y = ((frame & 1u) != 0u) ? 0.25f : 0.f;
            t->dirty = true;
        }
    }
    w.host->gameTick();
}
#endif

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
#if FUSE_STEADY_HAS_SCRIPT
    if (g_worlds.script != nullptr) {
        PhaseScope p(kScript);
        tickScript(*g_worlds.script);
    }
#endif
#if FUSE_STEADY_HAS_NET
    if (g_worlds.net != nullptr) {
        PhaseScope p(kNet);
        tickNet(*g_worlds.net);
    }
#endif
#if FUSE_STEADY_HAS_EDITOR
    if (g_worlds.editor != nullptr) {
        PhaseScope p(kEditor);
        tickEditor(*g_worlds.editor, frame);
    }
#endif
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

/// True for frames inside the C / C++ runtime (allocation plumbing, libc helpers like fopen).
bool isRuntimeLibrary(const char* path) {
    const char* base = std::strrchr(path, '/');
    base = base != nullptr ? base + 1 : path;
    static const char* const kRuntime[] = {"libc.so", "libc-", "libstdc++", "libgcc_s", "libm.so", "libm-",
                                           "ld-linux", "libpthread", "libdl.so"};
    for (const char* prefix : kRuntime) {
        if (std::strncmp(base, prefix, std::strlen(prefix)) == 0) {
            return true;
        }
    }
    return false;
}

bool inExecutable(void* addr) {
    const char* f = static_cast<const char*>(addr);
    return f >= &__executable_start && f < &etext;
}

/// Driver-internal: the first frame above the allocator hook (the hook's own frames are the
/// leading run inside this executable) that is not in the C/C++ runtime lies in another shared
/// object — the Vulkan loader or driver allocated, not engine code. See the file comment.
bool siteIsDriver(const Site& s) {
    if (s.phase == kForeignPhase) {
        return true;
    }
    int i = 0;
    while (i < s.depth && inExecutable(s.frames[i])) {
        ++i;
    }
    for (; i < s.depth; ++i) {
        void* addr = s.frames[i];
        if (inExecutable(addr)) {
            return false;
        }
        Dl_info info{};
        if (::dladdr(addr, &info) == 0 || info.dli_fname == nullptr) {
            return false; // unknown: attribute to the engine (conservative)
        }
        if (!isRuntimeLibrary(info.dli_fname)) {
            return true;
        }
    }
    return false;
}

void splitByAttribution(PhaseSplit (&out)[kMaxPhases]) {
    for (const Site& s : g_sites) {
        if (s.hash.load() == 0 || s.count.load() == 0 || s.phase < 0 || s.phase >= kMaxPhases) {
            continue;
        }
        (siteIsDriver(s) ? out[s.phase].driver : out[s.phase].engine) += s.count.load();
    }
}

void printTopSites(unsigned limit) {
    std::vector<const Site*> sites;
    for (const Site& s : g_sites) {
        if (s.hash.load() != 0 && s.count.load() != 0) {
            sites.push_back(&s);
        }
    }
    // FUSE-attributed sites first (those are actionable), then driver ones; by count within each.
    std::sort(sites.begin(), sites.end(), [](const Site* a, const Site* b) {
        const bool da = siteIsDriver(*a);
        const bool db = siteIsDriver(*b);
        return da != db ? db : a->count.load() > b->count.load();
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
        std::printf("\n  #%u [%s] %s %s  %llu allocs (%.2f/frame), %llu bytes\n", printed, phaseName(s->phase),
                    siteIsDriver(*s) ? "driver" : "FUSE", s->kind == Kind::CxxNew ? "operator new" : "malloc",
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

#if FUSE_STEADY_HAS_SCRIPT
    auto script = std::make_unique<ScriptWorld>();
    if (setupScript(*script)) {
        g_worlds.script = script.get();
        g_phaseActive[kScript] = true;
    } else {
        std::printf("steady-state alloc: script phase skipped (no Lua backend: %s)\n",
                    script->vm.last_error().c_str());
    }
#endif
#if FUSE_STEADY_HAS_NET
    auto net = std::make_unique<NetWorld>();
    setupNet(*net);
    g_worlds.net = net.get();
    g_phaseActive[kNet] = true;
#endif
#if FUSE_STEADY_HAS_EDITOR
    auto editor = std::make_unique<EditorWorld>();
    setupEditor(*editor);
    g_worlds.editor = editor.get();
    g_phaseActive[kEditor] = true;
#endif

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

    PhaseSplit split[kMaxPhases] = {};
#if FUSE_STEADY_HAVE_MALLOC_HOOK
    const bool attributed = g_droppedSites.load() == 0u;
    if (attributed) {
        splitByAttribution(split);
    }
#else
    const bool attributed = false;
#endif
    std::printf("steady-state alloc: %-10s %12s %12s %10s %8s %8s\n", "phase", "operator new", "malloc", "per frame",
                "FUSE", "driver");
    for (int p = 0; p < kPhaseCount; ++p) {
        const std::uint64_t n = g_phaseNew[p].load();
        const std::uint64_t m = g_phaseMalloc[p].load();
        const char* mode = !g_phaseActive[p] ? "  [not built/run]"
                           : attributed      ? "  [enforced: FUSE = 0; driver reported]"
                           : phaseCallsVulkanDriver(p) ? "  [reported: no attribution]"
                                                       : "  [enforced: total = 0]";
        if (attributed) {
            std::printf("steady-state alloc: %-10s %12llu %12llu %10.2f %8llu %8llu%s\n", kPhaseNames[p],
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(m),
                        static_cast<double>(n + m) / kMeasuredFrames, static_cast<unsigned long long>(split[p].engine),
                        static_cast<unsigned long long>(split[p].driver), mode);
        } else {
            std::printf("steady-state alloc: %-10s %12llu %12llu %10.2f %8s %8s%s\n", kPhaseNames[p],
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(m),
                        static_cast<double>(n + m) / kMeasuredFrames, "-", "-", mode);
        }
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

    for (int p = 0; p < kPhaseCount; ++p) {
        if (!g_phaseActive[p]) {
            continue;
        }
        const std::uint64_t total = g_phaseNew[p].load() + g_phaseMalloc[p].load();
        char msg[192];
        if (attributed) {
            std::snprintf(msg, sizeof(msg), "steady-state '%s' phase performs zero FUSE heap allocations (new + malloc)",
                          kPhaseNames[p]);
            expectTrue(split[p].engine == 0u, msg);
        } else if (!phaseCallsVulkanDriver(p)) {
            std::snprintf(msg, sizeof(msg), "steady-state '%s' phase performs zero heap allocations (new + malloc)",
                          kPhaseNames[p]);
            expectTrue(total == 0u, msg);
        }
    }
    if (std::getenv("FUSE_STEADY_STRICT") != nullptr) {
        expectTrue(totalNew + totalMalloc == 0u, "FUSE_STEADY_STRICT: whole frame performs zero heap allocations");
    }
    expectTrue(e.animator.tick_count == kWarmupFrames + kMeasuredFrames, "animator ticked every frame");
    expectTrue(e.vfx.alive_particle_count() > 0u, "particles alive");

    const fuse::u32 frames = kWarmupFrames + kMeasuredFrames;
#if FUSE_STEADY_HAS_SCRIPT
    if (g_worlds.script != nullptr) {
        ScriptWorld& sw = *g_worlds.script;
        double hits = 0.0;
        double kicks = 0.0;
        double contacts = 0.0;
        for (fuse::ecs::EntityID id : sw.actors) {
            hits += scriptField(sw, id, "hits");
            kicks += scriptField(sw, id, "kicks");
            contacts += scriptField(sw, id, "contacts");
        }
        std::printf("steady-state alloc: script: %zu behaviours, %llu callbacks, %.0f ray hits, %.0f impulses, "
                    "%.0f contacts, Lua heap %zu KiB (peak %zu KiB), errors %zu\n",
                    sw.runtime.instance_count(), static_cast<unsigned long long>(sw.runtime.callback_count()), hits,
                    kicks, contacts, sw.vm.memory_bytes() / 1024u, sw.vm.peak_memory_bytes() / 1024u,
                    sw.runtime.error_count());
        expectTrue(sw.runtime.frame_count() == frames, "script runtime ticked every frame");
        expectTrue(sw.runtime.error_count() == 0u, "script behaviours ran without errors");
        expectTrue(hits > 0.0 && kicks > 0.0, "scripts performed physics queries (ray hits, impulses)");
        if (sw.runtime.error_count() != 0u) {
            std::fprintf(stderr, "script error: %s\n", sw.runtime.last_error().c_str());
        }
    }
#endif
#if FUSE_STEADY_HAS_NET
    if (g_worlds.net != nullptr) {
        NetWorld& nw = *g_worlds.net;
        std::printf("steady-state alloc: net (%s): %llu deltas sent, %llu applied (%llu entity-patch), %llu rejected, "
                    "%llu checksum failures, %llu acks (last frame %u), last delta %zu bytes\n",
                    nw.enet ? "ENet localhost" : "loopback", static_cast<unsigned long long>(nw.deltasSent),
                    static_cast<unsigned long long>(nw.deltasApplied), static_cast<unsigned long long>(nw.patchDeltas),
                    static_cast<unsigned long long>(nw.rejected), static_cast<unsigned long long>(nw.checksumFailures),
                    static_cast<unsigned long long>(nw.acksReceived), nw.lastAckFrame, nw.txBuffer.buffer.size());
        expectTrue(nw.deltasSent == frames, "net: server sent a snapshot delta every frame");
        expectTrue(nw.deltasApplied + 2u >= frames, "net: client applied (nearly) every delta");
        expectTrue(nw.rejected == 0u && nw.checksumFailures == 0u, "net: every delta applied checksum-exact");
        expectTrue(nw.patchDeltas > 0u, "net: deltas replicate as entity patches");
        expectTrue(nw.acksReceived > 0u, "net: server received client acks");
    }
#endif
#if FUSE_STEADY_HAS_EDITOR
    if (g_worlds.editor != nullptr) {
        expectTrue(g_worlds.editor->host->gameTickCount() == frames, "editor ticked every frame");
    }
#endif

#if FUSE_STEADY_HAS_EDITOR
    g_worlds.editor = nullptr;
    editor.reset();
#endif
#if FUSE_STEADY_HAS_NET
    g_worlds.net = nullptr;
    net.reset();
#endif
#if FUSE_STEADY_HAS_SCRIPT
    g_worlds.script = nullptr;
    script.reset();
#endif
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
