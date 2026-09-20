#include <fuse/fx/afx_mission_script_vm.hpp>

#include <fuse/fx/afx_template_pack.hpp>
#include <fuse/fx/spell_descriptor.hpp>

namespace fuse::fx {

void AfxMissionScriptVm::registerHooks(const std::vector<AfxMissionHook>& hooks) {
    for (const AfxMissionHook& hook : hooks) {
        m_hooks[hook.scriptHook] = hook;
        if (hook.delayMs > 0) {
            scheduleDelayedDispatch(hook.scriptHook, hook.delayMs);
        }
    }
    m_registeredHooks = hooks;
}

const std::vector<AfxMissionHook>& AfxMissionScriptVm::registeredHooks() const {
    return m_registeredHooks;
}

bool AfxMissionScriptVm::dispatch(const std::string& scriptHook,
                                  FxComposer& composer,
                                  const frame::FrameCtx& ctx) {
    const auto it = m_hooks.find(scriptHook);
    if (it == m_hooks.end()) {
        return false;
    }

    const AfxMissionHook& hook = it->second;
    ++m_dispatchCount;
    m_lastHookDispatched = scriptHook;

    if (hook.scriptHook == "on_spell_cast") {
        CastBinding binding;
        binding.caster = fuse::Handle<fuse::Object>(1u, 1u);
        binding.target = fuse::Handle<fuse::Object>(2u, 1u);
        return composer.beginCast(hook.spellId.empty() ? "fireball" : hook.spellId, binding);
    }

    if (hook.scriptHook == "on_ambient_fx") {
        FxSocket socket;
        socket.kind = FxSocketKind::Sprite2D;
        socket.effectId = hook.spellId.empty() ? "spark_burst" : hook.spellId;
        (void)ctx;
        return composer.attach(socket);
    }

    if (hook.scriptHook == "on_impact_fx") {
        FxSocket socket;
        socket.kind = FxSocketKind::Shape3D;
        socket.effectId = hook.spellId.empty() ? "muzzle_flash" : hook.spellId;
        socket.owner = fuse::Handle<fuse::Object>(3u, 1u);
        (void)ctx;
        return composer.attach(socket);
    }

    if (hook.scriptHook == "on_tick") {
        composer.particlePoolGpu().syncFromCpu(composer.particlePool());
        composer.particlePoolGpu().cudaDispatchOrSkip(ctx);
        return true;
    }

    if (hook.scriptHook == "on_spell_ready") {
        CastBinding binding;
        binding.caster = fuse::Handle<fuse::Object>(4u, 1u);
        binding.target = fuse::Handle<fuse::Object>(5u, 1u);
        return composer.beginCast(hook.spellId.empty() ? "spark_burst" : hook.spellId, binding);
    }

    return false;
}

bool AfxMissionScriptVm::executeFunctionBody(const std::string& functionName, const std::string& bodyText,
                                             FxComposer& composer, const frame::FrameCtx& ctx) {
    if (bodyText.empty()) {
        return false;
    }

    ++m_executeCount;
    m_lastHookDispatched = functionName;

    if (bodyText.find("beginCast") != std::string::npos || bodyText.find("spell_cast") != std::string::npos) {
        return dispatch("on_spell_cast", composer, ctx);
    }
    if (bodyText.find("attachEffect") != std::string::npos || bodyText.find("ambient") != std::string::npos) {
        return dispatch("on_ambient_fx", composer, ctx);
    }
    if (bodyText.find("impact") != std::string::npos) {
        return dispatch("on_impact_fx", composer, ctx);
    }
    if (bodyText.find("onTick") != std::string::npos || bodyText.find("tick") != std::string::npos) {
        return dispatch("on_tick", composer, ctx);
    }

    return false;
}

void AfxMissionScriptVm::scheduleDelayedDispatch(const std::string& scriptHook, u32 delayMs) {
    if (delayMs == 0) {
        return;
    }
    AfxMissionDelayedDispatch delayed{};
    delayed.scriptHook = scriptHook;
    delayed.delayMs = delayMs;
    m_delayedDispatches.push_back(std::move(delayed));
}

u32 AfxMissionScriptVm::pendingDelayedCount() const {
    u32 pending = 0;
    for (const AfxMissionDelayedDispatch& delayed : m_delayedDispatches) {
        if (!delayed.fired) {
            ++pending;
        }
    }
    return pending;
}

u32 AfxMissionScriptVm::advanceDelayedDispatches(u32 deltaMs,
                                                 FxComposer& composer,
                                                 const frame::FrameCtx& ctx) {
    u32 fired = 0;
    for (AfxMissionDelayedDispatch& delayed : m_delayedDispatches) {
        if (delayed.fired) {
            continue;
        }
        delayed.elapsedMs += deltaMs;
        if (delayed.elapsedMs < delayed.delayMs) {
            continue;
        }
        if (dispatch(delayed.scriptHook, composer, ctx)) {
            delayed.fired = true;
            ++fired;
            ++m_delayedDispatchCount;
        }
    }
    return fired;
}

bool AfxMissionScriptVm::dispatchTick(FxComposer& composer, const frame::FrameCtx& ctx) {
    const u32 deltaMs = static_cast<u32>(ctx.dt * 1000.f);
    advanceDelayedDispatches(deltaMs, composer, ctx);

    bool dispatched = false;
    if (m_hooks.count("on_tick") != 0) {
        dispatched = dispatch("on_tick", composer, ctx) || dispatched;
        if (dispatched) {
            ++m_tickDispatchCount;
        }
    }
    if (m_hooks.count("on_ambient_fx") != 0) {
        dispatched = dispatch("on_ambient_fx", composer, ctx) || dispatched;
    }
    if (m_hooks.count("on_spell_cast") != 0 && composer.castPipeline().activeCount() == 0u) {
        dispatched = dispatch("on_spell_cast", composer, ctx) || dispatched;
    }
    return dispatched;
}

bool registerAfxTemplateMissionVm(FxComposer& composer, AfxMissionScriptVm& vm) {
    registerAfxTemplateSamplePack(composer);
    composer.registerDemoVerticalSlice();

    std::vector<AfxMissionHook> hooks;
    if (!registerAfxTemplateMissionHooks(composer, &hooks)) {
        return false;
    }

    hooks.push_back({"AFXDemo_Minimal", "on_impact_fx", "muzzle_flash"});
    hooks.push_back({"AFXDemo_Minimal", "on_tick", "spark_burst"});
    vm.registerHooks(hooks);
    return true;
}

} // namespace fuse::fx
