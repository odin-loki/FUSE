#include <fuse/ai/uaisk_cs_codegen.hpp>

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/uaisk_cs_syntax_tree.hpp>

#include <cstdlib>

namespace fuse::ai::uaisk {

namespace {

float parseFieldFloat(const std::string& value, float fallback) {
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

u32 parseFieldUInt(const std::string& value, u32 fallback) {
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<u32>(std::stoul(value));
    } catch (...) {
        return fallback;
    }
}

void applyFieldDefaultsToSpecs(const UaiskCsAst& ast, std::vector<NodeLoadSpec>& specs) {
    float patrolRadius = 8.f;
    float distanceThreshold = 5.f;
    u32 squadFlagIndex = 1;

    for (const UaiskCsFieldRef& field : ast.fields) {
        if (field.name == "patrolRadius" || field.name == "allyRadius") {
            patrolRadius = parseFieldFloat(field.defaultValue, patrolRadius);
        } else if (field.name == "distanceThreshold" || field.name == "patrolDistance") {
            distanceThreshold = parseFieldFloat(field.defaultValue, distanceThreshold);
        } else if (field.name == "squadFlagIndex" || field.name == "flagIndex") {
            squadFlagIndex = parseFieldUInt(field.defaultValue, squadFlagIndex);
        }
    }

    for (NodeLoadSpec& spec : specs) {
        if (spec.typeId == "bb.condition.allies_in_radius") {
            spec.threshold = patrolRadius;
            spec.flagIndex = squadFlagIndex;
        } else if (spec.typeId == "bb.condition.distance_less") {
            spec.threshold = distanceThreshold;
        } else if (spec.typeId == "bb.action.set_flag") {
            spec.flagIndex = squadFlagIndex;
        }
    }
}

bool containsHook(const UaiskCsAst& ast, std::string_view needle) {
    for (const std::string& hook : ast.behaviorTreeHooks) {
        if (hook == needle) {
            return true;
        }
    }
    return ast.moduleName == needle;
}

bool codegenPatrolSquadSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.condition.allies_in_radius", 8.f, 0, 1},
        {"bb.action.set_flag", 0.f, 1},
        {"bb.condition.distance_less", 5.f},
        {"bb.action.set_flag", 0.f, 0},
        {"bb.sequence", 0.f, 0, 1, "", {0, 1}},
        {"bb.sequence", 0.f, 0, 1, "", {2, 3}},
        {"bb.selector", 0.f, 0, 1, "aiBehaviors.cs", {4, 5}},
    };
    rootIndex = 6;
    return true;
}

bool codegenMoveTowardSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"gb.action.move_toward", 0.25f},
    };
    rootIndex = 0;
    return true;
}

bool codegenSetFlagSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.action.set_flag", 0.f, 1},
    };
    rootIndex = 0;
    return true;
}

bool codegenDistanceLessSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.condition.distance_less", 5.f},
    };
    rootIndex = 0;
    return true;
}

bool codegenWaitThenMoveSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.action.wait", 0.5f},
        {"gb.action.move_toward", 0.25f},
        {"bb.sequence", 0.f, 0, 1, "", {0, 1}},
    };
    rootIndex = 2;
    return true;
}

bool codegenAlliesThenPatrolSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.condition.allies_in_radius", 6.f, 0, 1},
        {"bb.action.set_flag", 0.f, 1},
        {"bb.condition.distance_less", 4.f},
        {"bb.action.set_flag", 0.f, 0},
        {"bb.sequence", 0.f, 0, 1, "", {0, 1}},
        {"bb.sequence", 0.f, 0, 1, "", {2, 3}},
        {"bb.selector", 0.f, 0, 1, "aiSquad.cs", {4, 5}},
    };
    rootIndex = 6;
    return true;
}

bool methodImpliesCodegen(const UaiskCsAst& ast, std::string_view methodName) {
    for (const UaiskCsMethodRef& method : ast.methods) {
        if (method.name == methodName) {
            return true;
        }
    }
    return false;
}

} // namespace

bool buildAstFromParse(const UaiskCsParseResult& parsed, UaiskCsAst& outAst) {
    if (!parsed.valid) {
        return false;
    }

    outAst.moduleName = parsed.moduleName;
    outAst.className = parsed.className;
    outAst.baseClass = parsed.baseClass;
    outAst.primaryRegistryTypeId = parsed.fuseRegistryTypeId;
    outAst.behaviorTreeHooks = parsed.behaviorTreeHooks;

    for (const std::string& methodName : parsed.methodNames) {
        UaiskCsMethodRef method;
        method.name = methodName;
        for (const std::string& hook : parsed.behaviorTreeHooks) {
            if (hook.find(methodName) != std::string::npos) {
                method.behaviorTreeRefs.push_back(hook);
            }
        }
        outAst.methods.push_back(std::move(method));
    }

    for (const std::string& fieldName : parsed.fieldNames) {
        UaiskCsFieldRef field;
        field.name = fieldName;
        field.typeName = "float";
        outAst.fields.push_back(std::move(field));
    }

    return true;
}

bool buildAstFromSyntaxTree(const UaiskCsSyntaxTree& tree, UaiskCsAst& outAst) {
    if (!tree.valid) {
        return false;
    }

    outAst = UaiskCsAst{};
    outAst.moduleName = tree.moduleName;
    outAst.className = tree.rootClassName;
    outAst.behaviorTreeHooks = tree.behaviorTreeHooks;

    for (const UaiskCsSyntaxNode& node : tree.nodes) {
        if (node.kind == UaiskCsSyntaxNodeKind::Class && outAst.baseClass.empty() && !node.value.empty()) {
            outAst.baseClass = node.value;
        }
        if (node.kind == UaiskCsSyntaxNodeKind::Method) {
            UaiskCsMethodRef method;
            method.name = node.name;
            for (const std::string& hook : tree.behaviorTreeHooks) {
                if (hook.find(node.name) != std::string::npos) {
                    method.behaviorTreeRefs.push_back(hook);
                }
            }
            outAst.methods.push_back(std::move(method));
        }
        if (node.kind == UaiskCsSyntaxNodeKind::Field) {
            UaiskCsFieldRef field;
            field.name = node.name;
            field.typeName = "float";
            field.defaultValue = node.value;
            outAst.fields.push_back(std::move(field));
        }
        if (node.kind == UaiskCsSyntaxNodeKind::Attribute && node.name == "behaviorTree") {
            outAst.primaryRegistryTypeId = node.value;
        }
    }

    return !outAst.moduleName.empty();
}

bool codegenSpecsForModule(const UaiskCsAst& ast,
                           std::vector<NodeLoadSpec>& outSpecs,
                           u32& outRootIndex,
                           std::string* errorOut) {
    if (containsHook(ast, "patrol_squad.bt") || containsHook(ast, "aiBehaviors.cs")) {
        const bool ok = codegenPatrolSquadSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }
    if (containsHook(ast, "aiMovement.cs") || ast.primaryRegistryTypeId == "gb.action.move_toward") {
        return codegenMoveTowardSpecs(outSpecs, outRootIndex);
    }
    if (containsHook(ast, "aiActions.cs") || ast.primaryRegistryTypeId == "bb.action.set_flag") {
        return codegenSetFlagSpecs(outSpecs, outRootIndex);
    }
    if (containsHook(ast, "aiTargeting.cs") || ast.primaryRegistryTypeId == "bb.condition.distance_less") {
        const bool ok = codegenDistanceLessSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }
    if (containsHook(ast, "aiSquad.cs") || methodImpliesCodegen(ast, "onSquadPatrol")) {
        const bool ok = codegenAlliesThenPatrolSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }
    if (methodImpliesCodegen(ast, "onWaitThenMove") || methodImpliesCodegen(ast, "onPatrolWait")) {
        const bool ok = codegenWaitThenMoveSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }

    if (errorOut != nullptr) {
        *errorOut = "unsupported UAISK module for codegen";
    }
    return false;
}

bool codegenTreeFromCs(std::string_view csModule,
                       std::string_view csText,
                       BehaviorTree& outTree,
                       std::string* errorOut) {
    UaiskCsParseResult parsed;
    if (!parseCsModule(csModule, csText, parsed)) {
        if (errorOut != nullptr) {
            *errorOut = "UAISK cs parse failed";
        }
        return false;
    }

    UaiskCsAst ast;
    if (!buildAstFromParse(parsed, ast)) {
        if (errorOut != nullptr) {
            *errorOut = "UAISK ast build failed";
        }
        return false;
    }

    std::vector<NodeLoadSpec> specs;
    u32 rootIndex = 0;
    if (!codegenSpecsForModule(ast, specs, rootIndex, errorOut)) {
        return false;
    }

    if (!loadTreeFromSpecs(specs, rootIndex, outTree)) {
        if (errorOut != nullptr) {
            *errorOut = "UAISK codegen tree load failed";
        }
        return false;
    }

    return true;
}

bool codegenTreeFromSyntaxTree(std::string_view csModule,
                               std::string_view csText,
                               BehaviorTree& outTree,
                               std::string* errorOut) {
    UaiskCsSyntaxTree tree;
    if (!parseCsSyntaxTree(csModule, csText, tree)) {
        if (errorOut != nullptr) {
            *errorOut = "UAISK syntax tree parse failed";
        }
        return false;
    }

    UaiskCsAst ast;
    if (!buildAstFromSyntaxTree(tree, ast)) {
        if (errorOut != nullptr) {
            *errorOut = "UAISK ast build from syntax tree failed";
        }
        return false;
    }

    std::vector<NodeLoadSpec> specs;
    u32 rootIndex = 0;
    if (!codegenSpecsForModule(ast, specs, rootIndex, errorOut)) {
        return false;
    }

    if (!loadTreeFromSpecs(specs, rootIndex, outTree)) {
        if (errorOut != nullptr) {
            *errorOut = "UAISK syntax-tree codegen tree load failed";
        }
        return false;
    }

    return true;
}

bool importCodegenProfile(std::string_view csModule,
                          std::string_view csText,
                          u32 profileId,
                          BehaviorRuntime& runtime,
                          std::string* errorOut) {
    BehaviorTree tree;
    if (!codegenTreeFromSyntaxTree(csModule, csText, tree, errorOut) &&
        !codegenTreeFromCs(csModule, csText, tree, errorOut)) {
        return false;
    }
    runtime.registerTreeProfile(profileId, tree);
    return true;
}

bool reloadCodegenProfile(std::string_view csModule,
                          std::string_view csText,
                          u32 profileId,
                          BehaviorRuntime& runtime,
                          TreeReloadPolicy policy,
                          std::string* errorOut) {
    BehaviorTree tree;
    if (!codegenTreeFromSyntaxTree(csModule, csText, tree, errorOut) &&
        !codegenTreeFromCs(csModule, csText, tree, errorOut)) {
        return false;
    }
    runtime.reloadTreeProfile(profileId, tree, policy);
    return true;
}

} // namespace fuse::ai::uaisk
