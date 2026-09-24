// FUSE Relight RL-3.1: unit tests of the USD reader (ctest rl_mod_usd_unit).
//
//   values       USDA value text <-> Value (numbers incl. inf / nan, escapes, triple-quoted strings, @@@assets@@@,
//                paths, tuples, arrays, dictionaries, time-sample dictionaries, None) and the error paths;
//   paths        normalizePath / parentDir / anchorAssetPath / JSON relativization;
//   classify     classifyPrimPath over the Remix sections, prefixes, hash spellings and depths;
//   compose      in-memory stages: list-op application order inside one spec (delete, add, prepend, append) and
//                across layers, variant fallbacks, payload loading off, arc depth limit, reference cycles, bad
//                layers reported with the prim path, a missing root layer, internal-reference root identity,
//                sublayer offsets stripped (TinyUSDZ cannot parse them), deterministic dumps;
//   robustness   mutated and truncated layers never crash the loader or the composer.
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

namespace usd = fuse::relight::mods::usd;
using usd::Value;

// ---- values ---------------------------------------------------------------------------------------------------

void testValues() {
    auto parse = [](const char* t) { return usd::parseValue(t); };
    auto v = parse("  -1.5e3 ");
    CHECK(v && v->kind == Value::Kind::Number && v->number == -1500.0);
    v = parse("inf");
    CHECK(v && std::isinf(v->number) && v->number > 0);
    v = parse("-inf");
    CHECK(v && std::isinf(v->number) && v->number < 0);
    v = parse("nan");
    CHECK(v && std::isnan(v->number));
    v = parse("18000000000000000000");
    CHECK(v && v->text == "18000000000000000000");
    v = parse("true");
    CHECK(v && v->kind == Value::Kind::Bool && v->boolean);
    v = parse(R"("a \"q\" \\ \n b")");
    CHECK(v && v->kind == Value::Kind::String && v->text == "a \"q\" \\ \n b");
    v = parse("'single'");
    CHECK(v && v->text == "single");
    v = parse("\"\"\"multi\nline \"quoted\" \"\"\"");
    CHECK(v && v->text == "multi\nline \"quoted\" ");
    v = parse("@./a b/c.dds@");
    CHECK(v && v->kind == Value::Kind::Asset && v->text == "./a b/c.dds");
    v = parse("@@@odd@name.usd@@@");
    CHECK(v && v->kind == Value::Kind::Asset && v->text == "odd@name.usd");
    v = parse("</A/B.prop>");
    CHECK(v && v->kind == Value::Kind::Path && v->text == "/A/B.prop");
    v = parse("[(1, 2, 3), (4, 5, 6),]");
    CHECK((v && v->kind == Value::Kind::Array && v->items.size() == 2 && v->items[1].asNumbers() == std::vector<double>{4, 5, 6}));
    v = parse("( (1, 0), (0, 1) )");
    CHECK(v && v->kind == Value::Kind::Tuple && v->items.size() == 2);
    v = parse("[]");
    CHECK(v && v->kind == Value::Kind::Array && v->items.empty());
    v = parse("None");
    CHECK(v && v->isNone());
    v = parse("{ int answer = 42\n string \"quoted name\" = \"x\"; dictionary d = { double pi = 3.5 } }");
    CHECK(v && v->kind == Value::Kind::Dict && v->dict.size() == 3);
    CHECK(v && v->get("answer") && v->get("answer")->number == 42);
    CHECK(v && v->get("quoted name") && v->get("quoted name")->text == "x");
    CHECK(v && v->get("d") && v->get("d")->get("pi") && v->get("d")->get("pi")->number == 3.5);
    v = parse("{\n 0: 1,\n 2.5: (1, 2),\n -1: None,\n}");
    CHECK(v && v->dict.size() == 3 && v->dict[1].first == "2.5" && v->dict[2].second.isNone());
    v = parse("[1, # comment\n 2]");
    CHECK(v && v->items.size() == 2);
    // Errors.
    std::string err;
    CHECK(!usd::parseValue("\"unterminated", &err) && !err.empty());
    CHECK(!usd::parseValue("[1, 2", &err));
    CHECK(!usd::parseValue("@open", &err));
    CHECK(!usd::parseValue("1 2", &err));
    CHECK(!usd::parseValue("", &err));
    CHECK(!usd::parseValue("{ int = }", &err));
    // Format round trip.
    for (const char* t : {"[(0.1, -2, 3e-07), (1, 2, 3)]", "\"a \\\"b\\\" \\\\ c\"", "@x.dds@", "</a/b>", "None", "true",
                          "{ int a = 1; string b = \"x\" }", "{ 0: 1; 10: 2 }"}) {
        const auto a = usd::parseValue(t);
        CHECK(a.has_value());
        if (a) {
            const auto b = usd::parseValue(usd::formatValue(*a));
            CHECK(b && *a == *b);
        }
    }
    CHECK(usd::formatNumber(0.1) == "0.1");
    CHECK(usd::formatNumber(3.0) == "3");
    CHECK(usd::formatNumber(-0.0) == "0");
    CHECK(usd::formatNumber(1e300) == "1e+300");
    // Accessors.
    CHECK(Value::makeNumber(0).asBool() == false);
    CHECK(Value::makeBool(true).asNumber() == 1.0);
    CHECK(!Value::makeString("x").asNumber());
    CHECK(Value::makeString("x").asString() == std::string("x"));
    // JSON.
    Value asset = Value::makeAsset("../t.dds");
    asset.resolved = "/root/t.dds";
    CHECK(usd::valueToJson(asset, "/root/mod") == "{\"asset\": \"../t.dds\", \"resolved\": \"../t.dds\"}");
    CHECK(usd::valueToJson(asset, "/root") == "{\"asset\": \"../t.dds\", \"resolved\": \"t.dds\"}");
    CHECK(usd::valueToJson(Value::makeNumber(std::numeric_limits<double>::infinity())) == "\"inf\"");
    CHECK(usd::jsonQuote("a\"\n\x01") == "\"a\\\"\\n\\u0001\"");
}

// ---- paths ----------------------------------------------------------------------------------------------------

void testPaths() {
    CHECK(usd::normalizePath("a/./b/../c") == "a/c");
    CHECK(usd::normalizePath("/a//b/") == "/a/b");
    CHECK(usd::normalizePath("../x/../../y") == "../../y");
    CHECK(usd::normalizePath("/../x") == "/x");
    CHECK(usd::normalizePath("C:\\mods\\m\\..\\a.usda") == "C:/mods/a.usda");
    CHECK(usd::normalizePath("") == ".");
    CHECK(usd::parentDir("/a/b/c.usda") == "/a/b");
    CHECK(usd::parentDir("c.usda").empty());
    CHECK(usd::parentDir("/c.usda") == "/");
    CHECK(usd::anchorAssetPath("/mods/m/materials", "../textures/a.dds") == "/mods/m/textures/a.dds");
    CHECK(usd::anchorAssetPath("/mods/m", "./a.usd") == "/mods/m/a.usd");
    CHECK(usd::anchorAssetPath("/mods/m", "/abs/a.usd") == "/abs/a.usd");
    CHECK(usd::anchorAssetPath("/mods/m", "D:\\abs\\a.usd") == "D:/abs/a.usd");
    CHECK(usd::anchorAssetPath("/mods/m", "omniverse://server/a.usd") == "omniverse://server/a.usd");
    CHECK(usd::anchorAssetPath("/mods/m", "").empty());
    CHECK(usd::anchorAssetPath("", "a/../b.usd") == "b.usd");
}

// ---- classify -------------------------------------------------------------------------------------------------

void testClassify() {
    using usd::PrimClass;
    auto c = [](const char* p) { return usd::classifyPrimPath(p); };
    CHECK(c("/RootNode").cls == PrimClass::Section);
    CHECK(c("/RootNode/meshes").cls == PrimClass::Section);
    CHECK(c("/RootNode/Looks").cls == PrimClass::Section);
    CHECK(c("/RootNode/lights").cls == PrimClass::Section);
    CHECK(c("/RootNode/instances").cls == PrimClass::None);
    CHECK(c("/").cls == PrimClass::None);
    CHECK(c("/Other/meshes/mesh_0000000000000001").cls == PrimClass::None);
    CHECK((c("/RootNode/meshes/mesh_0123456789ABCDEF") == usd::Classification{PrimClass::Mesh, 0x0123456789ABCDEFull, false}));
    CHECK(c("/RootNode/meshes/mesh_0123456789abcdef").hash == 0x0123456789ABCDEFull); // strtoull accepts lower case
    CHECK(c("/RootNode/meshes/mesh_FFFFFFFFFFFFFFFFFF").hash == ~0ull);                // overflow saturates
    CHECK(c("/RootNode/meshes/mesh_").cls == PrimClass::Unrecognized);
    CHECK(c("/RootNode/meshes/mesh_0000000000000000").cls == PrimClass::Unrecognized); // hash 0 = no replacement
    CHECK(c("/RootNode/meshes/Mesh_0000000000000001").cls == PrimClass::Unrecognized);
    CHECK(c("/RootNode/meshes/mesh_0000000000000001/mesh").cls == PrimClass::None);
    CHECK((c("/RootNode/Looks/mat_00000000000000FF") == usd::Classification{PrimClass::Material, 0xFF, false}));
    CHECK(c("/RootNode/Looks/mesh_00000000000000FF").cls == PrimClass::Unrecognized);
    CHECK((c("/RootNode/lights/light_0000000000000010") == usd::Classification{PrimClass::Light, 0x10, false}));
    CHECK((c("/RootNode/lights/sphereLight_0000000000000010") == usd::Classification{PrimClass::Light, 0x10, true}));
    CHECK(c("/RootNode/lights/spotlight_0000000000000010").cls == PrimClass::Unrecognized); // 's' -> legacy prefix only
    CHECK(std::string(usd::primClassName(PrimClass::Material)) == "material");
}

// ---- compose --------------------------------------------------------------------------------------------------

usd::ComposedStage compose(std::map<std::string, std::string> files, const std::string& root = "/m/mod.usda",
                           usd::ReadOptions opt = {}) {
    opt.files = usd::memoryFileSource(std::move(files));
    return usd::readStage(root, opt);
}

bool hasDiag(const usd::ComposedStage& s, const std::string& code, const std::string& prim = "") {
    for (const auto& d : s.diagnostics) {
        if (d.code == code && (prim.empty() || d.primPath == prim)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> targets(const usd::ComposedStage& s, const std::string& prim, const std::string& rel) {
    const usd::Prim* p = s.find(prim);
    const usd::Relationship* r = p ? p->relationship(rel) : nullptr;
    return r ? r->targets : std::vector<std::string>{"<missing>"};
}

void testListOps() {
    // Sdf applies one spec's edits as delete, add, prepend, append whatever the authored order.
    const auto s = compose({{"/m/mod.usda", R"(#usda 1.0
(
    subLayers = [@./base.usda@]
)
over "P"
{
    prepend rel r = [</x>, </y>]
}
over "Q"
{
    delete rel r = [</b>]
}
over "R"
{
    append rel r = [</a>]
}
)"},
                            {"/m/base.usda", R"(#usda 1.0
def "P"
{
    rel r = [</a>, </x>]
}
def "Q"
{
    rel r = [</a>, </b>, </c>]
}
def "R"
{
    rel r = [</a>, </b>]
}
)"}});
    CHECK(s.ok);
    CHECK((targets(s, "/P", "r") == std::vector<std::string>{"/x", "/y", "/a"}));
    CHECK((targets(s, "/Q", "r") == std::vector<std::string>{"/a", "/c"}));
    CHECK((targets(s, "/R", "r") == std::vector<std::string>{"/b", "/a"}));
    CHECK(s.count(usd::Diagnostic::Severity::Error) == 0);

    // One spec authoring "prepend" then "delete" of the same reference: delete applies first, so it stays.
    const auto r = compose({{"/m/mod.usda", R"(#usda 1.0
def "P" (
    prepend references = @./a.usda@
    delete references = @./a.usda@
)
{
}
)"},
                            {"/m/a.usda", "#usda 1.0\n(\n    defaultPrim = \"A\"\n)\ndef \"A\"\n{\n    float fromA = 1\n}\n"}});
    CHECK(r.find("/P") && r.find("/P")->attribute("fromA"));
}

void testVariantsAndPayloads() {
    const std::map<std::string, std::string> files = {{"/m/mod.usda", R"(#usda 1.0
def "P" (
    prepend variantSets = "lod"
    prepend payload = @./heavy.usda@
)
{
    variantSet "lod" = {
        "high" {
            float detail = 2
        }
        "low" {
            float detail = 1
        }
    }
}
)"},
                                                      {"/m/heavy.usda", R"(#usda 1.0
(
    defaultPrim = "H"
)
def "H"
{
    float heavy = 1
}
)"}};
    auto s = compose(files);
    const usd::Prim* p = s.find("/P");
    CHECK(p && !p->attribute("detail"));           // no selection, no fallback: nothing
    CHECK(p && p->attribute("heavy"));             // payloads load by default
    CHECK(p && p->variantSets.count("lod") && p->variantSets.at("lod").size() == 2);
    usd::ReadOptions opt;
    opt.variantFallbacks["lod"] = {"ultra", "low"};
    opt.loadPayloads = false;
    s = compose(files, "/m/mod.usda", opt);
    p = s.find("/P");
    const usd::Attribute* d = p ? p->attribute("detail") : nullptr;
    CHECK(d && d->defaultValue->number == 1.0);     // first existing fallback
    CHECK(p && p->variantSelections.at("lod") == "low");
    CHECK(p && !p->attribute("heavy"));
    CHECK(s.layers.size() == 1);                   // the payload layer was never opened
}

void testArcLimitsAndErrors() {
    // A chain of 5 references with a limit of 3.
    std::map<std::string, std::string> files;
    files["/m/mod.usda"] = "#usda 1.0\ndef \"P\" (\n    references = @./l0.usda@\n)\n{\n}\n";
    for (int i = 0; i < 5; ++i) {
        files["/m/l" + std::to_string(i) + ".usda"] = "#usda 1.0\n(\n    defaultPrim = \"A\"\n)\ndef \"A\" (\n    references = @./l" +
                                                      std::to_string(i + 1) + ".usda@\n)\n{\n    float v" + std::to_string(i) + " = 1\n}\n";
    }
    files["/m/l5.usda"] = "#usda 1.0\n(\n    defaultPrim = \"A\"\n)\ndef \"A\"\n{\n    float v5 = 1\n}\n";
    usd::ReadOptions opt;
    opt.maxArcDepth = 3;
    auto s = compose(files, "/m/mod.usda", opt);
    const usd::Prim* p = s.find("/P");
    CHECK(p && p->attribute("v0") && p->attribute("v2") && !p->attribute("v3"));
    CHECK(hasDiag(s, "arc_depth", "/P"));
    s = compose(files);
    p = s.find("/P");
    CHECK(p && p->attribute("v5") && s.count(usd::Diagnostic::Severity::Error) == 0);

    // Unparsable referenced layer: error with the prim path; the rest composes.
    s = compose({{"/m/mod.usda", "#usda 1.0\ndef \"P\" (\n    references = @./bad.usda@\n)\n{\n    float ok = 1\n}\n"},
                 {"/m/bad.usda", "#usda 1.0\ndef \"A\" {{{ this is not usda\n"}});
    CHECK(s.ok && hasDiag(s, "layer_parse_error", "/P"));
    CHECK(s.find("/P") && s.find("/P")->attribute("ok"));
    // Not a USD file at all.
    s = compose({{"/m/mod.usda", "#usda 1.0\ndef \"P\" (\n    references = @./bin.usda@\n)\n{\n}\n"}, {"/m/bin.usda", std::string("\x00\x01garbage", 9)}});
    CHECK(hasDiag(s, "layer_parse_error", "/P"));
    // Missing root layer.
    s = compose({}, "/m/none.usda");
    CHECK(!s.ok && hasDiag(s, "missing_layer") && s.prims.empty());
    // Sublayer cycle.
    s = compose({{"/m/mod.usda", "#usda 1.0\n(\n    subLayers = [@./a.usda@]\n)\ndef \"P\"\n{\n}\n"},
                 {"/m/a.usda", "#usda 1.0\n(\n    subLayers = [@./mod.usda@]\n)\ndef \"Q\"\n{\n}\n"}});
    CHECK(hasDiag(s, "sublayer_cycle") && s.find("/P") && s.find("/Q"));
}

void testNamespaceMapping() {
    // Internal references keep the root identity: targets outside the referenced prim pass through; external
    // references do not (the target is dropped with a warning).
    const auto s = compose({{"/m/mod.usda", R"(#usda 1.0
def "Proto"
{
    rel inside = </Proto/child>
    rel outside = </Other>
    def "child"
    {
    }
}
def "Inst" (
    references = </Proto>
)
{
}
def "Ext" (
    references = @./ext.usda@</E>
)
{
}
)"},
                            {"/m/ext.usda", R"(#usda 1.0
def "E"
{
    rel inside = </E/child>
    rel outside = </Other>
    def "child"
    {
    }
}
)"}});
    CHECK((targets(s, "/Inst", "inside") == std::vector<std::string>{"/Inst/child"}));
    CHECK((targets(s, "/Inst", "outside") == std::vector<std::string>{"/Other"}));
    CHECK((targets(s, "/Ext", "inside") == std::vector<std::string>{"/Ext/child"}));
    CHECK(targets(s, "/Ext", "outside").empty());
    CHECK(hasDiag(s, "target_outside_arc", "/Ext"));
    // Prim stack: strongest first, variant decorations in spec paths.
    const usd::Prim* p = s.find("/Ext/child");
    CHECK(p && p->stack.size() == 1 && p->stack[0].layer == "/m/ext.usda" && p->stack[0].path == "/E/child");
}

void testSubLayerOffsets() {
    std::string text = "#usda 1.0\n(\n    subLayers = [\n        @./a.usd@ (offset = 10; scale = 2),\n        @b(1).usd@,\n"
                       "        @./c.usd@ (scale = 0.5)\n    ]\n)\ndef \"P\" (\n    doc = \"(keep)\"\n)\n{\n}\n";
    const auto offsets = usd::stripSubLayerOffsets(text);
    CHECK(offsets.size() == 3);
    CHECK(offsets.size() == 3 && offsets[0] == std::make_pair(10.0, 2.0) && offsets[1] == std::make_pair(0.0, 1.0) &&
          offsets[2] == std::make_pair(0.0, 0.5));
    CHECK(text.find("offset") == std::string::npos && text.find("@b(1).usd@") != std::string::npos && text.find("(keep)") != std::string::npos);
    std::string plain = "#usda 1.0\n(\n    subLayers = [@./a.usd@]\n)\n";
    CHECK(usd::stripSubLayerOffsets(plain).empty());
    std::string err;
    const auto layer = usd::loadLayer("/m/x.usda", "#usda 1.0\n(\n    subLayers = [@./a.usd@ (offset = 3)]\n)\ndef \"P\"\n{\n}\n", &err);
    CHECK((layer && layer->subLayerOffsets && layer->subLayers == std::vector<std::string>{"./a.usd"}));
}

void testDeterminism() {
    const std::map<std::string, std::string> files = {
        {"/m/mod.usda", "#usda 1.0\n(\n    subLayers = [@./b.usda@]\n)\ndef \"Z\"\n{\n    float a = 1\n}\ndef \"A\"\n{\n}\n"},
        {"/m/b.usda", "#usda 1.0\ndef \"M\"\n{\n    token t = \"x\"\n}\n"}};
    const std::string d1 = usd::canonicalDump(compose(files));
    const std::string d2 = usd::canonicalDump(compose(files));
    CHECK(d1 == d2);
    CHECK(d1.find("{\"path\": \"/A\"") < d1.find("{\"path\": \"/M\"")); // sorted by path
    const auto s = compose(files);
    CHECK(s.rootPrims.size() == 3);
    CHECK(s.descendants("/").size() == 3);
}

// ---- robustness -----------------------------------------------------------------------------------------------

void testRobustness() {
    const std::string base = R"(#usda 1.0
(
    defaultPrim = "RootNode"
    subLayers = [@./sub.usda@]
)
def Xform "RootNode" (
    prepend apiSchemas = ["MaterialBindingAPI"]
    variants = { string v = "a" }
    prepend variantSets = "v"
)
{
    rel material:binding = </RootNode/Looks/mat_0000000000000001>
    point3f[] points = [(0, 0, 0), (1, 2, 3)]
    float x.timeSamples = { 0: 1, 1: 2 }
    variantSet "v" = {
        "a" { float y = 1 }
    }
    def Mesh "mesh_0000000000000002" (
        references = @./sub.usda@</S>
    )
    {
        asset tex = @./t.dds@
    }
}
)";
    const std::string sub = "#usda 1.0\ndef \"S\"\n{\n    int[] i = [1, 2, 3]\n}\n";
    std::uint32_t rng = 12345;
    auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    int parsed = 0, rejected = 0;
    for (int iter = 0; iter < 300; ++iter) {
        std::string m = base;
        const int kind = iter % 3;
        if (kind == 0) { // truncate
            m.resize(next() % m.size());
        } else if (kind == 1) { // flip bytes
            for (int k = 0; k < 3; ++k) {
                m[next() % m.size()] = static_cast<char>(next() & 0xff);
            }
        } else { // delete a span
            const std::size_t at = next() % m.size();
            m.erase(at, std::min<std::size_t>(m.size() - at, 1 + next() % 16));
        }
        const auto s = compose({{"/m/mod.usda", m}, {"/m/sub.usda", sub}});
        (void)usd::canonicalDump(s);
        (void)usd::collectRemixMod(s);
        (s.ok ? parsed : rejected) += 1;
    }
    std::printf("robustness: %d mutated layers parsed, %d rejected\n", parsed, rejected);
    CHECK(parsed + rejected == 300);
    CHECK(rejected > 0);
}

} // namespace

int main() {
    testValues();
    testPaths();
    testClassify();
    testListOps();
    testVariantsAndPayloads();
    testArcLimitsAndErrors();
    testNamespaceMapping();
    testSubLayerOffsets();
    testDeterminism();
    testRobustness();
    std::printf("rl_mod_usd_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
