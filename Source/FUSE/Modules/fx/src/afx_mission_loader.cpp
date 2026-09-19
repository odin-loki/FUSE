#include <fuse/fx/afx_mission_loader.hpp>

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

std::string camelToSnake(std::string_view camel) {
    std::string out;
    for (char ch : camel) {
        if (!out.empty() && std::isupper(static_cast<unsigned char>(ch))) {
            out.push_back('_');
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool appendHook(std::vector<AfxMissionHook>& hooks, const std::string& functionName) {
    if (functionName == "onSpellCast") {
        hooks.push_back({"AFXDemo_Minimal", "on_spell_cast", "fireball"});
        return true;
    }
    if (functionName == "onAmbientFx") {
        hooks.push_back({"AFXDemo_Day", "on_ambient_fx", "spark_burst"});
        return true;
    }
    if (functionName == "onImpactFx") {
        hooks.push_back({"AFXDemo_Minimal", "on_impact_fx", "fireball"});
        return true;
    }

    if (functionName.rfind("on", 0) != 0) {
        return false;
    }

    AfxMissionHook hook;
    hook.missionId = "mis_stub";
    hook.scriptHook = camelToSnake(functionName.substr(2));
    hook.spellId = hook.scriptHook;
    hooks.push_back(std::move(hook));
    return true;
}

} // namespace

bool load_afx_mission_hooks_from_mis(const std::string& misText,
                                     std::vector<AfxMissionHook>& outHooks,
                                     std::string* errorOut) {
    outHooks.clear();
    std::stringstream stream(misText);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.rfind("function ", 0) != 0) {
            continue;
        }

        std::string remainder = trim(line.substr(9));
        const std::size_t paren = remainder.find('(');
        if (paren == std::string::npos) {
            continue;
        }

        const std::string functionName = trim(remainder.substr(0, paren));
        if (!appendHook(outHooks, functionName)) {
            continue;
        }
    }

    if (outHooks.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no AFX mission hooks found in .mis text";
        }
        return false;
    }

    return true;
}

} // namespace fuse::fx
