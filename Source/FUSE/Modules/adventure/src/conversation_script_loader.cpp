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

bool load_conversation_hooks_from_torquescript(const std::string& scriptText,
                                               std::vector<ConversationScriptHook>& outHooks,
                                               std::string* errorOut) {
    outHooks.clear();
    std::stringstream stream(scriptText);
    std::string line;
    ConversationScriptHook current{};

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.rfind("function ", 0) == 0) {
            if (!current.npcId.empty() && !current.branchId.empty()) {
                outHooks.push_back(current);
            }
            current = ConversationScriptHook{};
            std::string remainder = trim(line.substr(9));
            const std::size_t paren = remainder.find('(');
            if (paren == std::string::npos) {
                continue;
            }
            const std::string functionName = trim(remainder.substr(0, paren));
            if (functionName.rfind("onConversation_", 0) == 0) {
                const std::string suffix = functionName.substr(15);
                const std::size_t underscore = suffix.rfind('_');
                if (underscore != std::string::npos) {
                    current.npcId = suffix.substr(0, underscore);
                    current.branchId = suffix.substr(underscore + 1);
                }
            } else if (functionName == "onConversation") {
                current.npcId = "npc";
                current.branchId = "default";
            }
            continue;
        }

        if (line.rfind("echo(", 0) == 0 || line.rfind("say(", 0) == 0) {
            const std::size_t quoteStart = line.find('"');
            const std::size_t quoteEnd = line.rfind('"');
            if (quoteStart != std::string::npos && quoteEnd > quoteStart) {
                current.lines.push_back(line.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
            }
            continue;
        }

        if (line.find("requiresItem") != std::string::npos || line.find("requires ") != std::string::npos) {
            std::istringstream reqStream(line);
            std::string token;
            reqStream >> token;
            reqStream >> current.requiredItem >> current.minInventoryCount;
            continue;
        }

        if (line.find("grantItem") != std::string::npos || line.rfind("grant ", 0) == 0) {
            std::istringstream grantStream(line);
            std::string token;
            grantStream >> token;
            grantStream >> current.grantItem >> current.grantAmount;
            continue;
        }
    }

    if (!current.npcId.empty() && !current.branchId.empty()) {
        outHooks.push_back(current);
    }

    if (outHooks.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no TorqueScript conversation hooks found";
        }
        return false;
    }

    return true;
}

bool register_conversation_hooks_from_torquescript(const std::string& scriptText,
                                                   ConversationScriptVm& vm,
                                                   std::string* errorOut) {
    std::vector<ConversationScriptHook> hooks;
    if (!load_conversation_hooks_from_torquescript(scriptText, hooks, errorOut)) {
        return false;
    }

    for (const ConversationScriptHook& hook : hooks) {
        vm.registerHook(hook);
    }
    return true;
}

} // namespace fuse::adventure
