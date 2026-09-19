#include <fuse/adventure/conversation_script_loader.hpp>

#include <cctype>
#include <sstream>

namespace fuse::adventure {

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

bool load_conversation_hooks_from_text(const std::string& convText,
                                       std::vector<ConversationScriptHook>& outHooks,
                                       std::string* errorOut) {
    outHooks.clear();
    std::stringstream stream(convText);
    std::string line;
    ConversationScriptHook current{};

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind("branch ", 0) == 0) {
            if (!current.npcId.empty() && !current.branchId.empty()) {
                outHooks.push_back(current);
            }
            current = ConversationScriptHook{};
            std::istringstream branchStream(line.substr(7));
            branchStream >> current.npcId >> current.branchId;
            continue;
        }

        if (line.rfind("line ", 0) == 0) {
            current.lines.push_back(trim(line.substr(5)));
            continue;
        }

        if (line.rfind("requires ", 0) == 0) {
            std::istringstream reqStream(line.substr(9));
            reqStream >> current.requiredItem >> current.minInventoryCount;
            continue;
        }

        if (line.rfind("grant ", 0) == 0) {
            std::istringstream grantStream(line.substr(6));
            grantStream >> current.grantItem >> current.grantAmount;
            continue;
        }

        if (line.rfind("priority ", 0) == 0) {
            continue;
        }
    }

    if (!current.npcId.empty() && !current.branchId.empty()) {
        outHooks.push_back(current);
    }

    if (outHooks.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no conversation hooks found in text";
        }
        return false;
    }

    return true;
}

bool register_conversation_hooks_from_text(const std::string& convText,
                                             ConversationScriptVm& vm,
                                             std::string* errorOut) {
    std::vector<ConversationScriptHook> hooks;
    if (!load_conversation_hooks_from_text(convText, hooks, errorOut)) {
        return false;
    }

    for (const ConversationScriptHook& hook : hooks) {
        vm.registerHook(hook);
    }
    return true;
}

} // namespace fuse::adventure
