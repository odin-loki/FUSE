// TorqueScript parser (plan §1.2 Track B, W1.6): tokens -> AST with error recovery. See parser.hpp.
#include "fuse/remaster/tscript/parser.hpp"

#include <fstream>
#include <sstream>

namespace fuse::remaster::tscript {

std::string_view kind_name(Kind k) noexcept {
    switch (k) {
    case Kind::Script: return "Script";
    case Kind::FunctionDecl: return "FunctionDecl";
    case Kind::Param: return "Param";
    case Kind::PackageDecl: return "PackageDecl";
    case Kind::List: return "List";
    case Kind::None: return "None";
    case Kind::Block: return "Block";
    case Kind::If: return "If";
    case Kind::While: return "While";
    case Kind::DoWhile: return "DoWhile";
    case Kind::For: return "For";
    case Kind::Foreach: return "Foreach";
    case Kind::Switch: return "Switch";
    case Kind::Case: return "Case";
    case Kind::Break: return "Break";
    case Kind::Continue: return "Continue";
    case Kind::Return: return "Return";
    case Kind::ExprStmt: return "ExprStmt";
    case Kind::Empty: return "Empty";
    case Kind::ObjectDecl: return "ObjectDecl";
    case Kind::FieldAssign: return "FieldAssign";
    case Kind::Int: return "Int";
    case Kind::Float: return "Float";
    case Kind::String: return "String";
    case Kind::TaggedString: return "TaggedString";
    case Kind::Bool: return "Bool";
    case Kind::Ident: return "Ident";
    case Kind::LocalVar: return "LocalVar";
    case Kind::GlobalVar: return "GlobalVar";
    case Kind::Index: return "Index";
    case Kind::Field: return "Field";
    case Kind::InternalName: return "InternalName";
    case Kind::Call: return "Call";
    case Kind::MethodCall: return "MethodCall";
    case Kind::Unary: return "Unary";
    case Kind::PreIncDec: return "PreIncDec";
    case Kind::PostIncDec: return "PostIncDec";
    case Kind::Binary: return "Binary";
    case Kind::Assign: return "Assign";
    case Kind::Ternary: return "Ternary";
    }
    return "?";
}

std::string_view ParseResult::slice(const Node& n) const {
    if (n.begin >= source.size() || n.end <= n.begin) return {};
    return std::string_view(source).substr(n.begin, n.end - n.begin);
}

namespace {

struct ParseError {}; // thrown after a diagnostic was recorded; caught at statement level for recovery

constexpr std::size_t kMaxDiagnostics = 200;

bool is_assign_op(std::string_view op) noexcept {
    return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" || op == "&=" ||
           op == "|=" || op == "^=" || op == "<<=" || op == ">>=";
}

// Binary precedence (CMDgram.y); 0 = not a binary operator.
int binary_prec(const Token& t) noexcept {
    const auto s = t.text;
    if (t.kind == TokKind::Ident) return (s == "SPC" || s == "TAB" || s == "NL") ? 8 : 0;
    if (t.kind != TokKind::Punct) return 0;
    if (s == "||") return 1;
    if (s == "&&") return 2;
    if (s == "|") return 3;
    if (s == "^") return 4;
    if (s == "&") return 5;
    if (s == "==" || s == "!=") return 6;
    if (s == "<" || s == ">" || s == "<=" || s == ">=") return 7;
    if (s == "@" || s == "$=" || s == "!$=") return 8;
    if (s == "<<" || s == ">>") return 9;
    if (s == "+" || s == "-") return 10;
    if (s == "*" || s == "/" || s == "%") return 11;
    return 0;
}

class Parser {
public:
    Parser(std::vector<Token> toks, std::string_view file, std::vector<Diagnostic>& diags)
        : toks_(std::move(toks)), file_(file), diags_(diags) {}

    NodePtr script() {
        auto root = node(Kind::Script, cur());
        while (!at_end()) {
            const std::size_t before = pos_;
            if (is_punct("}")) {
                error(cur(), "unexpected '}' at file scope");
                ++pos_;
                continue;
            }
            if (auto s = statement_or_decl(true)) root->kids.push_back(std::move(s));
            if (pos_ == before) ++pos_; // guarantee progress
        }
        finish(*root);
        if (!root->kids.empty()) root->end = root->kids.back()->end;
        return root;
    }

private:
    // ---- token helpers ----
    [[nodiscard]] const Token& cur() const noexcept { return toks_[pos_]; }
    [[nodiscard]] const Token& ahead(std::size_t n) const noexcept {
        return toks_[pos_ + n < toks_.size() ? pos_ + n : toks_.size() - 1];
    }
    [[nodiscard]] bool at_end() const noexcept { return cur().kind == TokKind::End; }
    [[nodiscard]] bool is_punct(std::string_view p) const noexcept {
        return cur().kind == TokKind::Punct && cur().text == p;
    }
    [[nodiscard]] bool is_word(std::string_view w) const noexcept {
        return cur().kind == TokKind::Ident && cur().text == w;
    }
    static bool tok_is(const Token& t, std::string_view p) noexcept {
        return t.kind == TokKind::Punct && t.text == p;
    }
    bool accept(std::string_view p) {
        if (!is_punct(p)) return false;
        ++pos_;
        return true;
    }
    const Token& expect(std::string_view p, std::string_view what = {}) {
        if (!is_punct(p)) {
            std::string msg = "expected '" + std::string(p) + "'";
            if (!what.empty()) msg += " " + std::string(what);
            fail(cur(), std::move(msg));
        }
        return toks_[pos_++];
    }
    std::string describe(const Token& t) const {
        switch (t.kind) {
        case TokKind::End: return "end of file";
        case TokKind::String: return "string";
        case TokKind::Tagged: return "tagged string";
        default: return "'" + std::string(t.text) + "'";
        }
    }
    void error(const Token& t, std::string msg) {
        if (diags_.size() < kMaxDiagnostics) {
            diags_.push_back(Diagnostic{std::string(file_), t.line, t.col, msg + " (found " + describe(t) + ")"});
        }
    }
    [[noreturn]] void fail(const Token& t, std::string msg) {
        error(t, std::move(msg));
        throw ParseError{};
    }

    // ---- node helpers ----
    NodePtr node(Kind k, const Token& at, std::string text = {}) const {
        auto n = std::make_unique<Node>();
        n->kind = k;
        n->text = std::move(text);
        n->line = at.line;
        n->col = at.col;
        n->begin = at.begin;
        n->end = at.end;
        return n;
    }
    NodePtr none() const {
        auto n = node(Kind::None, cur());
        n->end = n->begin;
        return n;
    }
    // Sets the node's end to the end of the last consumed token.
    void finish(Node& n) const {
        if (pos_ > 0 && toks_[pos_ - 1].end > n.begin) n.end = toks_[pos_ - 1].end;
    }

    // Skips to just past the next ';' or to (not past) the next '}' at the current brace depth.
    void recover() {
        int depth = 0;
        while (!at_end()) {
            if (is_punct("{")) {
                ++depth;
            } else if (is_punct("}")) {
                if (depth == 0) return;
                if (--depth == 0) {
                    ++pos_;
                    accept(";");
                    return;
                }
            } else if (is_punct(";") && depth == 0) {
                ++pos_;
                return;
            }
            ++pos_;
        }
    }

    // ---- statements ----
    NodePtr statement_or_decl(bool top_level) {
        const std::size_t start = pos_;
        try {
            if (is_word("function")) return function_decl();
            if (is_word("package") && ahead(1).kind == TokKind::Ident && top_level) return package_decl();
            return statement();
        } catch (const ParseError&) {
            if (pos_ == start) ++pos_;
            recover();
            return nullptr;
        }
    }

    NodePtr function_decl() {
        auto fn = node(Kind::FunctionDecl, cur());
        ++pos_; // 'function'
        if (cur().kind != TokKind::Ident) fail(cur(), "expected function name");
        fn->text = std::string(cur().text);
        ++pos_;
        auto params = node(Kind::List, cur(), "params");
        expect("(", "after function name");
        while (!is_punct(")")) {
            if (cur().kind != TokKind::LocalVar) fail(cur(), "expected parameter '%name'");
            auto p = node(Kind::Param, cur(), std::string(cur().text));
            ++pos_;
            if (accept("=")) p->kids.push_back(expression());
            finish(*p);
            params->kids.push_back(std::move(p));
            if (!accept(",")) break;
        }
        expect(")", "after parameters");
        finish(*params);
        fn->kids.push_back(std::move(params));
        if (!is_punct("{")) fail(cur(), "expected '{' to start function body");
        fn->kids.push_back(block());
        finish(*fn);
        return fn;
    }

    NodePtr package_decl() {
        auto pkg = node(Kind::PackageDecl, cur());
        ++pos_; // 'package'
        pkg->text = std::string(cur().text);
        ++pos_;
        expect("{", "to start package body");
        while (!is_punct("}") && !at_end()) {
            const std::size_t before = pos_;
            if (is_word("function")) {
                try {
                    pkg->kids.push_back(function_decl());
                } catch (const ParseError&) {
                    if (pos_ == before) ++pos_;
                    recover();
                }
            } else if (accept(";")) {
            } else {
                error(cur(), "expected 'function' inside package");
                ++pos_;
                recover();
            }
            if (pos_ == before) ++pos_;
        }
        expect("}", "to close package");
        accept(";");
        finish(*pkg);
        return pkg;
    }

    NodePtr block() {
        auto b = node(Kind::Block, cur());
        expect("{");
        statements_until_brace(*b);
        expect("}");
        finish(*b);
        return b;
    }

    void statements_until_brace(Node& into) {
        while (!is_punct("}") && !at_end()) {
            const std::size_t before = pos_;
            if (auto s = statement_or_decl(false)) into.kids.push_back(std::move(s));
            if (pos_ == before) ++pos_;
        }
    }

    NodePtr paren_expr(std::string_view what) {
        expect("(", what);
        auto e = expression();
        expect(")", what);
        return e;
    }

    NodePtr statement() {
        const Token& t = cur();
        if (is_punct("{")) return block();
        if (is_punct(";")) {
            auto e = node(Kind::Empty, t);
            ++pos_;
            return e;
        }
        if (t.kind == TokKind::Ident && !tok_is(ahead(1), "=")) {
            const auto w = t.text;
            if (w == "if") return if_stmt();
            if (w == "while") {
                auto n = node(Kind::While, t);
                ++pos_;
                n->kids.push_back(paren_expr("around while condition"));
                n->kids.push_back(statement());
                finish(*n);
                return n;
            }
            if (w == "do") {
                auto n = node(Kind::DoWhile, t);
                ++pos_;
                n->kids.push_back(statement());
                if (!is_word("while")) fail(cur(), "expected 'while' after do body");
                ++pos_;
                n->kids.push_back(paren_expr("around do-while condition"));
                expect(";", "after do-while");
                finish(*n);
                return n;
            }
            if (w == "for") return for_stmt();
            if (w == "foreach" || w == "foreach$") return foreach_stmt();
            if (w == "switch" || w == "switch$") return switch_stmt();
            if (w == "break" || w == "continue") {
                auto n = node(w == "break" ? Kind::Break : Kind::Continue, t);
                ++pos_;
                expect(";", "after break/continue");
                finish(*n);
                return n;
            }
            if (w == "return") {
                auto n = node(Kind::Return, t);
                ++pos_;
                if (!is_punct(";")) n->kids.push_back(expression());
                expect(";", "after return");
                finish(*n);
                return n;
            }
            if (w == "function") fail(t, "nested function declaration");
        }
        auto s = node(Kind::ExprStmt, t);
        auto e = expression();
        const bool object_with_body = e->kind == Kind::ObjectDecl && e->kids[4]->kind == Kind::List;
        s->kids.push_back(std::move(e));
        if (object_with_body) {
            accept(";"); // lenient: `new X() { ... }` and `datablock X(Y) { ... }` without the trailing ';'
        } else {
            expect(";", "after expression");
        }
        finish(*s);
        return s;
    }

    NodePtr if_stmt() {
        auto n = node(Kind::If, cur());
        ++pos_;
        n->kids.push_back(paren_expr("around if condition"));
        n->kids.push_back(statement());
        if (is_word("else")) {
            ++pos_;
            n->kids.push_back(statement());
        } else {
            n->kids.push_back(none());
        }
        finish(*n);
        return n;
    }

    NodePtr for_stmt() {
        auto n = node(Kind::For, cur());
        ++pos_;
        expect("(", "after for");
        n->kids.push_back(is_punct(";") ? none() : expression());
        expect(";", "in for header");
        n->kids.push_back(is_punct(";") ? none() : expression());
        expect(";", "in for header");
        n->kids.push_back(is_punct(")") ? none() : expression());
        expect(")", "to close for header");
        n->kids.push_back(statement());
        finish(*n);
        return n;
    }

    NodePtr foreach_stmt() {
        auto n = node(Kind::Foreach, cur(), std::string(cur().text));
        ++pos_;
        expect("(", "after foreach");
        if (cur().kind != TokKind::LocalVar) fail(cur(), "expected loop variable '%name'");
        n->kids.push_back(node(Kind::LocalVar, cur(), std::string(cur().text)));
        ++pos_;
        if (!is_word("in")) fail(cur(), "expected 'in'");
        ++pos_;
        n->kids.push_back(expression());
        expect(")", "to close foreach header");
        n->kids.push_back(statement());
        finish(*n);
        return n;
    }

    NodePtr switch_stmt() {
        auto n = node(Kind::Switch, cur(), std::string(cur().text));
        ++pos_;
        n->kids.push_back(paren_expr("around switch subject"));
        expect("{", "to start switch body");
        while (!is_punct("}") && !at_end()) {
            const bool is_default = is_word("default") && tok_is(ahead(1), ":");
            if (!is_default && !is_word("case")) fail(cur(), "expected 'case' or 'default'");
            auto c = node(Kind::Case, cur(), is_default ? "default" : "case");
            ++pos_;
            auto values = node(Kind::List, cur(), "values");
            if (!is_default) {
                values->kids.push_back(case_value());
                while (is_word("or")) {
                    ++pos_;
                    values->kids.push_back(case_value());
                }
            }
            finish(*values);
            expect(":", "after case label");
            auto body = node(Kind::Block, cur());
            while (!is_punct("}") && !at_end() && !is_word("case") &&
                   !(is_word("default") && tok_is(ahead(1), ":"))) {
                const std::size_t before = pos_;
                if (auto s = statement_or_decl(false)) body->kids.push_back(std::move(s));
                if (pos_ == before) ++pos_;
            }
            finish(*body);
            c->kids.push_back(std::move(values));
            c->kids.push_back(std::move(body));
            finish(*c);
            n->kids.push_back(std::move(c));
        }
        expect("}", "to close switch");
        finish(*n);
        return n;
    }

    // A case label: any expression below the ternary (so the label's ':' is not taken as ?:).
    NodePtr case_value() { return binary(1); }

    // ---- expressions ----
    NodePtr expression() {
        auto lhs = ternary();
        if (cur().kind == TokKind::Punct && is_assign_op(cur().text)) {
            const auto k = lhs->kind;
            if (k != Kind::LocalVar && k != Kind::GlobalVar && k != Kind::Index && k != Kind::Field &&
                k != Kind::InternalName) {
                fail(cur(), "left side of assignment is not assignable");
            }
            auto a = node(Kind::Assign, cur(), std::string(cur().text));
            ++pos_;
            a->line = lhs->line;
            a->col = lhs->col;
            a->begin = lhs->begin;
            a->kids.push_back(std::move(lhs));
            a->kids.push_back(expression());
            finish(*a);
            return a;
        }
        return lhs;
    }

    NodePtr ternary() {
        auto c = binary(1);
        if (!is_punct("?")) return c;
        auto t = node(Kind::Ternary, cur());
        t->line = c->line;
        t->col = c->col;
        t->begin = c->begin;
        ++pos_;
        t->kids.push_back(std::move(c));
        t->kids.push_back(expression());
        expect(":", "in ?: expression");
        t->kids.push_back(ternary_rhs());
        finish(*t);
        return t;
    }
    // The else-arm of ?: may itself be an assignment-free ternary.
    NodePtr ternary_rhs() { return ternary(); }

    NodePtr binary(int min_prec) {
        auto lhs = unary();
        for (;;) {
            const int prec = binary_prec(cur());
            if (prec == 0 || prec < min_prec) return lhs;
            auto b = node(Kind::Binary, cur(), std::string(cur().text));
            ++pos_;
            b->line = lhs->line;
            b->col = lhs->col;
            b->begin = lhs->begin;
            b->kids.push_back(std::move(lhs));
            b->kids.push_back(binary(prec + 1));
            finish(*b);
            lhs = std::move(b);
        }
    }

    NodePtr unary() {
        const Token& t = cur();
        if (t.kind == TokKind::Punct && (t.text == "!" || t.text == "~" || t.text == "-")) {
            auto u = node(Kind::Unary, t, std::string(t.text));
            ++pos_;
            u->kids.push_back(unary());
            finish(*u);
            return u;
        }
        if (t.kind == TokKind::Punct && (t.text == "++" || t.text == "--")) {
            auto u = node(Kind::PreIncDec, t, std::string(t.text));
            ++pos_;
            u->kids.push_back(unary());
            finish(*u);
            return u;
        }
        return postfix(primary());
    }

    NodePtr postfix(NodePtr e) {
        for (;;) {
            const Token& t = cur();
            if (t.kind != TokKind::Punct) return e;
            if (t.text == ".") {
                ++pos_;
                if (cur().kind != TokKind::Ident) fail(cur(), "expected field or method name after '.'");
                const Token& name = cur();
                ++pos_;
                NodePtr n;
                if (is_punct("(")) {
                    n = node(Kind::MethodCall, name, std::string(name.text));
                    n->kids.push_back(std::move(e));
                    call_args(*n);
                } else {
                    n = node(Kind::Field, name, std::string(name.text));
                    n->kids.push_back(std::move(e));
                }
                rebase(*n);
                finish(*n);
                e = std::move(n);
            } else if (t.text == "[") {
                auto n = node(Kind::Index, t);
                ++pos_;
                n->kids.push_back(std::move(e));
                for (;;) {
                    n->kids.push_back(expression());
                    if (!accept(",")) break;
                }
                expect("]", "to close subscript");
                rebase(*n);
                finish(*n);
                e = std::move(n);
            } else if (t.text == "-->" || t.text == "->") {
                ++pos_;
                if (cur().kind != TokKind::Ident) fail(cur(), "expected internal name after '" + std::string(t.text) + "'");
                auto n = node(Kind::InternalName, cur(), std::string(t.text) + std::string(cur().text));
                ++pos_;
                n->kids.push_back(std::move(e));
                rebase(*n);
                finish(*n);
                e = std::move(n);
            } else if (t.text == "++" || t.text == "--") {
                auto n = node(Kind::PostIncDec, t, std::string(t.text));
                ++pos_;
                n->kids.push_back(std::move(e));
                rebase(*n);
                finish(*n);
                e = std::move(n);
            } else {
                return e;
            }
        }
    }
    // A postfix node starts where its first child starts.
    static void rebase(Node& n) {
        n.begin = n.kids.front()->begin;
    }

    void call_args(Node& call) {
        expect("(");
        while (!is_punct(")")) {
            call.kids.push_back(expression());
            if (!accept(",")) break;
        }
        expect(")", "to close argument list");
    }

    NodePtr primary() {
        const Token& t = cur();
        switch (t.kind) {
        case TokKind::Int: ++pos_; return node(Kind::Int, t, std::string(t.text));
        case TokKind::Float: ++pos_; return node(Kind::Float, t, std::string(t.text));
        case TokKind::String: ++pos_; return node(Kind::String, t, std::string(t.text));
        case TokKind::Tagged: ++pos_; return node(Kind::TaggedString, t, std::string(t.text));
        case TokKind::LocalVar: ++pos_; return node(Kind::LocalVar, t, std::string(t.text));
        case TokKind::GlobalVar: ++pos_; return node(Kind::GlobalVar, t, std::string(t.text));
        case TokKind::Ident: {
            const auto w = t.text;
            const Token& next = ahead(1);
            if ((w == "new" || w == "singleton" || w == "datablock") &&
                (next.kind == TokKind::Ident || tok_is(next, "("))) {
                return object_decl();
            }
            if (tok_is(next, "(")) {
                auto c = node(Kind::Call, t, std::string(w));
                ++pos_;
                call_args(*c);
                finish(*c);
                return c;
            }
            ++pos_;
            if (w == "true" || w == "false") return node(Kind::Bool, t, std::string(w));
            return node(Kind::Ident, t, std::string(w));
        }
        case TokKind::Punct:
            if (t.text == "(") {
                const Token& open = t;
                ++pos_;
                auto e = expression();
                expect(")", "to close parenthesis");
                e->begin = open.begin; // keep the parens in the source range
                finish(*e);
                return e;
            }
            break;
        default: break;
        }
        fail(t, "expected expression");
    }

    // new|singleton|datablock Class(Name : CopySource, args...) { fields; nested objects; }
    NodePtr object_decl() {
        auto n = node(Kind::ObjectDecl, cur(), std::string(cur().text));
        ++pos_;
        // class
        if (cur().kind == TokKind::Ident) {
            n->kids.push_back(node(Kind::Ident, cur(), std::string(cur().text)));
            ++pos_;
        } else {
            const Token& open = cur();
            ++pos_;
            auto e = expression();
            expect(")", "to close class expression");
            e->begin = open.begin;
            finish(*e);
            n->kids.push_back(std::move(e));
        }
        auto args = node(Kind::List, cur(), "args");
        NodePtr name = none();
        NodePtr parent = none();
        if (accept("(")) {
            if (!is_punct(")") && !is_punct(":") && !is_punct(",")) name = binary(1);
            if (accept(":")) parent = postfix(primary());
            while (accept(",")) args->kids.push_back(expression());
            expect(")", "to close object header");
        }
        finish(*args);
        n->kids.push_back(std::move(name));
        n->kids.push_back(std::move(parent));
        n->kids.push_back(std::move(args));
        if (is_punct("{")) {
            auto body = node(Kind::List, cur(), "body");
            ++pos_;
            object_body(*body);
            expect("}", "to close object body");
            finish(*body);
            n->kids.push_back(std::move(body));
        } else {
            n->kids.push_back(none());
        }
        finish(*n);
        return n;
    }

    void object_body(Node& body) {
        while (!is_punct("}") && !at_end()) {
            const std::size_t before = pos_;
            try {
                if (accept(";")) continue;
                const Token& t = cur();
                if (t.kind != TokKind::Ident) fail(t, "expected field name or object declaration");
                const Token& next = ahead(1);
                if ((t.text == "new" || t.text == "singleton" || t.text == "datablock") &&
                    (next.kind == TokKind::Ident || tok_is(next, "("))) {
                    body.kids.push_back(object_decl());
                    accept(";");
                    continue;
                }
                auto f = node(Kind::FieldAssign, t, std::string(t.text));
                ++pos_;
                auto index = node(Kind::List, cur(), "index");
                if (accept("[")) {
                    for (;;) {
                        index->kids.push_back(expression());
                        if (!accept(",")) break;
                    }
                    expect("]", "to close field subscript");
                    finish(*index);
                } else {
                    index->end = index->begin;
                }
                expect("=", "after field name");
                f->kids.push_back(std::move(index));
                f->kids.push_back(expression());
                if (!is_punct("}")) expect(";", "after field assignment");
                finish(*f);
                body.kids.push_back(std::move(f));
            } catch (const ParseError&) {
                if (pos_ == before) ++pos_;
                recover();
            }
        }
    }

    std::vector<Token> toks_;
    std::string_view file_;
    std::vector<Diagnostic>& diags_;
    std::size_t pos_ = 0;
};

} // namespace

ParseResult parse(std::string source, std::string file) {
    ParseResult r;
    r.file = std::move(file);
    r.source = std::move(source);
    auto toks = lex(r.source, r.file, r.diags);
    r.root = Parser(std::move(toks), r.file, r.diags).script();
    return r;
}

ParseResult parse_file(const std::filesystem::path& path, std::string display_name) {
    if (display_name.empty()) display_name = path.generic_string();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ParseResult r;
        r.file = display_name;
        r.root = std::make_unique<Node>();
        r.root->kind = Kind::Script;
        r.diags.push_back(Diagnostic{display_name, 0, 0, "cannot open file"});
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(std::move(ss).str(), std::move(display_name));
}

} // namespace fuse::remaster::tscript
