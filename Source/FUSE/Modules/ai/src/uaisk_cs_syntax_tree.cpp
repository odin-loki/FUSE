#include <fuse/ai/uaisk_cs_syntax_tree.hpp>

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

void appendUnique(std::vector<std::string>& values, std::string value) {
    for (const std::string& existing : values) {
        if (existing == value) {
            return;
        }
    }
    values.push_back(std::move(value));
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

} // namespace

bool parseCsSyntaxTree(std::string_view csModule, std::string_view csText, UaiskCsSyntaxTree& outTree) {
    outTree = UaiskCsSyntaxTree{};
    outTree.moduleName = std::string(csModule);

    std::string currentClass;
    u32 lineNumber = 0;
    std::string_view cursor = csText;
    while (!cursor.empty()) {
        ++lineNumber;
        const std::size_t lineEnd = cursor.find('\n');
        const std::string_view line = trimView(cursor.substr(0, lineEnd));

        if (line.rfind("class ", 0) == 0) {
            const std::size_t nameStart = line.find(' ') + 1;
            const std::size_t nameEnd = line.find_first_of(" :{", nameStart);
            if (nameEnd != std::string_view::npos && nameStart < nameEnd) {
                currentClass = std::string(line.substr(nameStart, nameEnd - nameStart));
                if (outTree.rootClassName.empty()) {
                    outTree.rootClassName = currentClass;
                }

                UaiskCsSyntaxNode node;
                node.kind = UaiskCsSyntaxNodeKind::Class;
                node.name = currentClass;
                node.line = lineNumber;
                outTree.nodes.push_back(std::move(node));
            }
        }

        if (line.rfind("void ", 0) == 0 || line.rfind("public void ", 0) == 0) {
            std::string_view methodLine = line;
            if (methodLine.rfind("public ", 0) == 0) {
                methodLine.remove_prefix(7);
            }
            if (methodLine.rfind("void ", 0) == 0) {
                methodLine.remove_prefix(5);
                const std::size_t paren = methodLine.find('(');
                if (paren != std::string_view::npos) {
                    UaiskCsSyntaxNode node;
                    node.kind = UaiskCsSyntaxNodeKind::Method;
                    node.name = std::string(methodLine.substr(0, paren));
                    node.parentClass = currentClass;
                    node.line = lineNumber;
                    outTree.nodes.push_back(std::move(node));
                }
            }
        }

        const std::size_t equals = line.find('=');
        if (equals != std::string_view::npos && line.find("class ") == std::string_view::npos) {
            std::string_view key = trimView(line.substr(0, equals));
            if (!key.empty() && key.find(' ') == std::string_view::npos && key.back() != ')') {
                UaiskCsSyntaxNode node;
                node.kind = UaiskCsSyntaxNodeKind::Field;
                node.name = std::string(key);
                node.value = std::string(trimView(line.substr(equals + 1)));
                node.parentClass = currentClass;
                node.line = lineNumber;
                outTree.nodes.push_back(std::move(node));
            }
        }

        const std::string behaviorTree = extractQuotedValue(line, "behaviorTree");
        if (!behaviorTree.empty()) {
            appendUnique(outTree.behaviorTreeHooks, behaviorTree);
            UaiskCsSyntaxNode node;
            node.kind = UaiskCsSyntaxNodeKind::Attribute;
            node.name = "behaviorTree";
            node.value = behaviorTree;
            node.parentClass = currentClass;
            node.line = lineNumber;
            outTree.nodes.push_back(std::move(node));
        }

        const std::string treeProfile = extractQuotedValue(line, "treeProfile");
        if (!treeProfile.empty()) {
            appendUnique(outTree.behaviorTreeHooks, treeProfile);
            UaiskCsSyntaxNode node;
            node.kind = UaiskCsSyntaxNodeKind::Attribute;
            node.name = "treeProfile";
            node.value = treeProfile;
            node.line = lineNumber;
            outTree.nodes.push_back(std::move(node));
        }

        const std::size_t hookPos = line.find("hook=");
        if (hookPos != std::string_view::npos) {
            std::string_view hookValue = line.substr(hookPos + 5);
            const std::size_t space = hookValue.find_first_of(" \t\r");
            if (space != std::string_view::npos) {
                hookValue = hookValue.substr(0, space);
            }
            if (!hookValue.empty()) {
                appendUnique(outTree.behaviorTreeHooks, std::string(hookValue));
                UaiskCsSyntaxNode node;
                node.kind = UaiskCsSyntaxNodeKind::Attribute;
                node.name = "hook";
                node.value = std::string(hookValue);
                node.parentClass = currentClass;
                node.line = lineNumber;
                outTree.nodes.push_back(std::move(node));
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        cursor.remove_prefix(lineEnd + 1);
    }

    outTree.valid = !outTree.moduleName.empty();
    return outTree.valid;
}

std::vector<std::string> behaviorTreeHooksFromSyntaxTree(const UaiskCsSyntaxTree& tree) {
    return tree.behaviorTreeHooks;
}

} // namespace fuse::ai::uaisk
