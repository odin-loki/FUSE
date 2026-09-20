#pragma once

#include <fuse/fx/afx_mission_hooks.hpp>
#include <fuse/fx/particle_pool_gpu.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/fx/particle_pool_gpu.hpp>
#include <fuse/frame/frame_ctx.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::fx {

/// AFX-Template mission script VM stub — dispatches `on_spell_cast` / `on_ambient_fx` / `on_impact_fx` hooks.
class AfxMissionScriptVm {
public:
    void registerHooks(const std::vector<AfxMissionHook>& hooks);

    bool dispatch(const std::string& scriptHook, FxComposer& composer, const frame::FrameCtx& ctx = {});
    bool dispatchTick(FxComposer& composer, const frame::FrameCtx& ctx = {});

    /// Execute a TorqueScript function body stub (attach/cast commands in body text).
    bool executeFunctionBody(const std::string& functionName, const std::string& bodyText, FxComposer& composer,
                             const frame::FrameCtx& ctx = {});

    u32 dispatchCount() const { return m_dispatchCount; }
    u32 tickDispatchCount() const { return m_tickDispatchCount; }
    u32 executeCount() const { return m_executeCount; }
    u32 hookCount() const { return static_cast<u32>(m_hooks.size()); }
    const std::vector<AfxMissionHook>& registeredHooks() const;
    const std::string& lastHookDispatched() const { return m_lastHookDispatched; }

private:
    std::unordered_map<std::string, AfxMissionHook> m_hooks;
    std::vector<AfxMissionHook> m_registeredHooks;
    u32 m_dispatchCount = 0;
    u32 m_tickDispatchCount = 0;
    u32 m_executeCount = 0;
    std::string m_lastHookDispatched;
};

/// Register built-in AFX-Template mission hooks and wire the VM dispatch table.
bool registerAfxTemplateMissionVm(FxComposer& composer, AfxMissionScriptVm& vm);

} // namespace fuse::fx
