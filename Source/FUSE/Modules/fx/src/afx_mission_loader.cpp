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

std::string effectIdForSimObjectName(const std::string& name) {
    if (name.find("Spark") != std::string::npos || name == "SparkEmitter") {
        return "spark_burst";
    }
    if (name.find("Muzzle") != std::string::npos || name == "MuzzleFlashEmitter") {
        return "muzzle_flash";
    }
    if (name.find("Fireball") != std::string::npos) {
        return "fireball";
    }
    return name;
}

bool appendSimObjectBody(AfxMissionBody& body, const std::string& name, const std::string& blockText) {
    for (const auto& existing : body.simObjectBodies) {
        if (existing.first == name) {
            return false;
        }
    }
    body.simObjectBodies.push_back({name, blockText});
    return true;
}

u32 parseNestedSimObjectsFromBodyImpl(const std::string& parentName,
                                      const std::string& bodyText,
                                      AfxMissionBody& outBody) {
    u32 nestedCount = 0;
    std::size_t searchPos = 0;
    while (searchPos < bodyText.size()) {
        const std::size_t simPos = bodyText.find("new SimObject(", searchPos);
        if (simPos == std::string::npos) {
            break;
        }

        const std::size_t paren = bodyText.find('(', simPos);
        const std::size_t close = bodyText.find(')', paren);
        if (paren == std::string::npos || close == std::string::npos) {
            break;
        }

        const std::string nestedName = trim(bodyText.substr(paren + 1, close - paren - 1));
        const std::size_t braceOpen = bodyText.find('{', close);
        const std::size_t braceClose = bodyText.find('}', braceOpen);
        if (braceOpen != std::string::npos && braceClose > braceOpen) {
            const std::string nestedBody = trim(bodyText.substr(braceOpen, braceClose - braceOpen + 1));
            outBody.nestedSimObjectBodies.push_back({parentName + "::" + nestedName, nestedBody});
            appendSimObjectName(outBody, nestedName);
            appendSimObjectBody(outBody, nestedName, nestedBody);
            ++nestedCount;
        }

        searchPos = close + 1;
    }
    return nestedCount;
}

std::string normalizeMissionHookName(std::string_view rawName) {
    std::string hook = camelToSnake(trim(std::string(rawName)));
    if (hook.rfind("on_", 0) == 0) {
        hook = hook.substr(3);
    }
    return hook;
}

std::string missionHookDispatchName(const std::string& hookName) {
    if (hookName.rfind("on_", 0) == 0) {
        return hookName;
    }
    return "on_" + hookName;
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
                const std::string simName = trim(line.substr(paren + 1, close - paren - 1));
                appendSimObjectName(outBody, simName);
                const std::size_t braceOpen = line.find('{', close);
                if (braceOpen != std::string::npos) {
                    std::string blockText = line.substr(braceOpen);
                    auto countDepth = [](std::string_view text) {
                        std::size_t depth = 0;
                        for (char ch : text) {
                            if (ch == '{') {
                                ++depth;
                            } else if (ch == '}' && depth > 0) {
                                --depth;
                            }
                        }
                        return depth;
                    };
                    std::size_t depth = countDepth(blockText);
                    while (depth > 0 && std::getline(stream, line)) {
                        blockText.push_back('\n');
                        blockText += line;
                        depth = countDepth(blockText);
                    }
                    const std::string trimmedBlock = trim(blockText);
                    appendSimObjectBody(outBody, simName, trimmedBlock);
                    parseNestedSimObjectsFromBodyImpl(simName, trimmedBlock, outBody);
                }
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

        if (line.rfind("schedule(", 0) == 0) {
            const std::size_t open = line.find('(');
            const std::size_t close = line.find(')', open);
            if (open != std::string::npos && close != std::string::npos) {
                const std::string args = trim(line.substr(open + 1, close - open - 1));
                const std::size_t comma = args.find(',');
                AfxMissionScheduleEntry entry;
                if (comma != std::string::npos) {
                    try {
                        entry.delayMs = static_cast<u32>(std::stoul(trim(args.substr(0, comma))));
                    } catch (...) {
                        entry.delayMs = 0;
                    }
                    entry.hookName = normalizeMissionHookName(trim(args.substr(comma + 1)));
                } else {
                    entry.hookName = normalizeMissionHookName(args);
                }
                if (!entry.hookName.empty()) {
                    outBody.scheduleEntries.push_back(std::move(entry));
                }
            }
            continue;
        }

        if (line.rfind("call(", 0) == 0) {
            const std::size_t open = line.find('(');
            const std::size_t close = line.find(')', open);
            if (open != std::string::npos && close != std::string::npos) {
                const std::string target = normalizeMissionHookName(trim(line.substr(open + 1, close - open - 1)));
                if (!target.empty()) {
                    outBody.callTargets.push_back(target);
                }
            }
        }
    }

    if (outBody.missionName.empty() && outBody.simObjectNames.empty() && outBody.missionInfoKeys.empty() &&
        outBody.scheduleEntries.empty() && outBody.callTargets.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no AFX mission body content found in .mis text";
        }
        return false;
    }

    return true;
}

u32 codegen_spells_from_mission_body(const AfxMissionBody& body,
                                     std::vector<AfxMissionBodySpell>& outSpells) {
    outSpells.clear();
    for (const auto& bodyPair : body.simObjectBodies) {
        const std::size_t spellPos = bodyPair.second.find("spellId");
        if (spellPos == std::string::npos) {
            continue;
        }
        const std::size_t eq = bodyPair.second.find('=', spellPos);
        if (eq == std::string::npos) {
            continue;
        }
        std::string value = trim(bodyPair.second.substr(eq + 1));
        const std::size_t semi = value.find(';');
        if (semi != std::string::npos) {
            value = trim(value.substr(0, semi));
        }
        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) {
            outSpells.push_back({bodyPair.first, value});
        }
    }
    for (const auto& nestedPair : body.nestedSimObjectBodies) {
        const std::size_t spellPos = nestedPair.second.find("spellId");
        if (spellPos == std::string::npos) {
            continue;
        }
        const std::size_t eq = nestedPair.second.find('=', spellPos);
        if (eq == std::string::npos) {
            continue;
        }
        std::string value = trim(nestedPair.second.substr(eq + 1));
        const std::size_t semi = value.find(';');
        if (semi != std::string::npos) {
            value = trim(value.substr(0, semi));
        }
        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) {
            outSpells.push_back({nestedPair.first, value});
        }
    }
    return static_cast<u32>(outSpells.size());
}

u32 codegen_effects_from_mission_body(const AfxMissionBody& body,
                                      std::vector<AfxMissionBodyEffect>& outEffects) {
    outEffects.clear();
    for (const std::string& simName : body.simObjectNames) {
        AfxMissionBodyEffect effect;
        effect.simObjectName = simName;
        effect.effectId = effectIdForSimObjectName(simName);
        outEffects.push_back(std::move(effect));
    }
    for (const auto& bodyPair : body.simObjectBodies) {
        bool found = false;
        for (AfxMissionBodyEffect& effect : outEffects) {
            if (effect.simObjectName == bodyPair.first) {
                if (bodyPair.second.find("spark") != std::string::npos) {
                    effect.effectId = "spark_burst";
                } else if (bodyPair.second.find("muzzle") != std::string::npos) {
                    effect.effectId = "muzzle_flash";
                }
                found = true;
                break;
            }
        }
        if (!found) {
            outEffects.push_back({bodyPair.first, effectIdForSimObjectName(bodyPair.first)});
        }
    }
    return static_cast<u32>(outEffects.size());
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
    std::vector<AfxMissionBodyEffect> bodyEffects;
    codegen_effects_from_mission_body(body, bodyEffects);
    std::vector<AfxMissionBodySpell> bodySpells;
    codegen_spells_from_mission_body(body, bodySpells);

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

    for (const AfxMissionBodyEffect& bodyEffect : bodyEffects) {
        if (bodyEffect.effectId == "spark_burst") {
            composer.registerEffect(EffectDescriptor::makeSparkBurst());
        } else if (bodyEffect.effectId == "muzzle_flash") {
            composer.registerEffect(EffectDescriptor::makeMuzzleFlash());
        } else if (bodyEffect.effectId == "fireball") {
            composer.registerSpell(SpellDescriptor::makeFireball());
        }
    }
    for (const AfxMissionBodySpell& bodySpell : bodySpells) {
        if (bodySpell.spellId == "fireball") {
            composer.registerSpell(SpellDescriptor::makeFireball());
        }
    }
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

    AfxMissionBody body;
    parse_afx_mission_body_from_mis(misText, body);

    bool dispatched = false;
    if (!body.scheduleEntries.empty()) {
        for (const AfxMissionScheduleEntry& entry : body.scheduleEntries) {
            const std::string hookName = missionHookDispatchName(entry.hookName);
            if (entry.delayMs > 0) {
                vm.scheduleDelayedDispatch(hookName, entry.delayMs);
                dispatched = true;
            } else {
                dispatched = vm.dispatch(hookName, composer) || dispatched;
            }
        }
    } else if (!body.callTargets.empty()) {
        for (const std::string& target : body.callTargets) {
            dispatched = vm.dispatch(missionHookDispatchName(target), composer) || dispatched;
        }
    } else {
        for (const AfxMissionHook& hook : vm.registeredHooks()) {
            dispatched = vm.dispatch(hook.scriptHook, composer) || dispatched;
        }
    }

    for (const auto& nestedPair : body.nestedSimObjectBodies) {
            const std::size_t hookPos = nestedPair.second.find("%hook");
            if (hookPos == std::string::npos) {
                continue;
            }
            const std::size_t eq = nestedPair.second.find('=', hookPos);
            if (eq == std::string::npos) {
                continue;
            }
            std::string hookName = trim(nestedPair.second.substr(eq + 1));
            if (!hookName.empty() && hookName.front() == '"' && hookName.back() == '"') {
                hookName = hookName.substr(1, hookName.size() - 2);
            }
            if (!hookName.empty()) {
                dispatched = vm.dispatch(hookName, composer) || dispatched;
            }
        }

    return dispatched;
}

u32 parse_afx_mission_functions_from_mis(const std::string& misText,
                                         std::vector<AfxMissionFunctionBody>& outFunctions) {
    outFunctions.clear();
    std::stringstream stream(misText);
    std::string line;
    AfxMissionFunctionBody current{};

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.rfind("function ", 0) == 0) {
            if (!current.functionName.empty()) {
                outFunctions.push_back(current);
            }
            current = AfxMissionFunctionBody{};
            std::string remainder = trim(line.substr(9));
            const std::size_t paren = remainder.find('(');
            if (paren != std::string::npos) {
                current.functionName = trim(remainder.substr(0, paren));
            }
            const std::size_t braceOpen = line.find('{');
            if (braceOpen != std::string::npos) {
                current.bodyText = trim(line.substr(braceOpen + 1));
            }
            continue;
        }

        if (!current.functionName.empty()) {
            if (line == "};" || line == "}") {
                outFunctions.push_back(current);
                current = AfxMissionFunctionBody{};
                continue;
            }
            if (!line.empty()) {
                if (!current.bodyText.empty()) {
                    current.bodyText.push_back('\n');
                }
                current.bodyText += line;
            }
        }
    }

    if (!current.functionName.empty()) {
        outFunctions.push_back(current);
    }

    return static_cast<u32>(outFunctions.size());
}

bool execute_afx_mission_from_mis(const std::string& misText, FxComposer& composer, AfxMissionScriptVm& vm,
                                  std::string* errorOut) {
    if (!register_afx_mission_from_mis(misText, composer, vm, errorOut)) {
        return false;
    }

    std::vector<AfxMissionFunctionBody> functions;
    parse_afx_mission_functions_from_mis(misText, functions);
    bool executed = false;
    for (const AfxMissionFunctionBody& function : functions) {
        executed = vm.executeFunctionBody(function.functionName, function.bodyText, composer) || executed;
    }

    if (!executed) {
        executed = dispatch_afx_mission_from_mis(misText, composer, vm, errorOut);
    }

    return executed;
}

} // namespace fuse::fx
