// Gate remaster_tscript_unit (plan §1.2 Track B, W1.6): snippet tests for the TorqueScript lexer / parser
// (every statement and expression form), error recovery diagnostics (line:col), call graph edges, datablock
// parent chains and JSON dump determinism. Plain main(); exits nonzero if any CHECK fails.
#include "fuse/remaster/tscript/json.hpp"

#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using namespace fuse::remaster::tscript;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        ++g_checks;                                                                                     \
        if (!(cond)) {                                                                                  \
            ++g_failures;                                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        ++g_checks;                                                                                     \
        const auto& va_ = (a);                                                                          \
        const auto& vb_ = (b);                                                                          \
        if (!(va_ == vb_)) {                                                                            \
            ++g_failures;                                                                               \
            std::printf("FAIL %s:%d: %s == %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #a, \
                        #b, to_s(va_).c_str(), to_s(vb_).c_str());                                      \
        }                                                                                               \
    } while (0)

std::string to_s(const std::string& s) { return s; }
std::string to_s(std::string_view s) { return std::string(s); }
std::string to_s(const char* s) { return s; }
template <class T> std::string to_s(const T& v) { return std::to_string(v); }

// Compact S-expression of an AST node: (Kind text kids...)
std::string sx(const Node& n) {
    std::string s = "(" + std::string(kind_name(n.kind));
    if (!n.text.empty()) s += " " + n.text;
    for (const auto& k : n.kids) s += " " + sx(*k);
    return s + ")";
}

void dump_diags(const ParseResult& r) {
    for (const auto& d : r.diags) std::printf("  diag %s:%u:%u: %s\n", d.file.c_str(), d.line, d.col, d.message.c_str());
}

ParseResult ok_parse(const std::string& src) {
    auto r = parse(src, "t.tscript");
    if (!r.ok()) {
        ++g_failures;
        std::printf("FAIL unexpected diagnostics for: %s\n", src.c_str());
        dump_diags(r);
    }
    return r;
}

// S-expression of the single statement in `src`.
std::string stmt(const std::string& src) {
    const auto r = ok_parse(src);
    if (r.root->kids.size() != 1) return "<" + std::to_string(r.root->kids.size()) + " statements>";
    return sx(r.root->kid(0));
}

// S-expression of the expression in the single expression statement `src;`.
std::string expr(const std::string& src) {
    const auto r = ok_parse(src + ";");
    if (r.root->kids.size() != 1 || r.root->kid(0).kind != Kind::ExprStmt) return "<not one expression>";
    return sx(r.root->kid(0).kid(0));
}

void test_lexer() {
    std::vector<Diagnostic> d;
    const auto toks = lex("\xEF\xBB\xBF%a $pref::Video::mode Foo::bar 0x1F .5 1.5e3 7 \"s\\\"x\" 'tag' !$= --> // c\n/* x\n */ @",
                          "t", d);
    CHECK(d.empty());
    CHECK_EQ(toks.size(), std::size_t{13});
    CHECK(toks[0].kind == TokKind::LocalVar && toks[0].text == "%a");
    CHECK(toks[0].line == 1 && toks[0].col == 1);
    CHECK(toks[1].kind == TokKind::GlobalVar && toks[1].text == "$pref::Video::mode");
    CHECK(toks[2].kind == TokKind::Ident && toks[2].text == "Foo::bar");
    CHECK(toks[3].kind == TokKind::Int && toks[3].text == "0x1F");
    CHECK(toks[4].kind == TokKind::Float && toks[4].text == ".5");
    CHECK(toks[5].kind == TokKind::Float && toks[5].text == "1.5e3");
    CHECK(toks[6].kind == TokKind::Int);
    CHECK(toks[7].kind == TokKind::String && toks[7].text == "s\\\"x");
    CHECK(toks[8].kind == TokKind::Tagged && toks[8].text == "tag");
    CHECK(toks[9].kind == TokKind::Punct && toks[9].text == "!$=");
    CHECK(toks[10].kind == TokKind::Punct && toks[10].text == "-->");
    CHECK(toks[11].kind == TokKind::Punct && toks[11].text == "@" && toks[11].line == 3 && toks[11].col == 5);
    CHECK(toks[12].kind == TokKind::End);

    d.clear();
    const auto t2 = lex("switch$ ($a) foreach$(%x in $set) %b % %c %d %= 2", "t", d);
    CHECK(d.empty());
    CHECK(t2[0].text == "switch$" && t2[2].kind == TokKind::GlobalVar);
    CHECK(t2[4].text == "foreach$");
    CHECK(t2[11].kind == TokKind::Punct && t2[11].text == "%"); // modulo, not a variable
    CHECK(t2[14].text == "%=");

    d.clear();
    (void)lex("%a = \"open;\n# x", "f.cs", d);
    CHECK_EQ(d.size(), std::size_t{2});
    if (d.size() == 2) {
        CHECK(d[0].line == 1 && d[0].col == 6 && d[0].message == "unterminated string");
        CHECK(d[1].line == 2 && d[1].col == 1 && d[1].file == "f.cs");
    }
}

void test_expressions() {
    CHECK_EQ(expr("%a = 1 + 2 * 3"), "(Assign = (LocalVar %a) (Binary + (Int 1) (Binary * (Int 2) (Int 3))))");
    CHECK_EQ(expr("%a = %b = 2"), "(Assign = (LocalVar %a) (Assign = (LocalVar %b) (Int 2)))");
    CHECK_EQ(expr("$x += 1"), "(Assign += (GlobalVar $x) (Int 1))");
    for (const char* op : {"-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}) {
        CHECK_EQ(expr(std::string("%v ") + op + " 3"), std::string("(Assign ") + op + " (LocalVar %v) (Int 3))");
    }
    CHECK_EQ(expr("%a = %b ? 1 : %c ? 2 : 3"),
             "(Assign = (LocalVar %a) (Ternary (LocalVar %b) (Int 1) (Ternary (LocalVar %c) (Int 2) (Int 3))))");
    CHECK_EQ(expr("%a || %b && %c"), "(Binary || (LocalVar %a) (Binary && (LocalVar %b) (LocalVar %c)))");
    CHECK_EQ(expr("%a | %b ^ %c & %d"),
             "(Binary | (LocalVar %a) (Binary ^ (LocalVar %b) (Binary & (LocalVar %c) (LocalVar %d))))");
    CHECK_EQ(expr("%a == %b < %c"), "(Binary == (LocalVar %a) (Binary < (LocalVar %b) (LocalVar %c)))");
    CHECK_EQ(expr("%a != 1 && %b <= 2 || %c >= 3 && %d > 4"),
             "(Binary || (Binary && (Binary != (LocalVar %a) (Int 1)) (Binary <= (LocalVar %b) (Int 2))) "
             "(Binary && (Binary >= (LocalVar %c) (Int 3)) (Binary > (LocalVar %d) (Int 4))))");
    CHECK_EQ(expr("%s $= \"x\" && %t !$= \"y\""),
             "(Binary && (Binary $= (LocalVar %s) (String x)) (Binary !$= (LocalVar %t) (String y)))");
    CHECK_EQ(expr("\"a\" @ %b SPC %c TAB %d NL \"e\""),
             "(Binary NL (Binary TAB (Binary SPC (Binary @ (String a) (LocalVar %b)) (LocalVar %c)) (LocalVar %d)) "
             "(String e))");
    CHECK_EQ(expr("%a @ 1 + 2"), "(Binary @ (LocalVar %a) (Binary + (Int 1) (Int 2)))");
    CHECK_EQ(expr("%a << 1 + 2 >> 3"),
             "(Binary >> (Binary << (LocalVar %a) (Binary + (Int 1) (Int 2))) (Int 3))");
    CHECK_EQ(expr("%a - %b - %c"), "(Binary - (Binary - (LocalVar %a) (LocalVar %b)) (LocalVar %c))");
    CHECK_EQ(expr("%a / %b % 2"), "(Binary % (Binary / (LocalVar %a) (LocalVar %b)) (Int 2))");
    CHECK_EQ(expr("!%a && ~%b || -%c"),
             "(Binary || (Binary && (Unary ! (LocalVar %a)) (Unary ~ (LocalVar %b))) (Unary - (LocalVar %c)))");
    CHECK_EQ(expr("%i++"), "(PostIncDec ++ (LocalVar %i))");
    CHECK_EQ(expr("$c--"), "(PostIncDec -- (GlobalVar $c))");
    CHECK_EQ(expr("++%i"), "(PreIncDec ++ (LocalVar %i))");
    CHECK_EQ(expr("(1 + 2) * 3"), "(Binary * (Binary + (Int 1) (Int 2)) (Int 3))");
    CHECK_EQ(expr("%f = 0x10 + .5 + 1.5e-3 + 'tag' + true"),
             "(Assign = (LocalVar %f) (Binary + (Binary + (Binary + (Binary + (Int 0x10) (Float .5)) (Float 1.5e-3)) "
             "(TaggedString tag)) (Bool true)))");
    CHECK_EQ(expr("%s = \"q\\\"\\n\\c1\\x41\""), "(Assign = (LocalVar %s) (String q\\\"\\n\\c1\\x41))");
    CHECK_EQ(expr("$a[%i, %j] = 5"), "(Assign = (Index (GlobalVar $a) (LocalVar %i) (LocalVar %j)) (Int 5))");
    CHECK_EQ(expr("%obj.field[2] = 1"), "(Assign = (Index (Field field (LocalVar %obj)) (Int 2)) (Int 1))");
    CHECK_EQ(expr("%obj.datablock = %this.class"),
             "(Assign = (Field datablock (LocalVar %obj)) (Field class (LocalVar %this)))");
    CHECK_EQ(expr("%obj.method(1, \"two\")"), "(MethodCall method (LocalVar %obj) (Int 1) (String two))");
    CHECK_EQ(expr("Canvas.getContent().getId()"), "(MethodCall getId (MethodCall getContent (Ident Canvas)))");
    CHECK_EQ(expr("Ns::fn(%a)"), "(Call Ns::fn (LocalVar %a))");
    CHECK_EQ(expr("Parent::onAdd(%this)"), "(Call Parent::onAdd (LocalVar %this))");
    CHECK_EQ(expr("foo()"), "(Call foo)");
    CHECK_EQ(expr("%this-->Scroll.setVisible(false)"),
             "(MethodCall setVisible (InternalName -->Scroll (LocalVar %this)) (Bool false))");
    CHECK_EQ(expr("%vp = %this->hor"), "(Assign = (LocalVar %vp) (InternalName ->hor (LocalVar %this)))");
    CHECK_EQ(expr("%x = SomeObject"), "(Assign = (LocalVar %x) (Ident SomeObject))");
    CHECK_EQ(expr("%this.schedule(100, \"tick\")"), "(MethodCall schedule (LocalVar %this) (Int 100) (String tick))");
    CHECK_EQ(expr("assert(isObject(%o), \"msg\")"), "(Call assert (Call isObject (LocalVar %o)) (String msg))");
}

void test_objects() {
    CHECK_EQ(expr("%o = new ScriptObject(Foo : Bar) { a = 1; b[2, 3] = \"x\"; datablock = \"y\"; class = Foo; "
                  "new SimObject(Child) { }; }"),
             "(Assign = (LocalVar %o) (ObjectDecl new (Ident ScriptObject) (Ident Foo) (Ident Bar) (List args) "
             "(List body (FieldAssign a (List index) (Int 1)) (FieldAssign b (List index (Int 2) (Int 3)) (String x)) "
             "(FieldAssign datablock (List index) (String y)) (FieldAssign class (List index) (Ident Foo)) "
             "(ObjectDecl new (Ident SimObject) (Ident Child) (None) (List args) (List body)))))");
    CHECK_EQ(expr("new (%cls)(%name)"),
             "(ObjectDecl new (LocalVar %cls) (LocalVar %name) (None) (List args) (None))");
    CHECK_EQ(expr("new SimSet()"), "(ObjectDecl new (Ident SimSet) (None) (None) (List args) (None))");
    CHECK_EQ(expr("new GuiControl(\"Name\" @ %i, 1, 2)"),
             "(ObjectDecl new (Ident GuiControl) (Binary @ (String Name) (LocalVar %i)) (None) (List args (Int 1) "
             "(Int 2)) (None))");
    CHECK_EQ(expr("new Foo(: Tmpl)"), "(ObjectDecl new (Ident Foo) (None) (Ident Tmpl) (List args) (None))");
    // object statements: trailing ';' optional after a body
    CHECK_EQ(stmt("datablock ItemData(Gun : BaseItem) { shapeFile = \"gun.dts\"; mass = 2; }"),
             "(ExprStmt (ObjectDecl datablock (Ident ItemData) (Ident Gun) (Ident BaseItem) (List args) (List body "
             "(FieldAssign shapeFile (List index) (String gun.dts)) (FieldAssign mass (List index) (Int 2)))))");
    CHECK_EQ(stmt("singleton Material(Mat) { diffuseColor[0] = \"1 1 1 1\" };"),
             "(ExprStmt (ObjectDecl singleton (Ident Material) (Ident Mat) (None) (List args) (List body "
             "(FieldAssign diffuseColor (List index (Int 0)) (String 1 1 1 1)))))");
    CHECK_EQ(stmt("new SimGroup(G) { new SimSet(S); };"),
             "(ExprStmt (ObjectDecl new (Ident SimGroup) (Ident G) (None) (List args) (List body "
             "(ObjectDecl new (Ident SimSet) (Ident S) (None) (List args) (None)))))");
}

void test_statements() {
    CHECK_EQ(stmt("function f(%a, %b = 2) { return %a + %b; }"),
             "(FunctionDecl f (List params (Param %a) (Param %b (Int 2))) (Block (Return (Binary + (LocalVar %a) "
             "(LocalVar %b)))))");
    CHECK_EQ(stmt("function Ns::m(%this) { return; }"),
             "(FunctionDecl Ns::m (List params (Param %this)) (Block (Return)))");
    CHECK_EQ(stmt("package P { function a() {} function B::c() {} };"),
             "(PackageDecl P (FunctionDecl a (List params) (Block)) (FunctionDecl B::c (List params) (Block)))");
    CHECK_EQ(stmt("package Q { function a() {} }"), "(PackageDecl Q (FunctionDecl a (List params) (Block)))");
    CHECK_EQ(stmt("if (%a) b(); else if (%c) { d(); } else e();"),
             "(If (LocalVar %a) (ExprStmt (Call b)) (If (LocalVar %c) (Block (ExprStmt (Call d))) "
             "(ExprStmt (Call e))))");
    CHECK_EQ(stmt("if (%a) {}"), "(If (LocalVar %a) (Block) (None))");
    CHECK_EQ(stmt("while (%i < 3) %i++;"),
             "(While (Binary < (LocalVar %i) (Int 3)) (ExprStmt (PostIncDec ++ (LocalVar %i))))");
    CHECK_EQ(stmt("do { %i--; } while (%i);"),
             "(DoWhile (Block (ExprStmt (PostIncDec -- (LocalVar %i)))) (LocalVar %i))");
    CHECK_EQ(stmt("for (%i = 0; %i < 10; %i++) { continue; }"),
             "(For (Assign = (LocalVar %i) (Int 0)) (Binary < (LocalVar %i) (Int 10)) (PostIncDec ++ (LocalVar %i)) "
             "(Block (Continue)))");
    CHECK_EQ(stmt("for (;;) break;"), "(For (None) (None) (None) (Break))");
    CHECK_EQ(stmt("foreach (%o in %set) %o.delete();"),
             "(Foreach foreach (LocalVar %o) (LocalVar %set) (ExprStmt (MethodCall delete (LocalVar %o))))");
    CHECK_EQ(stmt("foreach$ (%w in \"a b\") echo(%w);"),
             "(Foreach foreach$ (LocalVar %w) (String a b) (ExprStmt (Call echo (LocalVar %w))))");
    CHECK_EQ(stmt("switch$ (%s) { case \"a\" or \"b\": x(); y(); case \"c\": default: z(); }"),
             "(Switch switch$ (LocalVar %s) (Case case (List values (String a) (String b)) (Block (ExprStmt (Call x)) "
             "(ExprStmt (Call y)))) (Case case (List values (String c)) (Block)) (Case default (List values) "
             "(Block (ExprStmt (Call z)))))");
    CHECK_EQ(stmt("switch (%n) { case 1: return 2; default: break; }"),
             "(Switch switch (LocalVar %n) (Case case (List values (Int 1)) (Block (Return (Int 2)))) "
             "(Case default (List values) (Block (Break))))");
    CHECK_EQ(stmt(";"), "(Empty)");
    CHECK_EQ(stmt("{ %a = 1; ; }"), "(Block (ExprStmt (Assign = (LocalVar %a) (Int 1))) (Empty))");

    // locations
    const auto r = ok_parse("\n  function f()\n  {\n     %x = 1;\n  }\n");
    CHECK(r.root->kids.size() == 1);
    if (r.root->kids.size() == 1) {
        const Node& f = r.root->kid(0);
        CHECK(f.line == 2 && f.col == 3);
        const Node& s = f.kid(1).kid(0);
        CHECK(s.line == 4 && s.col == 6);
        CHECK_EQ(r.slice(s), std::string_view("%x = 1;"));
        CHECK_EQ(r.slice(f.kid(1)), std::string_view("{\n     %x = 1;\n  }"));
    }
}

void test_error_recovery() {
    auto r = parse("function f()\n{\n   %a = ;\n   %b = 2;\n}\nfunction g() { h(); }\n", "bad.tscript");
    CHECK_EQ(r.diags.size(), std::size_t{1});
    if (!r.diags.empty()) {
        CHECK_EQ(r.diags[0].file, std::string("bad.tscript"));
        CHECK_EQ(r.diags[0].line, 3u);
        CHECK_EQ(r.diags[0].col, 9u);
        CHECK(r.diags[0].message.find("expected expression") != std::string::npos);
    }
    CHECK_EQ(r.root->kids.size(), std::size_t{2}); // both functions survive
    if (r.root->kids.size() == 2) {
        CHECK_EQ(r.root->kid(0).kid(1).kids.size(), std::size_t{1}); // %b = 2 kept
        CHECK_EQ(r.root->kid(1).text, std::string("g"));
    }

    r = parse("%x = 1\n%y = 2;\nfoo();", "t");
    CHECK_EQ(r.diags.size(), std::size_t{1});
    if (!r.diags.empty()) CHECK(r.diags[0].line == 2 && r.diags[0].col == 1);

    r = parse("new Foo(Bar) { a = 1; b = ; c = 3; };\n}\nfunction ok() {}", "t");
    CHECK_EQ(r.diags.size(), std::size_t{2});
    if (r.diags.size() == 2) {
        CHECK(r.diags[0].line == 1 && r.diags[0].col == 27);
        CHECK(r.diags[1].line == 2 && r.diags[1].col == 1); // stray '}'
    }
    CHECK(!r.root->kids.empty() && r.root->kids.back()->text == "ok");

    r = parse("function f( { }", "t");
    CHECK(!r.diags.empty());
    r = parse("if (%a { b(); }\nc();", "t");
    CHECK(!r.diags.empty());
    CHECK(!r.root->kids.empty());
    r = parse("1 = 2;", "t");
    CHECK_EQ(r.diags.size(), std::size_t{1});
    if (!r.diags.empty()) CHECK(r.diags[0].message.find("not assignable") != std::string::npos);
    r = parse("function f() { %s = \"oops; }\n", "t");
    CHECK(!r.diags.empty());
}

const CallSite* find_call(const FileAnalysis& fa, std::string_view caller, std::string_view callee) {
    for (const auto& c : fa.calls) {
        if (c.caller == caller && c.callee == callee) return &c;
    }
    return nullptr;
}

void test_call_graph() {
    const auto r = ok_parse(
        "init();\n"
        "function Player::onAdd(%this) { Parent::onAdd(%this); %this.setup(); Game.start(); Util::log(1); }\n"
        "package Mod {\n"
        "  function Player::onAdd(%this) { Parent::onAdd(%this); helper(); }\n"
        "  function helper() { Parent::helper(); }\n"
        "};\n"
        "function helper() { echo(\"x\"); }\n");
    const auto fa = analyze(r);
    CHECK_EQ(fa.functions.size(), std::size_t{4});
    CHECK_EQ(fa.packages.size(), std::size_t{1});
    if (fa.functions.size() == 4) {
        CHECK(fa.functions[0].key == "Player::onAdd" && fa.functions[0].package.empty() && fa.functions[0].line == 2);
        CHECK(fa.functions[1].key == "Player::onAdd" && fa.functions[1].package == "Mod");
        CHECK(fa.functions[0].params.size() == 1 && fa.functions[0].params[0] == "%this");
    }
    const CallSite* c = find_call(fa, "<file>", "init");
    CHECK(c && c->kind == CallKind::Plain && c->line == 1 && c->col == 1);
    c = find_call(fa, "Player::onAdd", "parent::onAdd");
    CHECK(c && c->kind == CallKind::Parent && !c->resolved && c->receiver == "Player");
    c = find_call(fa, "Player::onAdd", ".setup");
    CHECK(c && c->kind == CallKind::Method && c->receiver.empty());
    c = find_call(fa, "Player::onAdd", ".start");
    CHECK(c && c->receiver == "Game");
    c = find_call(fa, "Player::onAdd", "Util::log");
    CHECK(c && c->kind == CallKind::Namespace);
    // inside the package, Parent:: resolves to the overridden definition
    int resolved_parent = 0;
    for (const auto& cs : fa.calls) {
        if (cs.kind == CallKind::Parent && cs.resolved) {
            ++resolved_parent;
            CHECK(cs.callee == "Player::onAdd" || cs.callee == "helper");
        }
    }
    CHECK_EQ(resolved_parent, 2);

    const auto r2 = ok_parse("function Util::log(%x) { echo(%x); }\nfunction PLAYER::onAdd() {}\n");
    const auto r2b = parse(r2.source, "u.tscript");
    const auto g = build_call_graph({fa, analyze(r2b)});
    CHECK(g.definitions.contains("player::onadd"));
    if (g.definitions.contains("player::onadd")) CHECK_EQ(g.definitions.at("player::onadd").size(), std::size_t{3});
    CHECK(g.definitions.contains("util::log"));
    CHECK(g.unresolved.contains("init"));
    CHECK(g.unresolved.contains("echo"));
    if (g.unresolved.contains("echo")) CHECK_EQ(g.unresolved.at("echo"), 2u);
    CHECK(!g.unresolved.contains("util::log"));
    CHECK(!g.unresolved.contains("helper"));
    CHECK_EQ(g.unresolved_parent_calls, 1u);
    CHECK(g.packages.contains("Mod"));
    if (g.packages.contains("Mod")) CHECK_EQ(g.packages.at("Mod").size(), std::size_t{2});
    for (std::size_t i = 1; i < g.edges.size(); ++i) {
        const auto& a = g.edges[i - 1];
        const auto& b = g.edges[i];
        CHECK(std::tie(a.file, a.line, a.col) <= std::tie(b.file, b.line, b.col));
    }
}

void test_datablocks() {
    const auto r1 = ok_parse(
        "datablock PlayerData(BaseData) { mass = 1; };\n"
        "datablock PlayerData(MidData : basedata) { mass = 2; maxDamage[1] = 3 + 4; };\n"
        "singleton Material(Leaf : MIDDATA) { };\n"
        "datablock ItemData(Orphan : NoSuchParent) {};\n"
        "new ScriptObject(NamedThing) { new SimSet(Inner) {}; new SimSet() {}; };\n"
        "new SimSet();\n"
        "function f() { new ScriptObject(InFunction); datablock X(Y) {}; }\n");
    const auto fa = analyze(r1);
    CHECK_EQ(fa.objects.size(), std::size_t{7});
    if (fa.objects.size() == 7) {
        const auto& mid = fa.objects[1];
        CHECK(mid.kind == "datablock" && mid.cls == "PlayerData" && mid.name == "MidData" && mid.parent == "basedata");
        CHECK_EQ(mid.fields.size(), std::size_t{2});
        if (mid.fields.size() == 2) {
            CHECK(mid.fields[1].name == "maxDamage" && mid.fields[1].index == "1" && mid.fields[1].value == "3 + 4");
        }
        CHECK(fa.objects[2].kind == "singleton");
        CHECK(fa.objects[4].kind == "new" && fa.objects[4].name == "NamedThing" && fa.objects[4].file_scope());
        CHECK(fa.objects[5].name == "Inner" && fa.objects[5].container == "NamedThing");
        CHECK(fa.objects[6].kind == "datablock" && fa.objects[6].function == "f");
    }
    const auto t = build_datablock_tree({fa});
    CHECK_EQ(t.nodes.size(), std::size_t{7});
    if (t.nodes.size() == 7) {
        CHECK_EQ(t.nodes[1].parent_index, 0);
        CHECK_EQ(t.nodes[2].parent_index, 1); // case-insensitive chain Leaf -> MidData -> BaseData
        CHECK(t.nodes[0].children == std::vector<int>{1});
        CHECK(t.nodes[0].parent_index == -1);
    }
    CHECK_EQ(t.missing_parents.size(), std::size_t{1});
    if (!t.missing_parents.empty()) {
        CHECK(t.missing_parents[0].child == "Orphan" && t.missing_parents[0].parent == "NoSuchParent");
        CHECK_EQ(t.missing_parents[0].line, 4u);
    }
    CHECK_EQ(t.roots.size(), std::size_t{5}); // BaseData, Orphan, NamedThing, Inner, Y

    // parents across files link regardless of add order
    const auto a = parse("datablock A(Child : Root) {};", "b.tscript");
    const auto b = parse("datablock A(Root) {};", "a.tscript");
    const auto t2 = build_datablock_tree({analyze(a), analyze(b)});
    CHECK(t2.nodes.size() == 2 && t2.nodes[0].def.name == "Root" && t2.nodes[1].parent_index == 0);
}

void test_json() {
    const std::string src =
        "// header\nfunction Foo::bar(%a) { %s = \"q\\\"\" @ %a; if (%a) Foo::bar(%a - 1); %a.baz(); }\n"
        "datablock D(N : P) { f[0] = \"x\\ty\"; };\n";
    const auto r1 = parse(src, "j.tscript");
    const auto r2 = parse(src, "j.tscript");
    CHECK(r1.ok());
    const auto j1 = ast_to_json(*r1.root);
    CHECK_EQ(j1, ast_to_json(*r2.root));
    CHECK_EQ(analysis_to_json(analyze(r1)), analysis_to_json(analyze(r2)));
    CHECK(j1.starts_with("{\n  \"kind\": \"Script\",\n  \"line\": 2,\n  \"col\": 1,\n  \"kids\": [\n"));
    CHECK(j1.find("\"text\": \"q\\\\\\\"\"") != std::string::npos); // raw q\" escaped for JSON
    CHECK(j1.find('\r') == std::string::npos);

    const auto f1 = analyze(parse("function a() { b(); }\ndatablock X(Y : Z) {};", "1.tscript"));
    const auto f2 = analyze(parse("function b() { a(); c(); }\ndatablock X(Z) {};", "2.tscript"));
    CHECK_EQ(call_graph_to_json(build_call_graph({f1, f2})), call_graph_to_json(build_call_graph({f2, f1})));
    CHECK_EQ(datablock_tree_to_json(build_datablock_tree({f1, f2})),
             datablock_tree_to_json(build_datablock_tree({f2, f1})));
    const auto cg = call_graph_to_json(build_call_graph({f1, f2}));
    CHECK(cg.find("\"unresolved\": {\n    \"c\": 1\n  }") != std::string::npos);
    CHECK_EQ(diagnostics_to_json({}), std::string("[]\n"));
    CHECK_EQ(diagnostics_to_json({Diagnostic{"f", 1, 2, "m"}}),
             std::string("[\n  {\n    \"file\": \"f\",\n    \"line\": 1,\n    \"col\": 2,\n    \"message\": \"m\"\n  }\n]\n"));
}

} // namespace

int main() {
    test_lexer();
    test_expressions();
    test_objects();
    test_statements();
    test_error_recovery();
    test_call_graph();
    test_datablocks();
    test_json();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
