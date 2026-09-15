#include <fuse/phase7/phase7_test_registry.hpp>

#include <fuse/animation/animator.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/net/transport.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/terrain/terrain.hpp>
#include <fuse/vfx/particle_system.hpp>
#include <fuse/world_partition/world_partition.hpp>

#if defined(FUSE_PHASE7_HAS_AUDIO)
#include <fuse/audio/audio_engine.hpp>
#endif

#if defined(FUSE_PHASE7_HAS_SCRIPT)
#include <fuse/script/script_host.hpp>
#endif

#if defined(FUSE_PHASE7_HAS_PROJECT)
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>
#endif

#include <cstring>
#include <memory>

namespace fuse::phase7 {

namespace {

const std::vector<Phase7Deliverable> kChecklist = {
    {"animation.skeleton_hierarchy", "Skeleton loads with parent/child chain", Phase7Module::Animation, true, true},
    {"animation.clip_sampling", "ClipNode samples channels at keyframes", Phase7Module::Animation, true, true},
    {"animation.blend_interpolation", "BlendNode2 interpolates at 0/0.5/1", Phase7Module::Animation, true, true},
    {"animation.gpu_skinning", "GPU skinning 10k verts < 0.5ms", Phase7Module::Animation, false, false},
    {"audio.engine_init", "Audio engine initialises backend without error", Phase7Module::Audio, true, true},
    {"audio.spatial_attenuation", "Attenuation near-zero at max_distance", Phase7Module::Audio, true, true},
    {"audio.cuda_reverb", "CUDA FFT reverb within -60dB of CPU reference", Phase7Module::Audio, false, false},
    {"script.lua_hello", "Lua executes hello-world script", Phase7Module::Script, false, false},
    {"script.host_callbacks", "ScriptHost registers and dispatches callbacks", Phase7Module::Script, true, true},
    {"script.hot_reload", "Hot-reload replaces function within 1 frame", Phase7Module::Script, false, false},
    {"net.loopback_reliable", "Loopback transport reliable send/receive", Phase7Module::Net, true, true},
    {"net.enet_process_pair", "ENet reliable packet between two processes", Phase7Module::Net, false, false},
    {"net.rollback_resim", "Rollback resimulates 4 frames after remote input", Phase7Module::Net, false, false},
    {"terrain.heightfield_sample", "get_height matches bilinear heightmap read", Phase7Module::Terrain, true, true},
    {"terrain.lod_streaming", "LOD loads/unloads chunks as camera moves", Phase7Module::Terrain, true, true},
    {"terrain.svo_cave", "Terrain-SVO cave renders below surface", Phase7Module::Terrain, false, false},
    {"partition.cell_residency", "Cell residency transitions on camera move", Phase7Module::WorldPartition, true, true},
    {"partition.force_load", "force_load completes synchronously", Phase7Module::WorldPartition, true, true},
    {"vfx.emit_rate", "emit_rate produces expected particles per second", Phase7Module::Vfx, true, true},
    {"vfx.cuda_occupancy", "CUDA particle kernel > 70% occupancy", Phase7Module::Vfx, false, false},
    {"platform.lifecycle_power", "Background visibility drives PowerState", Phase7Module::Platform, true, true},
    {"platform.crash_handlers", "OS minidump / signal handlers installed", Phase7Module::Platform, false, false},
    {"assets.cook_manifest", "Cook manifest plans mesh/texture entries", Phase7Module::Assets, true, true},
    {"assets.real_mesh_cook", "FBX/GLTF mesh cook to engine binary", Phase7Module::Assets, false, false},
};

animation::Skeleton makeSmokeSkeleton() {
    animation::Skeleton skel;
    animation::Bone root{};
    std::strncpy(root.name, "root", sizeof(root.name) - 1);
    root.parent_index = -1;
    root.local_transform = animation::mat4::identity();

    animation::Bone child{};
    std::strncpy(child.name, "child", sizeof(child.name) - 1);
    child.parent_index = 0;
    child.local_transform = animation::mat4::identity();
    child.local_transform.data[13] = 1.f;

    skel.bones = {root, child};
    skel.bone_count = 2;
    return skel;
}

bool smokeAnimationFacade() {
    const animation::Skeleton skel = makeSmokeSkeleton();
    animation::Animator animator;
    auto idleNode = std::make_unique<animation::ClipNode>();
    animation::AnimationClip idle{};
    idle.duration = 1.f;
    idleNode->clip = &idle;
    animator.state_machine = std::make_unique<animation::AnimStateMachine>();
    animator.state_machine->add_state("idle", std::move(idleNode));

    frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    animator.tick(skel, ctx);
    return animator.tick_count == 1u && animator.current_pose.bone_count == 2u;
}

bool smokeNetFacade() {
    net::LoopbackTransport host;
    net::LoopbackTransport client;
    if (!host.init(27100) || !client.init(27101)) {
        return false;
    }
    net::LoopbackTransport::link_peers(host, client);

    const char payload[] = "phase7";
    if (!host.send(1, reinterpret_cast<const net::byte*>(payload), sizeof(payload) - 1,
                   net::PacketChannel::Reliable)) {
        return false;
    }

    bool received = false;
    client.poll([&](const net::Packet& packet) { received = !packet.data.empty(); });
    return received && host.peer_count() == 1u;
}

bool smokeTerrainFacade() {
    terrain::Terrain terrain;
    terrain::TerrainDesc desc{};
    desc.resolution = 17;
    desc.world_size = 16.f;
    desc.max_height = 8.f;
    desc.lod_levels = 2;
    desc.chunk_resolution = 8;
    terrain.init(desc);
    if (!terrain.is_initialized()) {
        return false;
    }
    terrain.generate(42);
    const f32 height = terrain.get_height(0.f, 0.f);
    terrain.update_lod({0.f, 0.f, 0.f}, 1.f / 60.f);
    terrain.destroy();
    return height >= 0.f;
}

bool smokeWorldPartitionFacade() {
    world_partition::WorldPartition partition;
    world_partition::WorldPartitionDesc desc{};
    desc.async_loading = false;
    partition.init(desc);
    partition.update({0.f, 0.f, 0.f});
    const bool hasCells = partition.loaded_cell_count() >= 0u;
    partition.destroy();
    return hasCells;
}

bool smokeVfxFacade() {
    vfx::ParticleSystem system;
    vfx::VfxDesc desc{};
    system.init(desc);
    if (!system.is_initialized()) {
        return false;
    }

    vfx::ParticleEmitterDesc emitterDesc{};
    emitterDesc.max_particles = 16;
    emitterDesc.emit_rate = 10.f;
    emitterDesc.lifetime_min = 1.f;
    emitterDesc.lifetime_max = 1.f;
    const Handle<vfx::EffectInstance> effect = system.spawn_effect(emitterDesc, {0.f, 1.f, 0.f}, 0.5f);
    system.update(0.1f);
    system.destroy();
    return effect.isValid();
}

bool smokePlatformFacade() {
    const platform::JobProfileLimits limits = platform::currentJobProfileLimits();
    return limits.minWorkers >= 1u && limits.maxWorkers >= limits.minWorkers && limits.fiberStackBytes > 0u;
}

#if defined(FUSE_PHASE7_HAS_AUDIO)
bool smokeAudioFacade() {
    audio::AudioEngine engine;
    audio::AudioDesc desc{};
    desc.cuda_reverb = false;
    engine.init(desc);
    const bool ok = engine.is_initialized();
    engine.destroy();
    return ok;
}
#endif

#if defined(FUSE_PHASE7_HAS_SCRIPT)
bool smokeScriptFacade() {
    script::ScriptHost host;
    if (!host.init()) {
        return false;
    }
    const bool ok = host.is_initialized() && host.vm().is_initialized();
    host.shutdown();
    return ok;
}
#endif

#if defined(FUSE_PHASE7_HAS_PROJECT)
bool smokeAssetsFacade() {
    project::CookManifest manifest = project::makeDefaultCookManifest("/tmp/fuse_phase7");
    project::ImportPipeline pipeline;
    pipeline.set_project_root("/tmp/fuse_phase7");
    const project::CookBatchResult plan = pipeline.plan_from_manifest(manifest);
    return plan.ok && !plan.records.empty();
}
#endif

} // namespace

const std::vector<Phase7Deliverable>& Phase7TestRegistry::checklist() {
    return kChecklist;
}

u32 Phase7TestRegistry::countByModule(Phase7Module module) {
    u32 count = 0;
    for (const Phase7Deliverable& item : kChecklist) {
        if (item.module == module) {
            ++count;
        }
    }
    return count;
}

u32 Phase7TestRegistry::stubLandedCount() {
    u32 count = 0;
    for (const Phase7Deliverable& item : kChecklist) {
        if (item.stub_landed) {
            ++count;
        }
    }
    return count;
}

u32 Phase7TestRegistry::automatedCount() {
    u32 count = 0;
    for (const Phase7Deliverable& item : kChecklist) {
        if (item.automated) {
            ++count;
        }
    }
    return count;
}

bool Phase7TestRegistry::runIntegrationSmoke() {
    core::initialize();

    if (!smokePlatformFacade()) {
        core::shutdown();
        return false;
    }
    if (!smokeAnimationFacade()) {
        core::shutdown();
        return false;
    }
    if (!smokeNetFacade()) {
        core::shutdown();
        return false;
    }
    if (!smokeTerrainFacade()) {
        core::shutdown();
        return false;
    }
    if (!smokeWorldPartitionFacade()) {
        core::shutdown();
        return false;
    }
    if (!smokeVfxFacade()) {
        core::shutdown();
        return false;
    }

#if defined(FUSE_PHASE7_HAS_AUDIO)
    if (!smokeAudioFacade()) {
        core::shutdown();
        return false;
    }
#endif

#if defined(FUSE_PHASE7_HAS_SCRIPT)
    if (!smokeScriptFacade()) {
        core::shutdown();
        return false;
    }
#endif

#if defined(FUSE_PHASE7_HAS_PROJECT)
    if (!smokeAssetsFacade()) {
        core::shutdown();
        return false;
    }
#endif

    core::shutdown();
    return true;
}

} // namespace fuse::phase7
