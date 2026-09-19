#include <fuse/fx/afx_mission_hooks.hpp>

#include <fuse/fx/spell_descriptor.hpp>

#include <cctype>
#include <sstream>

namespace fuse::fx {

namespace {

std::string trim(const std::string& input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

} // namespace

bool load_afx_mission_hooks_from_text(const std::string& text,
                                      std::vector<AfxMissionHook>& outHooks,
                                      std::string* errorOut) {
    outHooks.clear();
    std::stringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::stringstream lineStream(line);
        std::string keyword;
        if (!(lineStream >> keyword) || keyword != "mission") {
            continue;
        }

        AfxMissionHook hook;
        if (!(lineStream >> hook.missionId)) {
            if (errorOut) {
                *errorOut = "mission line missing id";
            }
            return false;
        }

        std::string token;
        while (lineStream >> token) {
            const std::size_t eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            if (key == "hook") {
                hook.scriptHook = value;
            } else if (key == "spell") {
                hook.spellId = value;
            }
        }

        if (hook.missionId.empty() || hook.scriptHook.empty()) {
            if (errorOut) {
                *errorOut = "mission hook missing id or hook";
            }
            return false;
        }

        outHooks.push_back(std::move(hook));
    }

    return !outHooks.empty();
}

bool registerAfxTemplateMissionHooks(FxComposer& composer, std::vector<AfxMissionHook>* outHooks) {
    static const char* kMissionText =
        "# AFX-Template mission hooks (ore: AFXDemo_Minimal.mis)\n"
        "mission AFXDemo_Minimal hook=on_spell_cast spell=fireball\n"
        "mission AFXDemo_Day hook=on_ambient_fx spell=spark_burst\n";

    std::vector<AfxMissionHook> hooks;
    std::string error;
    if (!load_afx_mission_hooks_from_text(kMissionText, hooks, &error)) {
        return false;
    }

    composer.registerSpell(SpellDescriptor::makeFireball());
    if (outHooks != nullptr) {
        *outHooks = hooks;
    }
    return true;
}

} // namespace fuse::fx
