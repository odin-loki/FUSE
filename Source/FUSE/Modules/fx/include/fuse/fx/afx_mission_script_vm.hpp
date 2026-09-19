#pragma once

#include <fuse/fx/afx_mission_hooks.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/frame/frame_ctx.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::fx {

/// AFX-Template mission script VM stub — dispatches `on_spell_cast` / `on_ambient_fx` hooks.
class AfxMissionScriptVm {
public:
    void registerHooks(const std::vector<AfxMissionHook>& hooks);

    bool dispatch(const std::string& scriptHook, FxComposer& composer, const frame::FrameCtx& ctx = {});

    u32 dispatchCount() const { return m_dispatchCount; }
    u32 hookCount() const { return static_cast<u32>(m_hooks.size()); }

private:
    std::unordered_map<std::string, AfxMissionHook> m_hooks;
    u32 m_dispatchCount = 0;
};

/// Register built-in AFX-Template mission hooks and wire the VM dispatch table.
bool registerAfxTemplateMissionVm(FxComposer& composer, AfxMissionScriptVm& vm);

} // namespace fuse::fx
