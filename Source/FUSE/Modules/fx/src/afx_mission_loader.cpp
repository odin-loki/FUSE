#include <fuse/fx/afx_mission_loader.hpp>

#include <fuse/fx/afx_mission_script_vm.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/fx_composer.hpp>
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

bool appendMissionInfoKey(AfxMissionBody& body, const std::string& key) {
    for (const std::string& existing : body.missionInfoKeys) {
        if (existing == key) {
            return false;
        }
    }
    body.missionInfoKeys.push_back(key);
    return true;
}

bool appendSimObjectName(AfxMissionBody& body, const std::string& name) {
    for (const std::string& existing : body.simObjectNames) {
        if (existing == name) {
            return false;
        }
    }
    body.simObjectNames.push_back(name);
    return true;
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
    if (functionName == "onTick") {
        hooks.push_back({"AFXDemo_Minimal", "on_tick", "spark_burst"});
        return true;
    }
    if (functionName == "onSpellReady") {
        hooks.push_back({"AFXDemo_Minimal", "on_spell_ready", "fireball"});
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

bool parse_afx_mission_body_from_mis(const std::string& misText, AfxMissionBody& outBody,
                                     std::string* errorOut) {
    outBody = AfxMissionBody{};
    std::stringstream stream(misText);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.rfind("//---", 0) == 0 && line.find("MISSION") != std::string::npos) {
            const std::size_t namePos = line.find("MISSION");
            if (namePos != std::string::npos) {
                std::string missionToken = trim(line.substr(namePos + 7));
                const std::size_t dash = missionToken.find("---");
                if (dash != std::string::npos) {
                    missionToken = trim(missionToken.substr(0, dash));
                }
                if (!missionToken.empty()) {
                    outBody.missionName = missionToken;
                }
            }
            continue;
        }

        if (line.rfind("new SimObject(", 0) == 0) {
            const std::size_t paren = line.find('(');
            const std::size_t close = line.find(')', paren);
            if (paren != std::string::npos && close != std::string::npos) {
                appendSimObjectName(outBody, trim(line.substr(paren + 1, close - paren - 1)));
            }
            continue;
        }

        if (line.rfind("MissionInfo", 0) == 0 || line.find("missionInfo") != std::string::npos) {
            const std::size_t dot = line.find('.');
            if (dot != std::string::npos) {
                const std::size_t eq = line.find('=', dot);
                if (eq != std::string::npos) {
                    appendMissionInfoKey(outBody, trim(line.substr(dot + 1, eq - dot - 1)));
                }
            }
            continue;
        }

        if (line.rfind("missionName", 0) == 0 || line.find("missionName") != std::string::npos) {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string value = trim(line.substr(eq + 1));
                while (!value.empty() && value.back() == ';') {
                    value.pop_back();
                }
                value = trim(value);
                if (!value.empty() && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                if (!value.empty()) {
                    outBody.missionName = value;
                }
            }
        }
    }

    if (outBody.missionName.empty() && outBody.simObjectNames.empty() && outBody.missionInfoKeys.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no AFX mission body content found in .mis text";
        }
        return false;
    }

    return true;
}

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

bool apply_afx_mission_spell_assignments(const std::string& misText, std::vector<AfxMissionHook>& hooks) {
    std::stringstream stream(misText);
    std::string line;
    bool changed = false;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.rfind("%", 0) != 0) {
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(1, eq - 1));
        std::string value = trim(line.substr(eq + 1));
        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (value.empty()) {
            continue;
        }

        for (AfxMissionHook& hook : hooks) {
            if (hook.scriptHook == key || hook.scriptHook == camelToSnake(key)) {
                hook.spellId = value;
                changed = true;
            }
        }
    }

    return changed;
}

bool register_afx_mission_from_mis(const std::string& misText, FxComposer& composer, AfxMissionScriptVm& vm,
                                   std::string* errorOut) {
    AfxMissionBody body;
    parse_afx_mission_body_from_mis(misText, body);

    std::vector<AfxMissionHook> hooks;
    if (!load_afx_mission_hooks_from_mis(misText, hooks, errorOut)) {
        return false;
    }

    if (!body.missionName.empty()) {
        for (AfxMissionHook& hook : hooks) {
            if (hook.missionId == "mis_stub") {
                hook.missionId = body.missionName;
            }
        }
    }

    apply_afx_mission_spell_assignments(misText, hooks);
    vm.registerHooks(hooks);

    composer.registerEffect(EffectDescriptor::makeSparkBurst());
    composer.registerEffect(EffectDescriptor::makeMuzzleFlash());
    composer.registerSpell(SpellDescriptor::makeFireball());
    return true;
}

bool dispatch_afx_mission_from_mis(const std::string& misText, FxComposer& composer, AfxMissionScriptVm& vm,
                                   std::string* errorOut) {
    if (!register_afx_mission_from_mis(misText, composer, vm, errorOut)) {
        return false;
    }

    bool dispatched = false;
    for (const AfxMissionHook& hook : vm.registeredHooks()) {
        dispatched = vm.dispatch(hook.scriptHook, composer) || dispatched;
    }
    return dispatched;
}

} // namespace fuse::fx
