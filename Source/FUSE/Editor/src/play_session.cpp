#include <fuse/editor/play_session.hpp>

#include <fuse/editor/undo_stack.hpp>

#include <fuse/ecs/components/transform.hpp>

#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT
#include <fuse/config/cvar.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/io/file_time.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_hot_reload.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_system.hpp>
#include <fuse/script/script_vm.hpp>
#if defined(FUSE_EDITOR_HAS_SCRIPT_PHYSICS) && FUSE_EDITOR_HAS_SCRIPT_PHYSICS
#include <fuse/script/script_physics_bridge.hpp>
#endif
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#endif

#include <algorithm>
#include <utility>

namespace fuse::editor {

namespace {

std::vector<ecs::EntityID> aliveEntities(ecs::Registry& registry) {
    std::vector<ecs::EntityID> ids;
    registry.each_query<>([&ids](ecs::EntityID id) { ids.push_back(id); });
    std::sort(ids.begin(), ids.end(), [](ecs::EntityID lhs, ecs::EntityID rhs) {
        return lhs.index != rhs.index ? lhs.index < rhs.index : lhs.generation < rhs.generation;
    });
    return ids;
}

bool sameComponentLayout(const EntityComponentSet& lhs, const EntityComponentSet& rhs) {
    return std::apply(
        [&rhs](const auto&... left) {
            return std::apply(
                [&](const auto&... right) { return ((left.has_value() == right.has_value()) && ...); },
                rhs);
        },
        lhs);
}

/// Restores `live` to `snapshot`. When play changed only component values (same live entity ids,
/// same component sets, same archetypes) the values are written back in place so component
/// storage — and references into it — stays valid; any structural change (spawn, destroy,
/// add/remove component) falls back to replacing the whole registry with the snapshot.
void restoreRegistry(ecs::Registry& live, ecs::Registry& snapshot) {
    const std::vector<ecs::EntityID> liveIds = aliveEntities(live);
    const std::vector<ecs::EntityID> snapshotIds = aliveEntities(snapshot);

    bool inPlace = liveIds == snapshotIds && live.count() == snapshot.count() &&
                   live.archetype_count() == snapshot.archetype_count();
    std::vector<EntityComponentSet> saved;
    if (inPlace) {
        saved.reserve(snapshotIds.size());
        for (const ecs::EntityID id : snapshotIds) {
            saved.push_back(captureEntityComponents(snapshot, id));
            if (!sameComponentLayout(saved.back(), captureEntityComponents(live, id))) {
                inPlace = false;
                break;
            }
        }
    }

    if (!inPlace) {
        live = std::move(snapshot);
        return;
    }

    for (usize i = 0; i < snapshotIds.size(); ++i) {
        restoreEntityComponents(live, snapshotIds[i], saved[i]);
    }
}

} // namespace

#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT
struct PlaySession::PieScripting {
    /// A module preloaded from `scriptRoot / path` under its component path (hot-reloaded here;
    /// modules whose path resolves as given are watched by `hotReload`).
    struct RootedModule {
        std::string module;
        std::string file;
        u64 writeTimeNs = 0;
        u64 size = 0;
        bool direct = false; ///< reloaded by `hotReload` (file path == module name)
    };

    // Declaration order = reverse destruction order: system, runtime, then the VM.
    script::ScriptVM vm;
    script::ScriptRuntime runtime;
    script::ScriptSystem system;
#if defined(FUSE_EDITOR_HAS_SCRIPT_PHYSICS) && FUSE_EDITOR_HAS_SCRIPT_PHYSICS
    std::unique_ptr<script::PhysicsManagerScriptBackend> physicsBackend;
#endif
    script::ScriptHotReload hotReload;
    std::vector<RootedModule> rooted;
};
#else
struct PlaySession::PieScripting {};
#endif

PlaySession::PlaySession() = default;

PlaySession::~PlaySession() {
    stopScripting_();
}

PlayWorldSnapshot PlayWorldSnapshot::capture(EditorScene& editorScene) {
    PlayWorldSnapshot snapshot;
    editorScene.registry().each<ecs::Transform>(
        [&snapshot](ecs::EntityID id, const ecs::Transform& transform) {
            snapshot.entities.push_back({id, transform});
        });
    std::sort(snapshot.entities.begin(), snapshot.entities.end(),
              [](const std::pair<ecs::EntityID, ecs::Transform>& lhs,
                 const std::pair<ecs::EntityID, ecs::Transform>& rhs) {
                  if (lhs.first.index != rhs.first.index) {
                      return lhs.first.index < rhs.first.index;
                  }
                  return lhs.first.generation < rhs.first.generation;
              });
    return snapshot;
}

void PlayWorldSnapshot::apply(EditorScene& editorScene) const {
    for (const std::pair<ecs::EntityID, ecs::Transform>& entry : entities) {
        ecs::Transform* transform = editorScene.registry().get<ecs::Transform>(entry.first);
        if (transform != nullptr) {
            *transform = entry.second;
        }
    }
}

void PlaySession::start(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
                        PlayModePhysicsState& physics) {
    if (isActive()) {
        return;
    }

    captureDirtySnapshot_(editorScene, state);
    captureWorldSnapshot_(editorScene);
    m_registrySnapshot = editorScene.registry();
    m_hasRegistrySnapshot = true;
    m_controller.enterPlay(scene, physics);
    // Fresh physics world per session: its body mapping is tied to the play registry, which Stop
    // replaces with the edit-time snapshot.
    m_physicsWorld.destroy();
    m_physicsWorldLive = false;
    if (physics.drivePhysics && !physics.stepHook) {
        m_physicsWorld.init(physics.desc);
        m_physicsWorldLive = true;
    }
    // GAP-GAME-LOOP-ECS: PIE runs the runtime frame schedule (input -> scripts -> animation ->
    // physics -> transform -> camera -> audio -> VFX -> extract) over the play registry. Physics is
    // this session's manager (or the state's step hook). Transform / Camera are off in PIE: the
    // editor's Transform dirty flags are its play-time change tracking (coalesceTransformDirty_) and
    // the viewport derives matrices itself; TransformSystem would clear them every step.
    {
        world3d::RuntimeScheduleDesc desc{};
        desc.physics.maxBodies = 16; // the owned manager is unused (external / hook physics below)
        desc.physics.maxContacts = 64;
        desc.physics.maxConstraints = 64;
        desc.enabledStages &= ~(world3d::runtimeStageBit(world3d::RuntimeStage::Transform) |
                                world3d::runtimeStageBit(world3d::RuntimeStage::Camera));
        m_schedule.init(editorScene.registry(), desc);
        m_schedule.setExternalPhysics(&m_physicsWorld);
        m_schedule.setHook(world3d::RuntimeStage::Physics, &PlaySession::stepPhysicsStage_, this);
    }
    m_scriptContactDispatches = 0;
    m_scriptReloadCount = 0;
    m_manualStepCount = 0;
    startScripting_(editorScene);
    m_sessionTickCount = 0;
    m_tickAccumulator = 0.f;
    m_coalescedDirtyCount = 0;
    m_skippedInactiveTickCount = 0;
    m_skippedInactiveFixedStepCount = 0;
    m_lastDeferredFixedStepCount = 0;

    state.playing = true;
    state.paused = false;
}

void PlaySession::stop(EditorScene& editorScene, scene::Scene& scene, EditorState& state,
                       PlayModePhysicsState& physics) {
    if (!isActive()) {
        return;
    }

    m_controller.stop(scene, physics);
    // Behaviours first (on_destroy still sees the play registry), then the schedule and physics.
    stopScripting_();
    m_schedule.shutdown();
    m_physicsWorld.destroy();
    m_physicsWorldLive = false;
    if (m_hasRegistrySnapshot) {
        restoreRegistry(editorScene.registry(), m_registrySnapshot);
        m_registrySnapshot = ecs::Registry{};
        m_hasRegistrySnapshot = false;
    }
    restoreWorldSnapshot_(editorScene);
    restoreDirtySnapshot_(editorScene, state);
    m_sessionTickCount = 0;
    m_tickAccumulator = 0.f;
    m_coalescedDirtyCount = 0;
    m_skippedInactiveTickCount = 0;
    m_skippedInactiveFixedStepCount = 0;
    m_lastDeferredFixedStepCount = 0;
    m_hasWorldSnapshot = false;
    m_hasDirtySnapshot = false;

    state.playing = false;
    state.paused = false;
}

void PlaySession::pause(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics) {
    if (!m_controller.isPlaying()) {
        return;
    }

    m_controller.pause(scene, physics);
    state.paused = true;
}

void PlaySession::resume(scene::Scene& scene, EditorState& state, PlayModePhysicsState& physics) {
    if (!m_controller.isPaused()) {
        return;
    }

    m_controller.resume(scene, physics);
    state.paused = false;
}

void PlaySession::tick(f32 dt, EditorScene& editorScene, PlayModePhysicsState& physics) {
    tick_(dt, editorScene, physics, true);
}

void PlaySession::tick_(f32 dt, EditorScene& editorScene, PlayModePhysicsState& physics,
                        bool stepPhysics) {
    if (shouldSkipVariableTick(dt, physics)) {
        ++m_skippedInactiveTickCount;
        return;
    }

    m_tickAccumulator += dt;
    simulateStep_(editorScene, physics, stepPhysics ? dt : 0.f);
}

u32 PlaySession::consumeFixedSteps(f32 fixedDt, EditorScene& editorScene,
                                   PlayModePhysicsState& physics, u32 maxSteps) {
    if (shouldSkipFixedStepDrain(fixedDt, physics)) {
        ++m_skippedInactiveFixedStepCount;
        m_lastDeferredFixedStepCount = 0;
        return 0;
    }

    u32 steps = 0;
    while (m_tickAccumulator >= fixedDt) {
        if (maxSteps > 0 && steps >= maxSteps) {
            break;
        }

        m_tickAccumulator -= fixedDt;
        simulateStep_(editorScene, physics, fixedDt);
        ++steps;
    }

    m_lastDeferredFixedStepCount = pendingFixedStepCount(fixedDt);
    return steps;
}

u32 PlaySession::tickFixedStep(f32 dt, f32 fixedDt, EditorScene& editorScene,
                               PlayModePhysicsState& physics, u32 maxSteps) {
    tick_(dt, editorScene, physics, false);
    return consumeFixedSteps(fixedDt, editorScene, physics, maxSteps);
}

u32 PlaySession::pendingFixedStepCount(f32 fixedDt) const {
    if (fixedDt <= 0.f) {
        return 0;
    }

    return static_cast<u32>(m_tickAccumulator / fixedDt);
}

FixedStepPreflight PlaySession::preflightFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                                                    u32 maxSteps) const {
    FixedStepPreflight preflight{};
    preflight.maxSteps = maxSteps;

    if (shouldSkipFixedStepDrain(fixedDt, physics)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.pending = pendingFixedStepCount(fixedDt);
    if (maxSteps == 0) {
        preflight.allowed = preflight.pending;
        preflight.deferred = 0;
        return preflight;
    }

    preflight.allowed = preflight.pending < maxSteps ? preflight.pending : maxSteps;
    preflight.deferred = preflight.pending > preflight.allowed ? preflight.pending - preflight.allowed : 0;
    preflight.wouldCap = preflight.deferred > 0;
    return preflight;
}

TickFixedStepPreflight PlaySession::preflightTickFixedStep(f32 dt, f32 fixedDt,
                                                           const PlayModePhysicsState& physics,
                                                           u32 maxSteps) const {
    TickFixedStepPreflight preflight{};
    preflight.variableTickSkipped = shouldSkipVariableTick(dt, physics);
    preflight.fixedStep = preflightFixedSteps(fixedDt, physics, maxSteps);
    return preflight;
}

VariableTickPreflight PlaySession::preflightVariableTick(f32 dt,
                                                         const PlayModePhysicsState& physics) const {
    VariableTickPreflight preflight{};
    preflight.skipped = shouldSkipVariableTick(dt, physics);
    if (preflight.skipped) {
        return preflight;
    }

    preflight.wouldSimulate = true;
    preflight.wouldAdvanceAccumulator = dt > 0.f;
    return preflight;
}

bool PlaySession::hasPendingFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics) const {
    if (shouldSkipFixedStepDrain(fixedDt, physics)) {
        return false;
    }

    return pendingFixedStepCount(fixedDt) > 0;
}

bool PlaySession::canConsumeFixedSteps(f32 fixedDt, const PlayModePhysicsState& physics,
                                       u32 maxSteps) const {
    const FixedStepPreflight preflight = preflightFixedSteps(fixedDt, physics, maxSteps);
    return !preflight.skipped && preflight.allowed > 0;
}

f32 PlaySession::fixedAccumulatorRemainder(f32 fixedDt) const {
    if (fixedDt <= 0.f) {
        return 0.f;
    }

    return m_tickAccumulator - static_cast<f32>(pendingFixedStepCount(fixedDt)) * fixedDt;
}

DirtySnapshotPreflight PlaySession::preflightDirtySnapshot() const {
    DirtySnapshotPreflight preflight{};

    if (shouldSkipDirtySnapshotRestore()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.captured = true;
    preflight.sceneModified = m_dirtySnapshot.sceneModified;
    preflight.entityCount = static_cast<u32>(m_dirtySnapshot.transformDirty.size());
    preflight.dirtyEntityCount = dirtySnapshotDirtyEntityCount();
    return preflight;
}

WorldSnapshotPreflight PlaySession::preflightWorldSnapshot() const {
    WorldSnapshotPreflight preflight{};

    if (shouldSkipWorldSnapshotRestore()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.captured = true;
    preflight.entityCount = static_cast<u32>(m_worldSnapshot.entities.size());
    return preflight;
}

bool PlaySession::shouldSkipVariableTick(f32 dt, const PlayModePhysicsState& physics) const {
    return !m_controller.isPlaying() || !physics.simulationActive || dt <= 0.f;
}

bool PlaySession::shouldSkipFixedStepDrain(f32 fixedDt, const PlayModePhysicsState& physics) const {
    return !m_controller.isPlaying() || !physics.simulationActive || fixedDt <= 0.f;
}

DirtySnapshotInfo PlaySession::dirtySnapshotInfo() const {
    DirtySnapshotInfo info{};
    info.captured = m_hasDirtySnapshot;
    if (!m_hasDirtySnapshot) {
        return info;
    }

    info.sceneModified = m_dirtySnapshot.sceneModified;
    info.entityCount = static_cast<u32>(m_dirtySnapshot.transformDirty.size());
    info.dirtyEntityCount = dirtySnapshotDirtyEntityCount();
    return info;
}

ecs::EntityID PlaySession::dirtySnapshotEntityAt(usize index) const {
    if (!m_hasDirtySnapshot || index >= m_dirtySnapshot.transformDirty.size()) {
        return ecs::EntityID{};
    }

    return m_dirtySnapshot.transformDirty[index].first;
}

bool PlaySession::transformDirtyAt(usize index) const {
    if (!m_hasDirtySnapshot || index >= m_dirtySnapshot.transformDirty.size()) {
        return false;
    }

    return m_dirtySnapshot.transformDirty[index].second;
}

bool PlaySession::transformDirtyForEntity(ecs::EntityID entityId) const {
    if (!m_hasDirtySnapshot || !entityId.valid()) {
        return false;
    }

    for (const std::pair<ecs::EntityID, bool>& entry : m_dirtySnapshot.transformDirty) {
        if (entry.first == entityId) {
            return entry.second;
        }
    }

    return false;
}

u32 PlaySession::dirtySnapshotDirtyEntityCount() const {
    if (!m_hasDirtySnapshot) {
        return 0;
    }

    u32 dirtyCount = 0;
    for (const std::pair<ecs::EntityID, bool>& entry : m_dirtySnapshot.transformDirty) {
        if (entry.second) {
            ++dirtyCount;
        }
    }

    return dirtyCount;
}

WorldSnapshotInfo PlaySession::worldSnapshotInfo() const {
    WorldSnapshotInfo info{};
    info.captured = m_hasWorldSnapshot;
    if (!m_hasWorldSnapshot) {
        return info;
    }

    info.entityCount = static_cast<u32>(m_worldSnapshot.entities.size());
    return info;
}

ecs::EntityID PlaySession::worldSnapshotEntityAt(usize index) const {
    if (!m_hasWorldSnapshot || index >= m_worldSnapshot.entities.size()) {
        return ecs::EntityID{};
    }

    return m_worldSnapshot.entities[index].first;
}

bool PlaySession::worldSnapshotContainsEntity(ecs::EntityID entityId) const {
    if (!m_hasWorldSnapshot || !entityId.valid()) {
        return false;
    }

    for (const std::pair<ecs::EntityID, ecs::Transform>& entry : m_worldSnapshot.entities) {
        if (entry.first == entityId) {
            return true;
        }
    }

    return false;
}

bool PlaySession::shouldSkipWorldSnapshotDrain() const {
    return !m_hasWorldSnapshot;
}

bool PlaySession::shouldSkipDirtySnapshotDrain() const {
    return !m_hasDirtySnapshot;
}

PlayWorldSnapshot PlaySession::captureWorldSnapshot(EditorScene& editorScene) const {
    return PlayWorldSnapshot::capture(editorScene);
}

void PlaySession::restoreWorldSnapshot(EditorScene& editorScene,
                                       const PlayWorldSnapshot& snapshot) const {
    if (snapshot.empty()) {
        return;
    }

    snapshot.apply(editorScene);
}

bool PlaySession::drainWorldSnapshot(EditorScene& editorScene) {
    if (shouldSkipWorldSnapshotDrain()) {
        return false;
    }

    m_worldSnapshot.apply(editorScene);
    m_hasWorldSnapshot = false;
    return true;
}

void PlaySession::restoreDirtyFlags(EditorScene& editorScene, EditorState& state) const {
    if (shouldSkipDirtySnapshotRestore()) {
        return;
    }

    restoreDirtySnapshot_(editorScene, state);
}

bool PlaySession::drainDirtySnapshot(EditorScene& editorScene, EditorState& state) {
    if (shouldSkipDirtySnapshotDrain()) {
        return false;
    }

    restoreDirtySnapshot_(editorScene, state);
    m_hasDirtySnapshot = false;
    return true;
}

void PlaySession::captureDirtySnapshot_(EditorScene& editorScene, const EditorState& state) {
    m_dirtySnapshot.sceneModified = state.sceneModified;
    m_dirtySnapshot.transformDirty.clear();

    editorScene.registry().each<ecs::Transform>([this](ecs::EntityID id, const ecs::Transform& transform) {
        m_dirtySnapshot.transformDirty.push_back({id, transform.dirty});
    });
    std::sort(m_dirtySnapshot.transformDirty.begin(), m_dirtySnapshot.transformDirty.end(),
              [](const std::pair<ecs::EntityID, bool>& lhs,
                 const std::pair<ecs::EntityID, bool>& rhs) {
                  if (lhs.first.index != rhs.first.index) {
                      return lhs.first.index < rhs.first.index;
                  }
                  return lhs.first.generation < rhs.first.generation;
              });
    m_hasDirtySnapshot = true;
}

void PlaySession::restoreDirtySnapshot_(EditorScene& editorScene, EditorState& state) const {
    if (!m_hasDirtySnapshot) {
        return;
    }

    state.sceneModified = m_dirtySnapshot.sceneModified;

    for (const std::pair<ecs::EntityID, bool>& entry : m_dirtySnapshot.transformDirty) {
        ecs::Transform* transform = editorScene.registry().get<ecs::Transform>(entry.first);
        if (transform != nullptr) {
            transform->dirty = entry.second;
        }
    }
}

void PlaySession::captureWorldSnapshot_(EditorScene& editorScene) {
    m_worldSnapshot = PlayWorldSnapshot::capture(editorScene);
    m_hasWorldSnapshot = true;
}

void PlaySession::restoreWorldSnapshot_(EditorScene& editorScene) const {
    if (!m_hasWorldSnapshot) {
        return;
    }

    m_worldSnapshot.apply(editorScene);
}

void PlaySession::simulateStep_(EditorScene& editorScene, PlayModePhysicsState& physics,
                                f32 physicsDt) {
    ++m_sessionTickCount;
    ++physics.stepCount;
    if (physicsDt > 0.f) {
        if (m_schedule.initialized() && m_schedule.registry() == &editorScene.registry()) {
            m_stepPhysicsState = &physics;
            m_schedule.step(physicsDt);
            m_stepPhysicsState = nullptr;
        } else if (physics.drivePhysics) {
            // Registry swapped under the session (not expected during play): physics only.
            if (physics.stepHook) {
                physics.stepHook(editorScene.registry(), physicsDt);
            } else if (m_physicsWorldLive) {
                m_physicsWorld.step(editorScene.registry(), physicsDt, m_physicsStreams);
            }
        }
    }
    coalesceTransformDirty_(editorScene);
}

void PlaySession::stepPhysicsStage_(void* user, ecs::Registry& registry, f32 dt) {
    PlaySession& session = *static_cast<PlaySession*>(user);
    PlayModePhysicsState* physics = session.m_stepPhysicsState;
    if (physics == nullptr || !physics->drivePhysics) {
        return;
    }
    if (physics->stepHook) {
        physics->stepHook(registry, dt);
    } else if (session.m_physicsWorldLive) {
        session.m_physicsWorld.step(registry, dt, session.m_physicsStreams);
#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT && defined(FUSE_EDITOR_HAS_SCRIPT_PHYSICS) && \
    FUSE_EDITOR_HAS_SCRIPT_PHYSICS
        // This step's contacts -> on_collision / on_trigger_enter on both entities.
        if (session.m_scripting != nullptr && !session.m_physicsWorld.lastEvents().empty()) {
            session.m_scriptContactDispatches +=
                script::dispatch_physics_events(session.m_physicsWorld.lastEvents(), session.m_scripting->system);
        }
#endif
    }
}

void PlaySession::scriptStage_(void* user, ecs::Registry& /*registry*/, f32 dt) {
#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT
    PlaySession& session = *static_cast<PlaySession*>(user);
    if (session.m_scripting != nullptr) {
        session.m_scripting->system.update(dt);
    }
#else
    (void)user;
    (void)dt;
#endif
}

bool PlaySession::stepPaused(f32 fixedDt, EditorScene& editorScene, PlayModePhysicsState& physics) {
    if (!isPaused() || fixedDt <= 0.f) {
        return false;
    }
    simulateStep_(editorScene, physics, fixedDt);
    ++m_manualStepCount;
    return true;
}

bool PlaySession::scriptsLive() const {
    return m_scripting != nullptr;
}

#if defined(FUSE_EDITOR_HAS_SCRIPT) && FUSE_EDITOR_HAS_SCRIPT

namespace {

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

u64 fileSizeOf(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
    return ec ? 0u : static_cast<u64>(size);
}

} // namespace

script::ScriptVM* PlaySession::scriptVm() {
    return m_scripting != nullptr ? &m_scripting->vm : nullptr;
}

script::ScriptRuntime* PlaySession::scriptRuntime() {
    return m_scripting != nullptr ? &m_scripting->runtime : nullptr;
}

script::ScriptSystem* PlaySession::scriptSystem() {
    return m_scripting != nullptr ? &m_scripting->system : nullptr;
}

usize PlaySession::scriptAttachedCount() const {
    return m_scripting != nullptr ? m_scripting->system.attached_count() : 0u;
}

void PlaySession::startScripting_(EditorScene& editorScene) {
    m_scriptError.clear();
    if (!m_scriptsEnabled || m_scripting != nullptr) {
        return;
    }
    ecs::Registry& registry = editorScene.registry();
    std::vector<std::string> modules;
    registry.each<ecs::Script>([&modules](ecs::EntityID, const ecs::Script& script) {
        const std::string path(script.path());
        if (!path.empty() && std::find(modules.begin(), modules.end(), path) == modules.end()) {
            modules.push_back(path);
        }
    });
    bool anyScript = !modules.empty();
    if (!anyScript) {
        registry.each<ecs::Script>([&anyScript](ecs::EntityID, const ecs::Script&) { anyScript = true; });
    }
    if (!anyScript) {
        return; // nothing to run: PIE stays script-free
    }

    auto scripting = std::make_unique<PieScripting>();
    script::ScriptVMDesc desc;
    desc.memory_limit_bytes = 64u * 1024u * 1024u;
    desc.instruction_budget = 10'000'000u; // per protected call: a runaway on_update cannot hang PIE
    if (!scripting->vm.init(desc) || !scripting->vm.has_lua_backend()) {
        m_scriptError = "PIE scripts disabled: no Lua backend in this build";
        return;
    }
    script::ScriptEngineBindings bindings;
    bindings.registry = &registry;
    bindings.cvars = &config::CVarRegistry::global();
#if defined(FUSE_EDITOR_HAS_SCRIPT_PHYSICS) && FUSE_EDITOR_HAS_SCRIPT_PHYSICS
    if (m_physicsWorldLive) {
        scripting->physicsBackend = std::make_unique<script::PhysicsManagerScriptBackend>(m_physicsWorld, registry);
        bindings.physics = scripting->physicsBackend.get();
    }
#endif
    if (!scripting->runtime.init(scripting->vm, bindings) || !scripting->system.init(registry, scripting->runtime)) {
        m_scriptError = "PIE scripts disabled: script runtime init failed";
        scripting->runtime.shutdown();
        return;
    }

    for (const std::string& module : modules) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::path(module), ec)) {
            scripting->hotReload.watch(module.c_str());
            scripting->rooted.push_back({module, module, io::fileWriteTimeNs(module), fileSizeOf(module), true});
            continue;
        }
        if (m_scriptRoot.empty()) {
            continue; // a module registered by name, or unresolved (ScriptSystem reports it)
        }
        const std::string rooted = (std::filesystem::path(m_scriptRoot) / module).lexically_normal().string();
        std::string source;
        if (!std::filesystem::is_regular_file(std::filesystem::path(rooted), ec) || !readWholeFile(rooted, source)) {
            continue;
        }
        const script::ScriptLoadResult loaded =
            scripting->runtime.load_module_buffer(module.c_str(), source.data(), source.size());
        if (!loaded.ok()) {
            m_scriptError = loaded.message != nullptr ? loaded.message : "script module load failed";
        }
        scripting->rooted.push_back({module, rooted, io::fileWriteTimeNs(rooted), fileSizeOf(rooted), false});
    }
    // Prime the watcher: this first poll loads every watched module (not counted as reloads).
    (void)scripting->hotReload.poll(scripting->runtime);

    m_scripting = std::move(scripting);
    m_scripting->system.enterPlay();
    m_scripting->system.sync();
    if (m_scripting->system.stats().load_failures > 0u && m_scriptError.empty()) {
        m_scriptError = m_scripting->system.last_error();
    }
    m_schedule.setHook(world3d::RuntimeStage::Scripts, &PlaySession::scriptStage_, this);
}

void PlaySession::stopScripting_() {
    if (m_scripting == nullptr) {
        return;
    }
    if (m_schedule.initialized()) {
        m_schedule.setHook(world3d::RuntimeStage::Scripts, nullptr, nullptr);
    }
    m_scripting->system.exitPlay();
    m_scripting->system.shutdown();
    m_scripting->runtime.shutdown();
    m_scripting->vm.shutdown();
    m_scripting.reset();
}

u32 PlaySession::pollScriptHotReload(std::vector<std::string>* reloaded) {
    if (m_scripting == nullptr) {
        return 0;
    }
    u32 count = 0;
    std::vector<const std::string*> directChanged;
    for (PieScripting::RootedModule& module : m_scripting->rooted) {
        const u64 writeTime = io::fileWriteTimeNs(module.file);
        const u64 size = fileSizeOf(module.file);
        if (writeTime == 0u || (writeTime == module.writeTimeNs && size == module.size)) {
            continue;
        }
        module.writeTimeNs = writeTime;
        module.size = size;
        if (module.direct) {
            directChanged.push_back(&module.module);
            continue;
        }
        std::string source;
        if (!readWholeFile(module.file, source)) {
            continue;
        }
        const script::ScriptLoadResult loaded =
            m_scripting->runtime.load_module_buffer(module.module.c_str(), source.data(), source.size());
        if (!loaded.ok()) {
            m_scriptError = loaded.message != nullptr ? loaded.message : "script reload failed";
            continue;
        }
        ++count;
        if (reloaded != nullptr) {
            reloaded->push_back(module.module);
        }
    }
    // Modules whose path resolves as given: script_hot_reload (content hash, so a touch without an
    // edit is not a reload).
    const usize failuresBefore = m_scripting->hotReload.failure_count();
    const usize directReloads = m_scripting->hotReload.poll(m_scripting->runtime);
    if (m_scripting->hotReload.failure_count() != failuresBefore) {
        m_scriptError = m_scripting->hotReload.last_error();
    }
    count += static_cast<u32>(directReloads);
    if (reloaded != nullptr && directReloads > 0u) {
        for (const std::string* module : directChanged) {
            reloaded->push_back(*module);
        }
    }
    m_scriptReloadCount += count;
    return count;
}

#else // !FUSE_EDITOR_HAS_SCRIPT

script::ScriptVM* PlaySession::scriptVm() {
    return nullptr;
}

script::ScriptRuntime* PlaySession::scriptRuntime() {
    return nullptr;
}

script::ScriptSystem* PlaySession::scriptSystem() {
    return nullptr;
}

usize PlaySession::scriptAttachedCount() const {
    return 0u;
}

void PlaySession::startScripting_(EditorScene& /*editorScene*/) {
    m_scriptError = "PIE scripts disabled: editor built without fuse_script";
}

void PlaySession::stopScripting_() {}

u32 PlaySession::pollScriptHotReload(std::vector<std::string>* /*reloaded*/) {
    return 0;
}

#endif

void PlaySession::coalesceTransformDirty_(EditorScene& editorScene) {
    editorScene.registry().each<ecs::Transform>([this](ecs::EntityID, ecs::Transform& transform) {
        if (transform.dirty) {
            ++m_coalescedDirtyCount;
            return;
        }

        transform.dirty = true;
    });
}

} // namespace fuse::editor
