#include <fuse/ai/uaisk_cs_codegen.hpp>

#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/uaisk_cs_syntax_tree.hpp>
#include <fuse/ai/uaisk_expression_ast.hpp>

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

void applyLeafExpressionToSpecs(const UaiskExpressionAst& expr, std::vector<NodeLoadSpec>& specs) {
    if (!expr.valid) {
        return;
    }
    for (NodeLoadSpec& spec : specs) {
        if (spec.typeId == "bb.condition.distance_less" &&
            (expr.fieldName == "distance" || expr.fieldName == "distanceThreshold" ||
             expr.fieldName == "patrolDistance") &&
            (expr.op == UaiskExpressionOp::Less || expr.op == UaiskExpressionOp::Equal)) {
            spec.threshold = expr.threshold;
        } else if (spec.typeId == "bb.condition.allies_in_radius" &&
                   (expr.fieldName == "patrolRadius" || expr.fieldName == "allyRadius") &&
                   (expr.op == UaiskExpressionOp::Greater || expr.op == UaiskExpressionOp::Equal)) {
            spec.threshold = expr.threshold;
        }
    }
}

void applyExpressionDefaultsToSpecs(const UaiskCsAst& ast, std::vector<NodeLoadSpec>& specs) {
    for (const UaiskExpressionAst& expr : ast.conditions) {
        std::vector<UaiskExpressionAst> leaves;
        collectExpressionLeaves(expr, leaves);
        for (const UaiskExpressionAst& leaf : leaves) {
            applyLeafExpressionToSpecs(leaf, specs);
        }
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

    applyExpressionDefaultsToSpecs(ast, specs);
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

const UaiskCsMethodRef* findMethod(const UaiskCsAst& ast, std::string_view methodName) {
    for (const UaiskCsMethodRef& method : ast.methods) {
        if (method.name == methodName) {
            return &method;
        }
    }
    return nullptr;
}

bool bodyContains(const std::string& body, std::string_view needle) {
    return body.find(needle) != std::string::npos;
}

float parseWaitDurationFromBody(const std::string& body) {
    const std::size_t waitPos = body.find("wait(");
    if (waitPos == std::string::npos) {
        return 0.5f;
    }
    const std::size_t open = body.find('(', waitPos);
    const std::size_t close = body.find(')', open);
    if (open == std::string::npos || close == std::string::npos) {
        return 0.5f;
    }
    return parseFieldFloat(body.substr(open + 1, close - open - 1), 0.5f);
}

float parseMoveSpeedFromBody(const std::string& body) {
    const std::size_t speedPos = body.find("moveSpeed");
    if (speedPos == std::string::npos) {
        return 0.25f;
    }
    const std::size_t eq = body.find('=', speedPos);
    if (eq == std::string::npos) {
        return 0.25f;
    }
    const std::size_t semi = body.find(';', eq);
    return parseFieldFloat(body.substr(eq + 1, semi == std::string::npos ? body.size() - eq - 1 : semi - eq - 1),
                          0.25f);
}

float parseCallFloatArg(const std::string& body, std::string_view callName, float fallback) {
    const std::string needle = std::string(callName) + "(";
    const std::size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    const std::size_t open = body.find('(', pos);
    const std::size_t close = body.find(')', open);
    if (open == std::string::npos || close == std::string::npos) {
        return fallback;
    }
    return parseFieldFloat(body.substr(open + 1, close - open - 1), fallback);
}

void appendMethodBodyLeaves(const std::string& body, std::vector<NodeLoadSpec>& specs, std::vector<u32>& childIndices) {
    if (bodyContains(body, "wait(") || bodyContains(body, "Wait(")) {
        specs.push_back({"bb.action.wait", parseWaitDurationFromBody(body)});
        childIndices.push_back(static_cast<u32>(specs.size() - 1));
    }
    if (bodyContains(body, "moveToward") || bodyContains(body, "MoveToward")) {
        specs.push_back({"gb.action.move_toward", parseMoveSpeedFromBody(body)});
        childIndices.push_back(static_cast<u32>(specs.size() - 1));
    }
    if (bodyContains(body, "setFlag") || bodyContains(body, "SetFlag")) {
        specs.push_back({"bb.action.set_flag", 0.f, 1});
        childIndices.push_back(static_cast<u32>(specs.size() - 1));
    }
    if (bodyContains(body, "distanceLess") || bodyContains(body, "DistanceLess")) {
        specs.push_back({"bb.condition.distance_less", parseCallFloatArg(body, "distanceLess", 5.f)});
        childIndices.push_back(static_cast<u32>(specs.size() - 1));
    }
    if (bodyContains(body, "alliesInRadius") || bodyContains(body, "AlliesInRadius")) {
        specs.push_back({"bb.condition.allies_in_radius", parseCallFloatArg(body, "alliesInRadius", 6.f), 0, 1});
        childIndices.push_back(static_cast<u32>(specs.size() - 1));
    }
    if (bodyContains(body, "blackboardSet") || bodyContains(body, "BlackboardSet")) {
        specs.push_back({"bb.action.blackboard_set", 0.f, 0, 0, "patrol_flag"});
        childIndices.push_back(static_cast<u32>(specs.size() - 1));
    }
}

bool codegenNestedSelectorSequenceSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.action.wait", 0.25f},
        {"gb.action.move_toward", 0.2f},
        {"bb.condition.distance_less", 6.f},
        {"bb.action.set_flag", 0.f, 1},
        {"bb.sequence", 0.f, 0, 1, "patrol_branch", {0, 1}},
        {"bb.sequence", 0.f, 0, 1, "engage_branch", {2, 3}},
        {"bb.selector", 0.f, 0, 1, "aiComposite.cs", {4, 5}},
    };
    rootIndex = 6;
    return true;
}

bool codegenNestedSequenceSelectorSpecs(std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    specs = {
        {"bb.condition.allies_in_radius", 5.f, 0, 1},
        {"bb.action.set_flag", 0.f, 1},
        {"bb.condition.distance_less", 3.f},
        {"bb.action.set_flag", 0.f, 0},
        {"bb.selector", 0.f, 0, 1, "squad_branch", {0, 1}},
        {"bb.selector", 0.f, 0, 1, "solo_branch", {2, 3}},
        {"bb.sequence", 0.f, 0, 1, "aiComposite.cs", {4, 5}},
    };
    rootIndex = 6;
    return true;
}

bool codegenFromMethodBodies(const UaiskCsAst& ast, std::vector<NodeLoadSpec>& specs, u32& rootIndex) {
    std::vector<u32> childIndices;
    for (const UaiskCsMethodRef& method : ast.methods) {
        if (method.bodyText.empty()) {
            continue;
        }
        appendMethodBodyLeaves(method.bodyText, specs, childIndices);
    }

    if (childIndices.empty()) {
        return false;
    }

    if (childIndices.size() == 1u) {
        rootIndex = childIndices[0];
        return true;
    }

    specs.push_back({"bb.sequence", 0.f, 0, 1, "", childIndices});
    rootIndex = static_cast<u32>(specs.size() - 1);
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
            method.bodyText = node.bodyText;
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

            if (node.name == "condition" || node.value.find('<') != std::string::npos ||
                node.value.find('>') != std::string::npos || node.value.find("&&") != std::string::npos ||
                node.value.find("||") != std::string::npos || node.value.find('!') != std::string::npos) {
                UaiskExpressionAst expr;
                if (parseExpressionAst(node.value, expr)) {
                    outAst.conditions.push_back(std::move(expr));
                }
            }
        }
        if (node.kind == UaiskCsSyntaxNodeKind::Attribute && node.name == "behaviorTree") {
            outAst.primaryRegistryTypeId = node.value;
        }
        if (node.kind == UaiskCsSyntaxNodeKind::Attribute && node.name == "composite") {
            if (node.value == "selector") {
                outAst.compositeKind = UaiskCompositeKind::Selector;
            } else if (node.value == "sequence") {
                outAst.compositeKind = UaiskCompositeKind::Sequence;
            }
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
    if (containsHook(ast, "aiComposite.cs") || methodImpliesCodegen(ast, "onNestedPatrol")) {
        const bool ok = ast.compositeKind == UaiskCompositeKind::Sequence
                            ? codegenNestedSequenceSelectorSpecs(outSpecs, outRootIndex)
                            : codegenNestedSelectorSequenceSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }
    if (ast.compositeKind == UaiskCompositeKind::Selector) {
        const bool ok = codegenNestedSelectorSequenceSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }
    if (ast.compositeKind == UaiskCompositeKind::Sequence) {
        const bool ok = codegenNestedSequenceSelectorSpecs(outSpecs, outRootIndex);
        if (ok) {
            applyFieldDefaultsToSpecs(ast, outSpecs);
        }
        return ok;
    }

    for (const UaiskCsMethodRef& method : ast.methods) {
        if (!method.bodyText.empty()) {
            const bool ok = codegenFromMethodBodies(ast, outSpecs, outRootIndex);
            if (ok) {
                applyFieldDefaultsToSpecs(ast, outSpecs);
                return true;
            }
        }
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
