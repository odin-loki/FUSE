#pragma once

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/uaisk_script_import.hpp>
#include <fuse/script/script_host.hpp>

#include <string>
#include <string_view>

namespace fuse::ai::uaisk {

/// Bridges UAISK `.cs` module names into fuse_ai tree profiles via `ScriptHost` load stubs.
class ScriptHostBridge {
public:
    ScriptHostBridge(script::ScriptHost& host, BehaviorRuntime& runtime);

    [[nodiscard]] script::ScriptHost& host() { return m_host; }
    [[nodiscard]] BehaviorRuntime& runtime() { return m_runtime; }
    [[nodiscard]] u32 importCount() const { return m_importCount; }

    /// Register OnStart callback that maps `uaisk:<module>.cs` chunk names to profiles.
    bool attach();

    /// Import a UAISK behavior-tree asset for the profile mapped from `csModule`.
    bool importCsModule(std::string_view csModule, const std::string& btText, std::string* errorOut = nullptr);

    /// Load embedded patrol_squad template through ScriptHost then register profile 1.
    bool loadPatrolSquadViaHost(std::string* errorOut = nullptr);

private:
    script::ScriptHost& m_host;
    BehaviorRuntime& m_runtime;
    script::ScriptCallbackId m_callbackId = script::kInvalidScriptCallback;
    u32 m_importCount = 0;
};

} // namespace fuse::ai::uaisk
