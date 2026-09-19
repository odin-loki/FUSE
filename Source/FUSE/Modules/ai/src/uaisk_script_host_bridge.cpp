#include <fuse/ai/uaisk_script_host_bridge.hpp>

namespace fuse::ai::uaisk {

namespace {

constexpr const char* kUaiskChunkPrefix = "uaisk:";

bool isUaiskChunk(const char* chunkName) {
    return chunkName != nullptr && std::string_view(chunkName).rfind(kUaiskChunkPrefix, 0) == 0;
}

std::string_view csModuleFromChunk(const char* chunkName) {
    const std::string_view chunk(chunkName);
    if (!isUaiskChunk(chunkName)) {
        return {};
    }
    return chunk.substr(std::char_traits<char>::length(kUaiskChunkPrefix));
}

} // namespace

ScriptHostBridge::ScriptHostBridge(script::ScriptHost& host, BehaviorRuntime& runtime)
    : m_host(host), m_runtime(runtime) {}

bool ScriptHostBridge::attach() {
    if (!m_host.is_initialized() && !m_host.init()) {
        return false;
    }

    m_callbackId = m_host.register_callback(script::ScriptEventKind::OnStart,
                                            [this](const script::ScriptCallbackContext& ctx) {
                                                (void)ctx;
                                                ++m_importCount;
                                            });
    return m_callbackId != script::kInvalidScriptCallback;
}

bool ScriptHostBridge::importCsModule(std::string_view csModule,
                                      const std::string& btText,
                                      std::string* errorOut) {
    const u32 profileId = treeProfileForModuleText(csModule, btText);
    if (!importTemplateAsset(btText, profileId, m_runtime, errorOut)) {
        return false;
    }

    const std::string chunkName = std::string(kUaiskChunkPrefix) + std::string(csModule);
    const script::ScriptLoadResult loadResult = m_host.load_string(btText.c_str(), chunkName.c_str());
    if (!loadResult.ok()) {
        if (errorOut != nullptr && loadResult.message != nullptr) {
            *errorOut = loadResult.message;
        }
        return false;
    }

    ++m_importCount;
    return true;
}

bool ScriptHostBridge::loadPatrolSquadViaHost(std::string* errorOut) {
    static const char* kPatrolSquadBt =
        "# UAISK patrol_squad template\n"
        "bb.condition.allies_in_radius threshold=8 loops=1\n"
        "bb.action.set_flag flag=1\n"
        "bb.condition.distance_less threshold=5\n"
        "bb.action.set_flag flag=0\n"
        "bb.sequence children=0,1\n"
        "bb.sequence children=2,3\n"
        "bb.selector children=4,5 hook=aiBehaviors.cs\n"
        "root=6\n";

    return importCsModule("aiBehaviors.cs", kPatrolSquadBt, errorOut);
}

} // namespace fuse::ai::uaisk
