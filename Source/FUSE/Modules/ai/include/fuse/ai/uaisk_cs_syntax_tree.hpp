#pragma once

#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::ai::uaisk {

enum class UaiskCsSyntaxNodeKind : u8 {
    Class = 0,
    Method,
    Field,
    Attribute,
};

/// Line-level syntax node for fuller UAISK `.cs` AST (complements codegen `UaiskCsAst`).
struct UaiskCsSyntaxNode {
    UaiskCsSyntaxNodeKind kind = UaiskCsSyntaxNodeKind::Class;
    std::string name;
    std::string value;
    std::string bodyText;
    std::string parentClass;
    u32 line = 0;
};

struct UaiskCsSyntaxTree {
    std::string moduleName;
    std::string rootClassName;
    std::vector<UaiskCsSyntaxNode> nodes;
    std::vector<std::string> behaviorTreeHooks;
    bool valid = false;
};

/// Parse UAISK `.cs` text into a line-level syntax tree plus behavior-tree hook refs.
[[nodiscard]] bool parseCsSyntaxTree(std::string_view csModule, std::string_view csText, UaiskCsSyntaxTree& outTree);

/// Collect behavior-tree hook refs from a parsed syntax tree.
[[nodiscard]] std::vector<std::string> behaviorTreeHooksFromSyntaxTree(const UaiskCsSyntaxTree& tree);

} // namespace fuse::ai::uaisk
