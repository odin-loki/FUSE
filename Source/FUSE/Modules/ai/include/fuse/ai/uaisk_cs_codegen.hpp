#pragma once

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/uaisk_cs_parser.hpp>
#include <fuse/ai/uaisk_cs_syntax_tree.hpp>

#include <string>
#include <vector>

namespace fuse::ai::uaisk {

/// Method reference distilled from UAISK `.cs` (behavior-tree hook ore).
struct UaiskCsMethodRef {
    std::string name;
    std::string bodyText;
    std::vector<std::string> behaviorTreeRefs;
};

/// Field reference distilled from UAISK `.cs` (blackboard / tuning ore).
struct UaiskCsFieldRef {
    std::string name;
    std::string typeName;
    std::string defaultValue;
};

/// Lightweight AST distilled from `UaiskCsParseResult` for codegen passes.
struct UaiskCsAst {
    std::string moduleName;
    std::string className;
    std::string baseClass;
    std::string primaryRegistryTypeId;
    std::vector<std::string> behaviorTreeHooks;
    std::vector<UaiskCsMethodRef> methods;
    std::vector<UaiskCsFieldRef> fields;
};

/// Build an AST view from parsed `.cs` metadata.
[[nodiscard]] bool buildAstFromParse(const UaiskCsParseResult& parsed, UaiskCsAst& outAst);

/// Build an AST view from a line-level syntax tree (fuller UAISK codegen ore).
[[nodiscard]] bool buildAstFromSyntaxTree(const UaiskCsSyntaxTree& tree, UaiskCsAst& outAst);

/// Emit `NodeLoadSpec` rows for a UAISK module (patrol squad / move-toward templates).
[[nodiscard]] bool codegenSpecsForModule(const UaiskCsAst& ast,
                                         std::vector<NodeLoadSpec>& outSpecs,
                                         u32& outRootIndex,
                                         std::string* errorOut = nullptr);

/// Parse `.cs` text, codegen specs, and build a `BehaviorTree`.
[[nodiscard]] bool codegenTreeFromCs(std::string_view csModule,
                                     std::string_view csText,
                                     BehaviorTree& outTree,
                                     std::string* errorOut = nullptr);

/// Codegen from syntax tree AST (fuller UAISK `.cs` codegen path).
[[nodiscard]] bool codegenTreeFromSyntaxTree(std::string_view csModule,
                                             std::string_view csText,
                                             BehaviorTree& outTree,
                                             std::string* errorOut = nullptr);

/// Import a codegen tree into a runtime profile slot.
[[nodiscard]] bool importCodegenProfile(std::string_view csModule,
                                        std::string_view csText,
                                        u32 profileId,
                                        BehaviorRuntime& runtime,
                                        std::string* errorOut = nullptr);

/// Codegen from `.cs` and hot-reload an existing runtime profile (UAISK AST → tree reload ore).
[[nodiscard]] bool reloadCodegenProfile(std::string_view csModule,
                                        std::string_view csText,
                                        u32 profileId,
                                        BehaviorRuntime& runtime,
                                        TreeReloadPolicy policy = TreeReloadPolicy::PreserveBlackboard,
                                        std::string* errorOut = nullptr);

} // namespace fuse::ai::uaisk
