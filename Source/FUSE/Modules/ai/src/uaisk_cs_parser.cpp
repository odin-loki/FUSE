#include <fuse/ai/uaisk_cs_parser.hpp>

namespace fuse::ai::uaisk {

namespace {

std::string_view trimView(std::string_view input) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) {
        input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '\r')) {
        input.remove_suffix(1);
    }
    return input;
}

std::string registryTypeForModule(std::string_view csModule) {
    for (const TemplateHook& hook : kTemplateHooks) {
        if (hook.uaiskModule == csModule) {
            return std::string(hook.fuseRegistryTypeId);
        }
    }
    return {};
}

std::string extractQuotedValue(std::string_view line, std::string_view key) {
    const std::string needle = std::string(key) + "=\"";
    const std::string needleSpaced = std::string(key) + " = \"";
    std::size_t pos = line.find(needle);
    std::size_t keyLen = needle.size();
    if (pos == std::string_view::npos) {
        pos = line.find(needleSpaced);
        keyLen = needleSpaced.size();
    }
    if (pos == std::string_view::npos) {
        return {};
    }

    pos += keyLen;
    const std::size_t end = line.find('"', pos);
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string(line.substr(pos, end - pos));
}

void appendUniqueHook(std::vector<std::string>& hooks, std::string hook) {
    for (const std::string& existing : hooks) {
        if (existing == hook) {
            return;
        }
    }
    hooks.push_back(std::move(hook));
}

} // namespace

u32 profileIdForRegistryType(std::string_view fuseRegistryTypeId) {
    if (fuseRegistryTypeId == "gb.action.move_toward") {
        return 0;
    }
    if (fuseRegistryTypeId == "bb.selector") {
        return 1;
    }
    if (fuseRegistryTypeId == "bb.action.set_flag") {
        return 2;
    }
    if (fuseRegistryTypeId == "bb.condition.distance_less") {
        return 3;
    }
    return 0;
}

bool parseCsModule(std::string_view csModule, std::string_view csText, UaiskCsParseResult& outResult) {
    outResult = UaiskCsParseResult{};
    outResult.moduleName = std::string(csModule);
    outResult.fuseRegistryTypeId = registryTypeForModule(csModule);

    std::string_view cursor = csText;
    while (!cursor.empty()) {
        const std::size_t lineEnd = cursor.find('\n');
        const std::string_view line = trimView(cursor.substr(0, lineEnd));

        if (line.rfind("class ", 0) == 0) {
            const std::size_t nameStart = line.find(' ') + 1;
            const std::size_t nameEnd = line.find_first_of(" :{", nameStart);
            if (nameEnd != std::string_view::npos && nameStart < nameEnd) {
                outResult.className = std::string(line.substr(nameStart, nameEnd - nameStart));
            }
        }

        const std::string behaviorTree = extractQuotedValue(line, "behaviorTree");
        if (!behaviorTree.empty()) {
            appendUniqueHook(outResult.behaviorTreeHooks, behaviorTree);
        }

        const std::string treeProfile = extractQuotedValue(line, "treeProfile");
        if (!treeProfile.empty()) {
            appendUniqueHook(outResult.behaviorTreeHooks, treeProfile);
        }

        const std::size_t hookPos = line.find("hook=");
        if (hookPos != std::string_view::npos) {
            std::string_view hookValue = line.substr(hookPos + 5);
            const std::size_t space = hookValue.find_first_of(" \t\r");
            if (space != std::string_view::npos) {
                hookValue = hookValue.substr(0, space);
            }
            if (!hookValue.empty()) {
                appendUniqueHook(outResult.behaviorTreeHooks, std::string(hookValue));
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        cursor.remove_prefix(lineEnd + 1);
    }

    if (outResult.fuseRegistryTypeId.empty() && !outResult.behaviorTreeHooks.empty()) {
        outResult.fuseRegistryTypeId = registryTypeForModule(outResult.behaviorTreeHooks.front());
    }

    outResult.profileId = treeProfileForParsedModule(outResult);
    outResult.valid = !outResult.moduleName.empty();
    return outResult.valid;
}

u32 treeProfileForParsedModule(const UaiskCsParseResult& parsed) {
    if (!parsed.fuseRegistryTypeId.empty()) {
        return profileIdForRegistryType(parsed.fuseRegistryTypeId);
    }
    if (!parsed.behaviorTreeHooks.empty()) {
        return profileIdForRegistryType(registryTypeForModule(parsed.behaviorTreeHooks.front()));
    }
    return profileIdForRegistryType(registryTypeForModule(parsed.moduleName));
}

} // namespace fuse::ai::uaisk
