// FUSE Relight RL-3.5: the OmniGraph parser and the runtime (rl_logic_unit). Cases follow dxvk-remix
// tests/rtx/unit/test_graph_usd_parser.cpp@0867d3c (component lookup, version check, property values from typed
// attributes and tokens, connections, cycles, flexible type resolution, relationships), authored as USDA text and
// read through the RL-3.1 composed stage.
#include "test_logic_common.hpp"

#include <fuse/relight/logic/graph_usd_parser.hpp>
#include <fuse/relight/logic/logic_log.hpp>
#include <fuse/relight/logic/logic_runtime.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <algorithm>

namespace rl_logic_test {

namespace {

namespace usd = fuse::relight::mods::usd;

constexpr const char* kRoot = "/RootNode/meshes/mesh_0123456789ABCDEF";

std::string node(const std::string& name, const std::string& type, const std::string& body, int version = 1) {
    return "                def OmniGraphNode \"" + name + "\"\n                {\n                    custom token node:type = \"" +
           type + "\"\n" + (version >= 0 ? "                    custom int node:typeVersion = " + std::to_string(version) + "\n" : "") +
           body + "                }\n";
}
std::string conn(const std::string& type, const std::string& input, const std::string& fromNode, const std::string& output,
                 const std::string& graph = "graph") {
    return "                    custom " + type + " " + input + ".connect = <" + kRoot + "/" + graph + "/" + fromNode + "." + output + ">\n";
}
std::string attr(const std::string& text) { return "                    " + text + "\n"; }

/// A mod stage with one mesh replacement (preserveOriginalDrawCall, a mesh part, a light) and the given graphs.
usd::ComposedStage stageWith(const std::vector<std::pair<std::string, std::string>>& graphs, bool preserve = true) {
    std::string text = "#usda 1.0\n(\n    defaultPrim = \"RootNode\"\n)\n\ndef Xform \"RootNode\"\n{\n    def Scope \"meshes\"\n    {\n"
                       "        def Xform \"mesh_0123456789ABCDEF\"\n        {\n";
    if (preserve) {
        text += "            int preserveOriginalDrawCall = 1\n";
    }
    text += "            def Mesh \"part\"\n            {\n                int[] faceVertexCounts = [3]\n"
            "                int[] faceVertexIndices = [0, 1, 2]\n                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "            }\n            def SphereLight \"lamp\"\n            {\n                float inputs:radius = 0.1\n            }\n";
    for (const auto& [name, nodes] : graphs) {
        text += "            def OmniGraph \"" + name + "\"\n            {\n" + nodes + "            }\n";
    }
    text += "        }\n    }\n}\n";
    usd::ReadOptions options;
    options.files = usd::memoryFileSource({{"/mem/mod/mod.usda", text}});
    usd::ComposedStage stage = usd::readStage("/mem/mod/mod.usda", options);
    CHECK_MSG(stage.ok, "stage did not load");
    return stage;
}

std::size_t countDiag(const std::vector<GraphDiagnostic>& d, LogSeverity s, const std::string& needle) {
    return static_cast<std::size_t>(std::count_if(d.begin(), d.end(), [&](const GraphDiagnostic& x) {
        return x.severity == s && x.message.find(needle) != std::string::npos;
    }));
}

/// The value of `<node>.<property>` in instance 0 of a single-instance runtime run.
struct Run {
    LogicRuntime runtime;
    ReplacementGraphs graphs;
    FrameInputs inputs;
    explicit Run(const ModGraphs& mods) {
        graphs = mods.meshes.begin()->second;
    }
    void frame() {
        runtime.runFrame(inputs, {{1, &graphs, "test"}}, LogicRunOptions{true, false, false, false});
        ++inputs.frame;
    }
    PropertyValue value(const std::string& nodeName, const std::string& property, std::size_t graphIndex = 0) const {
        const GraphInstance* inst = runtime.manager().instance(runtime.instancesOf(1).at(graphIndex));
        const GraphBatch* batch = runtime.manager().batchOf(*inst);
        const GraphTopology& t = batch->topology();
        for (std::size_t c = 0; c < t.componentSpecs.size(); ++c) {
            if (t.nodePaths[c].substr(t.nodePaths[c].rfind('/') + 1) != nodeName) {
                continue;
            }
            for (std::size_t p = 0; p < t.componentSpecs[c]->properties.size(); ++p) {
                if (t.componentSpecs[c]->properties[p].name == property) {
                    return batch->value(t.propertyIndices[c][p], inst->batchIndex());
                }
            }
        }
        return kInvalidPropertyValue;
    }
};

void testTokens() {
    using parser_detail::inferTypeFromTokenString;
    CHECK(inferTypeFromTokenString("(1, 2)") == PT::Float2);
    CHECK(inferTypeFromTokenString("[1, 2, 3]") == PT::Float3);
    CHECK(inferTypeFromTokenString("(1, 2, 3, 4)") == PT::Float4);
    CHECK(inferTypeFromTokenString("0x1A2B") == PT::Hash);
    CHECK(inferTypeFromTokenString("0x") == PT::Float);                   // not a hash; std::stod reads the 0
    CHECK(inferTypeFromTokenString("0x12345678901234567") == PT::Float);  // 17 digits: not a hash; stod parses hex
    CHECK(inferTypeFromTokenString("1.5") == PT::Float);
    CHECK(inferTypeFromTokenString("7") == PT::Float);
    CHECK(inferTypeFromTokenString("true") == PT::Float); // upstream checks for 'e' before the bool names
    CHECK(inferTypeFromTokenString("False") == PT::Float);
    CHECK(inferTypeFromTokenString("hello") == PT::Float); // contains 'e', as upstream
    CHECK(inferTypeFromTokenString("world") == PT::String);

    CHECK(propertyValueFromString("true", PT::Bool) == kTruePropertyValue);
    CHECK(propertyValueFromString("TRUE", PT::Bool) == kTruePropertyValue);
    CHECK(propertyValueFromString("yes", PT::Bool) == kFalsePropertyValue);
    CHECK(propertyValueFromString("2.5", PT::Float) == PropertyValue(2.5f));
    CHECK(propertyValueFromString("abc", PT::Float) == kInvalidPropertyValue);
    CHECK(propertyValueFromString("(1, 2)", PT::Float2) == PropertyValue(Vector2(1, 2)));
    CHECK(propertyValueFromString("1 2 3", PT::Float3) == PropertyValue(Vector3(1, 2, 3)));
    CHECK(propertyValueFromString("(1, 2)", PT::Float3) == PropertyValue(Vector3(0.0f))); // too few components
    CHECK(propertyValueFromString("3", PT::Enum) == PropertyValue(std::in_place_type<std::uint32_t>, 3u));
    CHECK(propertyValueFromString("0x1A", PT::Hash) == PropertyValue(std::in_place_type<std::uint64_t>, 0x1Aull));
    CHECK(propertyValueFromString("1a", PT::Hash) == PropertyValue(std::in_place_type<std::uint64_t>, 0x1Aull));
    CHECK(propertyValueFromString("x", PT::Prim) == kInvalidPropertyValue);
    CHECK(propertyValueFromString("1", PT::Any) == kInvalidPropertyValue);
    CHECK(formatPropertyValue(PropertyValue(std::in_place_type<std::uint64_t>, 0xABull), PT::Hash) == "0x00000000000000AB");
    CHECK(formatPropertyValue(kTruePropertyValue, PT::Bool) == "true");
    CHECK(formatPropertyValue(PropertyValue(Vector3(1, 0.5f, -2)), PT::Float3) == "(1, 0.5, -2)");
}

void testSimpleGraph() {
    const std::string nodes =
        node("counter", "lightspeed.trex.logic.Counter",
             attr("custom bool inputs:increment = 1") + attr("custom float inputs:incrementValue = 2") + attr("custom float outputs:value")) +
        node("greater", "lightspeed.trex.logic.GreaterThan",
             attr("custom float inputs:a") + conn("float", "inputs:a", "counter", "outputs:value") + attr("custom float inputs:b = 5") +
                 attr("custom bool outputs:result")) +
        node("toggle", "lightspeed.trex.logic.Toggle",
             attr("custom bool inputs:triggerToggle") + conn("bool", "inputs:triggerToggle", "greater", "outputs:result") +
                 attr("custom token inputs:defaultState = \"true\"") + attr("custom bool outputs:isOn"));
    const usd::ComposedStage stage = stageWith({{"graph", nodes}});
    const ModGraphs mods = loadModGraphs(stage, "test");
    CHECK(mods.graphCount() == 1 && mods.meshes.count(0x0123456789ABCDEFull) == 1);
    if (mods.graphCount() != 1) {
        return;
    }
    const ReplacementGraphs& rg = mods.meshes.begin()->second;
    // Prim table (processReplacement): original draw, the mesh, the light, the graph.
    CHECK(rg.prims.size() == 4 && rg.prims[0].path == std::string(kRoot) + "/mesh" &&
          rg.prims[0].kind == ReplacementGraphs::PrimKind::OriginalMesh && rg.prims[1].kind == ReplacementGraphs::PrimKind::Mesh &&
          rg.prims[2].kind == ReplacementGraphs::PrimKind::Light && rg.prims[3].kind == ReplacementGraphs::PrimKind::Graph);
    const GraphTopology& t = *rg.graphs[0]->topology;
    CHECK(t.componentSpecs.size() == 3);
    CHECK(t.nodePaths.size() == 3 && t.nodePaths[0].ends_with("/counter") && t.nodePaths[1].ends_with("/greater") &&
          t.nodePaths[2].ends_with("/toggle"));
    // Counter: increment, incrementValue, defaultValue, count, value (5); GreaterThan shares the value index (+2);
    // Toggle shares greater's result (+2).
    CHECK(t.propertyTypes.size() == 9 && rg.graphs[0]->values.size() == 9);
    CHECK(t.propertyIndices[1][0] == t.propertyIndices[0][4] && t.propertyIndices[2][0] == t.propertyIndices[1][2]);
    Run run(mods);
    // Frame 0: created (count 2), then updated (4); frame 1: 6 > 5 -> toggle flips (true -> false); frame 2: flips back.
    run.frame();
    CHECK(run.value("counter", "value") == PropertyValue(4.0f) && run.value("greater", "result") == kFalsePropertyValue &&
          run.value("toggle", "isOn") == kTruePropertyValue);
    run.frame();
    CHECK(run.value("counter", "value") == PropertyValue(6.0f) && run.value("toggle", "isOn") == kFalsePropertyValue);
    run.frame();
    CHECK(run.value("toggle", "isOn") == kTruePropertyValue);
}

void testNodeValidation() {
    std::vector<GraphDiagnostic> diags;
    const std::string nodes =
        node("future", "lightspeed.trex.logic.ConstFloat", attr("custom float inputs:value = 1"), 2) +
        node("unversioned", "lightspeed.trex.logic.ConstFloat", attr("custom float inputs:value = 2"), -1) +
        node("old", "lightspeed.trex.logic.ConstFloat", attr("custom float inputs:value = 3"), 0) +
        node("unknown", "lightspeed.trex.logic.NoSuchComponent", "") +
        node("renamed", "lightspeed.trex.logic.InterpolateFloat", attr("custom float inputs:value = 0.5")) +
        "                def OmniGraphNode \"untyped\"\n                {\n                }\n"
        "                def Scope \"notANode\"\n                {\n                }\n";
    const usd::ComposedStage stage = stageWith({{"graph", nodes}});
    const auto state = parseGraph(stage, std::string(kRoot) + "/graph", {}, &diags);
    const GraphTopology& t = *state->topology;
    CHECK(t.componentSpecs.size() == 2); // "old" (warning) and "renamed" (Remap through its old name)
    CHECK(countDiag(diags, LogSeverity::Error, "newer than this runtime") == 1);
    CHECK(countDiag(diags, LogSeverity::Error, "missing a `node:typeVersion`") == 1);
    CHECK(countDiag(diags, LogSeverity::Warning, "is old") == 1);
    CHECK(countDiag(diags, LogSeverity::Error, "unknown `node:type`") == 1);
    CHECK(countDiag(diags, LogSeverity::Error, "no `node:type`") == 1);
    bool hasRemap = false;
    for (const ComponentSpec* s : t.componentSpecs) {
        hasRemap = hasRemap || s->getClassName() == "Remap";
    }
    CHECK(hasRemap);
}

void testOrderingAndCycles() {
    // Independent nodes of one type: upstream sorts each wave by (component type, path) and reverses the result.
    {
        const std::string nodes = node("a", "lightspeed.trex.logic.ConstFloat", "") + node("b", "lightspeed.trex.logic.ConstFloat", "") +
                                  node("c", "lightspeed.trex.logic.ConstFloat", "");
        const usd::ComposedStage stage = stageWith({{"graph", nodes}});
        const auto state = parseGraph(stage, std::string(kRoot) + "/graph", {});
        const auto& paths = state->topology->nodePaths;
        CHECK(paths.size() == 3 && paths[0].ends_with("/c") && paths[1].ends_with("/b") && paths[2].ends_with("/a"));
    }
    // A cycle drops the nodes on it; the rest loads.
    {
        std::vector<GraphDiagnostic> diags;
        const std::string nodes =
            node("x", "lightspeed.trex.logic.BoolNot", attr("custom bool inputs:input") + conn("bool", "inputs:input", "y", "outputs:result")) +
            node("y", "lightspeed.trex.logic.BoolNot", attr("custom bool inputs:input") + conn("bool", "inputs:input", "x", "outputs:result")) +
            node("z", "lightspeed.trex.logic.ConstBool", attr("custom bool inputs:value = 1"));
        const usd::ComposedStage stage = stageWith({{"graph", nodes}});
        const auto state = parseGraph(stage, std::string(kRoot) + "/graph", {}, &diags);
        CHECK(state->topology->componentSpecs.size() == 1 && state->topology->nodePaths[0].ends_with("/z"));
        CHECK(countDiag(diags, LogSeverity::Error, "has a cycle") == 1);
    }
    // Mismatched connection types are ignored (the input keeps its own value); dangling connections too.
    {
        std::vector<GraphDiagnostic> diags;
        const std::string nodes =
            node("flag", "lightspeed.trex.logic.ConstBool", attr("custom bool inputs:value = 1")) +
            node("less", "lightspeed.trex.logic.LessThan",
                 attr("custom float inputs:a = 1") + conn("float", "inputs:a", "flag", "inputs:value") + attr("custom float inputs:b = 2") +
                     attr("custom float inputs:b.connect = </RootNode/meshes/mesh_0123456789ABCDEF/graph/missing.outputs:x>"));
        const usd::ComposedStage stage = stageWith({{"graph", nodes}});
        const ModGraphs mods = loadModGraphs(stage, "test");
        CHECK(countDiag(mods.diagnostics, LogSeverity::Error, "mismatched types") == 1);
        Run run(mods);
        run.frame();
        CHECK(run.value("less", "result") == kTruePropertyValue); // 1 < 2 from the authored values
    }
}

void testValuesAndTypes() {
    const std::string nodes =
        // Typed attributes and tokens for every property type (upstream testAllPropertyTypes / ...AsStrings).
        node("addTyped", "lightspeed.trex.logic.Add", attr("custom float3 inputs:a = (1, 2, 3)") + attr("custom token inputs:b = \"(4, 5, 6)\"")) +
        node("mul", "lightspeed.trex.logic.Multiply",
             attr("custom token inputs:a") + conn("token", "inputs:a", "addTyped", "outputs:sum") + attr("custom float inputs:b = 2")) +
        node("sel", "lightspeed.trex.logic.Select",
             attr("custom bool inputs:condition = 0") + attr("custom token inputs:inputA = \"0x10\"") +
                 attr("custom token inputs:inputB = \"0xFF\"")) +
        node("hash", "lightspeed.trex.logic.ConstHash", attr("custom uint64 inputs:value = 18446744073709551615")) +
        node("loop", "lightspeed.trex.logic.Loop",
             attr("custom float inputs:value = 1.5") + attr("custom token inputs:loopingType = \"PingPong\"")) +
        node("asset", "lightspeed.trex.logic.ConstAssetPath", attr("custom token inputs:value = \"./layers/a.conf\"")) +
        node("assetTyped", "lightspeed.trex.logic.ConstAssetPath", attr("custom asset inputs:value = @./b.conf@")) +
        node("str", "lightspeed.trex.logic.ConstString", attr("custom string inputs:value = \"hello\"")) +
        node("vec4", "lightspeed.trex.logic.ConstFloat4", attr("custom color4f inputs:value = (0.1, 0.2, 0.3, 0.4)")) +
        node("badType", "lightspeed.trex.logic.ConstFloat3", attr("custom float inputs:value = 5")) +
        node("target", "lightspeed.trex.logic.ReadTransform",
             attr(std::string("custom rel inputs:target = <") + kRoot + "/part>")) +
        node("original", "lightspeed.trex.logic.ConstPrim", attr(std::string("custom rel inputs:value = <") + kRoot + "/mesh>")) +
        node("nowhere", "lightspeed.trex.logic.ConstPrim", attr("custom rel inputs:value = </RootNode/elsewhere>"));
    const usd::ComposedStage stage = stageWith({{"graph", nodes}});
    const ModGraphs mods = loadModGraphs(stage, "test");
    CHECK(countDiag(mods.diagnostics, LogSeverity::Error, "type mismatch") == 1);
    CHECK(countDiag(mods.diagnostics, LogSeverity::Error, "not found in replacement hierarchy") == 1);
    if (mods.graphCount() != 1) {
        CHECK(false);
        return;
    }
    Run run(mods);
    run.frame();
    CHECK(run.value("addTyped", "sum") == PropertyValue(Vector3(5, 7, 9)));
    CHECK(run.value("mul", "product") == PropertyValue(Vector3(10, 14, 18))); // Float3 * Float variant through the connection
    CHECK(run.value("sel", "output") == PropertyValue(std::in_place_type<std::uint64_t>, 0xFFull));
    CHECK(run.value("hash", "value") == PropertyValue(std::in_place_type<std::uint64_t>, 0xFFFFFFFFFFFFFFFFull));
    CHECK(run.value("loop", "loopingType") == PropertyValue(std::in_place_type<std::uint32_t>, 1u));
    CHECK(run.value("loop", "loopedValue") == PropertyValue(0.5f) && run.value("loop", "isReversing") == kTruePropertyValue);
    CHECK(run.value("asset", "value") == PropertyValue(std::string("/mem/mod/layers/a.conf")));
    CHECK(run.value("assetTyped", "value") == PropertyValue(std::string("/mem/mod/b.conf")));
    CHECK(run.value("str", "value") == PropertyValue(std::string("hello")));
    CHECK(run.value("vec4", "value") == PropertyValue(Vector4(0.1f, 0.2f, 0.3f, 0.4f)));
    CHECK(run.value("badType", "value") == PropertyValue(Vector3(0.0f)));
    const ReplacementGraphs& rg = mods.meshes.begin()->second;
    CHECK(run.value("target", "target") == PropertyValue(PrimTarget{rg.primTable.at(std::string(kRoot) + "/part"), PrimTarget::kInvalidInstanceId}));
    CHECK(run.value("original", "value") == PropertyValue(PrimTarget{0, PrimTarget::kInvalidInstanceId}));
    CHECK(run.value("nowhere", "value") == PropertyValue(PrimTarget{}));
    // Prim targets resolve against the owner's prim table.
    run.inputs.prims[1].resize(rg.prims.size());
    run.inputs.prims[1][1].kind = PrimSnapshot::Kind::Mesh;
    run.inputs.prims[1][1].objectToWorld[3] = Vector4(1, 2, 3, 1);
    run.frame();
    CHECK(run.value("target", "position") == PropertyValue(Vector3(1, 2, 3)));

    // Variant choice.
    const GraphTopology& t = *rg.graphs[0]->topology;
    for (std::size_t c = 0; c < t.componentSpecs.size(); ++c) {
        const std::string& n = t.nodePaths[c];
        const auto& types = t.componentSpecs[c]->resolvedTypes;
        if (n.ends_with("/mul")) {
            CHECK(types.at("a") == PT::Float3 && types.at("b") == PT::Float && types.at("product") == PT::Float3);
        } else if (n.ends_with("/sel")) {
            CHECK(types.at("inputA") == PT::Hash);
        }
    }
}

void testPropertyNames() {
    // Renamed properties: the current name when authored, else the first authored old name (upstream property
    // stack rule with identity layer offsets).
    const std::string nodes = node("n", "lightspeed.trex.logic.ConstFloat",
                                   attr("custom float inputs:oldName2 = 2") + attr("custom float inputs:oldName1 = 1"));
    const usd::ComposedStage stage = stageWith({{"graph", nodes}});
    const usd::Prim* prim = stage.find(std::string(kRoot) + "/graph/n");
    CHECK(prim != nullptr);
    if (prim == nullptr) {
        return;
    }
    PropertySpec p(PT::Float, PropertyValue(0.0f), PropertyIOType::Input, "value", "inputs:value", "Value", "", PT::Float);
    CHECK(parser_detail::resolvePropertyName(*prim, p) == "inputs:value");
    p.oldUsdNames = {"inputs:oldName1", "inputs:oldName2"};
    CHECK(parser_detail::resolvePropertyName(*prim, p) == "inputs:oldName1");
    p.oldUsdNames = {"inputs:missing", "inputs:oldName2"};
    CHECK(parser_detail::resolvePropertyName(*prim, p) == "inputs:oldName2");
}

void testRuntime() {
    const std::string counter = node("counter", "lightspeed.trex.logic.Counter", attr("custom bool inputs:increment = 1"));
    const usd::ComposedStage stage = stageWith({{"graph", counter}, {"graph2", counter}}, false);
    ModGraphs mods = loadModGraphs(stage, "test");
    CHECK(mods.graphCount() == 2);
    LogicRuntime rt;
    rt.setModGraphs({mods});
    const ReplacementGraphs* rg = rt.find("test", 0x0123456789ABCDEFull);
    CHECK(rg != nullptr && rt.find("other", 0x0123456789ABCDEFull) == nullptr && rt.find("test", 1) == nullptr);
    if (rg == nullptr) {
        return;
    }
    // Same topology: both graphs (and both owners) share one batch.
    CHECK(rg->graphs[0]->topology->graphHash == rg->graphs[1]->topology->graphHash);
    FrameInputs in;
    const LogicRunOptions opts{true, false, false, true};
    LogicFrameReport r = rt.runFrame(in, {{10, rg, "test"}, {11, rg, "test"}}, opts);
    CHECK(r.added == 4 && r.instances == 4 && r.batches == 1 && r.owners == 2 && r.values.size() == 4);
    CHECK(r.values[0].outputs.size() == 1 && r.values[0].outputs[0].first == "counter.value" && r.values[0].outputs[0].second == "2");
    r = rt.runFrame(in, {{10, rg, "test"}}, opts); // owner 11 gone
    CHECK(r.removed == 2 && r.instances == 2 && r.values[0].outputs[0].second == "3");
    r = rt.runFrame(in, {{10, rg, "test"}}, LogicRunOptions{true, true, false, true}); // paused: state kept, no update
    CHECK(r.values[0].outputs[0].second == "3");
    r = rt.runFrame(in, {{10, rg, "test"}, {12, rg, "test"}}, opts);
    CHECK(r.added == 2 && r.values[0].outputs[0].second == "4" && r.values[2].outputs[0].second == "2");
    const std::string json = logicFrameJson(r);
    CHECK(json.find("\"instances\":4") != std::string::npos && json.find("\"counter.value\":\"4\"") != std::string::npos);
    r = rt.runFrame(in, {{10, rg, "test"}}, LogicRunOptions{false, false, false, true}); // rtx.graph.enable off
    CHECK(r.instances == 0 && r.removed == 4);
    r = rt.runFrame(in, {{10, rg, "test"}}, opts); // re-enabled: state starts over
    CHECK(r.added == 2 && r.values[0].outputs[0].second == "2");
    rt.setModGraphs({mods}); // reload drops every instance
    CHECK(rt.manager().instances().empty());
    // Determinism: two runtimes fed the same frames report identical JSON.
    LogicRuntime a, b;
    a.setModGraphs({mods});
    b.setModGraphs({mods});
    for (int f = 0; f < 5; ++f) {
        const std::vector<GraphOwnerFrame> owners = {{3, a.find("test", 0x0123456789ABCDEFull), "test"}};
        const std::vector<GraphOwnerFrame> ownersB = {{3, b.find("test", 0x0123456789ABCDEFull), "test"}};
        CHECK(logicFrameJson(a.runFrame(in, owners, opts)) == logicFrameJson(b.runFrame(in, ownersB, opts)));
    }
}

} // namespace

void testParser() {
    registerAllComponents();
    testTokens();
    testSimpleGraph();
    testNodeValidation();
    testOrderingAndCycles();
    testValuesAndTypes();
    testPropertyNames();
    testRuntime();
    takeLogMessages();
}

} // namespace rl_logic_test
