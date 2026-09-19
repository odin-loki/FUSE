#include <fuse/fx/afx_mission_script_vm.hpp>

#include <fuse/fx/spell_descriptor.hpp>

namespace fuse::fx {

void AfxMissionScriptVm::registerHooks(const std::vector<AfxMissionHook>& hooks) {
    for (const AfxMissionHook& hook : hooks) {
        m_hooks[hook.scriptHook] = hook;
    }
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

    return false;
}

bool AfxMissionScriptVm::dispatchTick(FxComposer& composer, const frame::FrameCtx& ctx) {
    bool dispatched = false;
    if (m_hooks.count("on_ambient_fx") != 0) {
        dispatched = dispatch("on_ambient_fx", composer, ctx) || dispatched;
    }
    if (m_hooks.count("on_spell_cast") != 0 && composer.castPipeline().activeCount() == 0u) {
        dispatched = dispatch("on_spell_cast", composer, ctx) || dispatched;
    }
    return dispatched;
}

bool registerAfxTemplateMissionVm(FxComposer& composer, AfxMissionScriptVm& vm) {
    std::vector<AfxMissionHook> hooks;
    if (!registerAfxTemplateMissionHooks(composer, &hooks)) {
        return false;
    }

    hooks.push_back({"AFXDemo_Minimal", "on_impact_fx", "muzzle_flash"});
    vm.registerHooks(hooks);
    return true;
}

} // namespace fuse::fx
