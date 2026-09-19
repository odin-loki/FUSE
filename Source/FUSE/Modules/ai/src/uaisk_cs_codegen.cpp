#include <fuse/ai/uaisk_cs_codegen.hpp>

#include <fuse/ai/behavior_runtime.hpp>

namespace fuse::ai::uaisk {

namespace {

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

bool codegenSpecsForModule(const UaiskCsAst& ast,
                           std::vector<NodeLoadSpec>& outSpecs,
                           u32& outRootIndex,
                           std::string* errorOut) {
    if (containsHook(ast, "patrol_squad.bt") || containsHook(ast, "aiBehaviors.cs")) {
        return codegenPatrolSquadSpecs(outSpecs, outRootIndex);
    }
    if (containsHook(ast, "aiMovement.cs") || ast.primaryRegistryTypeId == "gb.action.move_toward") {
        return codegenMoveTowardSpecs(outSpecs, outRootIndex);
    }
    if (containsHook(ast, "aiActions.cs") || ast.primaryRegistryTypeId == "bb.action.set_flag") {
        return codegenSetFlagSpecs(outSpecs, outRootIndex);
    }
    if (containsHook(ast, "aiTargeting.cs") || ast.primaryRegistryTypeId == "bb.condition.distance_less") {
        return codegenDistanceLessSpecs(outSpecs, outRootIndex);
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

bool importCodegenProfile(std::string_view csModule,
                          std::string_view csText,
                          u32 profileId,
                          BehaviorRuntime& runtime,
                          std::string* errorOut) {
    BehaviorTree tree;
    if (!codegenTreeFromCs(csModule, csText, tree, errorOut)) {
        return false;
    }
    runtime.registerTreeProfile(profileId, tree);
    return true;
}

} // namespace fuse::ai::uaisk
