// Style sheet parser: selectors (compound, combinators, attribute / structural / functional pseudo-classes), rule
// blocks with nesting, shorthands, and the at-rules @media @container @supports @layer @scope @keyframes.
// Malformed input never fails harder than a diagnostic or a Syntax error (gate fuse_ui_style_fuzz).
#include "style_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::ui {

using namespace detail;

namespace {
using Comb = SimpleSelector::Combinator;
constexpr int kMaxSelectorDepth = 8;
constexpr int kMaxNesting = 16;

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }
std::size_t identEnd(std::string_view t, std::size_t from) {
    while (from < t.size() && identChar(t[from])) ++from;
    return from;
}
/// Index of the bracket that closes the one at `open` ('(' or '['), honouring quotes; npos when unbalanced.
std::size_t closingBracket(std::string_view t, std::size_t open) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = open; i < t.size(); ++i) {
        const char c = t[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && --depth == 0) return i;
    }
    return std::string_view::npos;
}

u32 packSpec(u32 ids, u32 cls, u32 types) { return (std::min(ids, 255u) << 16) | (std::min(cls, 255u) << 8) | std::min(types, 255u); }

bool parseInt(std::string_view s, i32& out) {
    s = trim(s);
    if (s.empty() || s.size() > 9) return false;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; ++i; }
    if (i >= s.size()) return false;
    i32 v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = neg ? -v : v;
    return true;
}
/// An+B: "odd", "even", "3", "2n", "-n + 3", "2n+1".
bool parseNth(std::string_view text, i32& a, i32& b) {
    std::string s;
    for (char c : text)
        if (!isSpace(c)) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (s == "odd") { a = 2; b = 1; return true; }
    if (s == "even") { a = 2; b = 0; return true; }
    const auto n = s.find('n');
    if (n == std::string::npos) { a = 0; return parseInt(s, b); }
    const std::string_view left = std::string_view(s).substr(0, n), right = std::string_view(s).substr(n + 1);
    if (left.empty() || left == "+") a = 1;
    else if (left == "-") a = -1;
    else if (!parseInt(left, a)) return false;
    b = 0;
    return right.empty() || ((right[0] == '+' || right[0] == '-') && parseInt(right, b));
}

struct SelectorParser {
    StyleRule& rule;

    /// A comma list of complex selectors (the argument of :not / :is / :where / :has, or an @scope prelude).
    bool list(std::string_view args, bool forgiving, bool relative, int depth, u32& firstList, u32& listCount, u32& maxSpec) {
        std::vector<SelectorRange> ranges;
        maxSpec = 0;
        bool ok = true;
        bool any = false;
        splitTop(args, ',', [&](std::string_view one) {
            any = true;
            std::vector<SimpleSelector> comps;
            u32 spec = 0;
            Comb lead = Comb::Descendant;
            bool root = false;
            if (!complex(one, comps, spec, relative ? &lead : nullptr, depth + 1, root) || comps.empty()) return forgiving ? true : (ok = false);
            if (rule.sub.size() + comps.size() > 60000) return ok = false;
            ranges.push_back({u32(rule.sub.size()), u32(comps.size()), lead});
            rule.sub.insert(rule.sub.end(), comps.begin(), comps.end());
            maxSpec = std::max(maxSpec, spec);
            return true;
        });
        if (!ok || (!forgiving && (!any || ranges.empty()))) return false;
        firstList = u32(rule.lists.size());
        listCount = u32(ranges.size());
        rule.lists.insert(rule.lists.end(), ranges.begin(), ranges.end());
        return true;
    }

    /// One complex selector into `out` (left to right). `lead`: a relative selector may start with a combinator.
    bool complex(std::string_view text, std::vector<SimpleSelector>& out, u32& spec, Comb* lead, int depth, bool& rootOnly) {
        if (depth > kMaxSelectorDepth) return false;
        text = trim(text);
        std::size_t i = 0;
        Comb comb = Comb::None;
        bool explicitComb = false;
        u32 ids = 0, cls = 0, types = 0;
        bool sawRoot = false;
        while (i < text.size()) {
            bool space = false;
            while (i < text.size() && isSpace(text[i])) { ++i; space = true; }
            if (i >= text.size()) break;
            const char c0 = text[i];
            if (c0 == '>' || c0 == '+' || c0 == '~') {
                if (explicitComb) return false;
                if (out.empty() && !lead) return false; // only a relative selector starts with a combinator
                comb = c0 == '>' ? Comb::Child : c0 == '+' ? Comb::NextSibling : Comb::LaterSibling;
                explicitComb = true;
                ++i;
                continue;
            }
            if (space && !out.empty() && !explicitComb) comb = Comb::Descendant;
            SimpleSelector sel;
            if (out.empty()) {
                if (lead) *lead = explicitComb ? comb : Comb::Descendant;
                sel.combinator = Comb::None;
            } else sel.combinator = comb == Comb::None ? Comb::Descendant : comb;
            std::vector<SelectorPart> parts;
            bool any = false;
            while (i < text.size() && !isSpace(text[i]) && text[i] != '>' && text[i] != '+' && text[i] != '~') {
                const char c = text[i];
                if (c == '*') { ++i; any = true; }
                else if (c == '#') {
                    const auto e = identEnd(text, i + 1);
                    if (e == i + 1) return false;
                    sel.idHash = hashId(text.substr(i + 1, e - i - 1));
                    i = e; ++ids; any = true;
                } else if (c == '.') {
                    const auto e = identEnd(text, i + 1);
                    if (e == i + 1 || sel.classCount >= 4) return false;
                    sel.classes[sel.classCount++] = hashId(text.substr(i + 1, e - i - 1));
                    i = e; ++cls; any = true;
                } else if (c == '[') {
                    const auto close = closingBracket(text, i);
                    if (close == std::string_view::npos) return false;
                    SelectorPart part;
                    if (!attribute(trim(text.substr(i + 1, close - i - 1)), part)) return false;
                    parts.push_back(part);
                    i = close + 1; ++cls; any = true;
                } else if (c == ':') {
                    if (i + 1 < text.size() && text[i + 1] == ':') return false; // pseudo-elements
                    const auto e = identEnd(text, i + 1);
                    const auto name = text.substr(i + 1, e - i - 1);
                    if (name.empty()) return false;
                    std::string_view args;
                    bool hasArgs = false;
                    i = e;
                    if (i < text.size() && text[i] == '(') {
                        const auto close = closingBracket(text, i);
                        if (close == std::string_view::npos) return false;
                        args = text.substr(i + 1, close - i - 1);
                        hasArgs = true;
                        i = close + 1;
                    }
                    any = true;
                    if (!pseudo(name, args, hasArgs, sel, parts, ids, cls, types, sawRoot, depth)) return false;
                } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                    if (any) return false; // a type selector comes first in its compound
                    const auto e = identEnd(text, i);
                    sel.typeHash = hashId(text.substr(i, e - i));
                    i = e; ++types; any = true;
                } else return false;
            }
            if (!any) return false;
            if (!parts.empty()) {
                if (rule.parts.size() + parts.size() > 60000) return false;
                sel.firstPart = u16(rule.parts.size());
                sel.partCount = u16(parts.size());
                rule.parts.insert(rule.parts.end(), parts.begin(), parts.end());
            }
            out.push_back(sel);
            comb = Comb::None;
            explicitComb = false;
        }
        if (explicitComb || out.empty()) return false; // a dangling combinator
        spec = packSpec(ids, cls, types);
        rootOnly = sawRoot && out.size() == 1;
        return true;
    }

    bool attribute(std::string_view t, SelectorPart& part) {
        const auto e = identEnd(t, 0);
        if (e == 0) return false;
        part.kind = SelectorPart::Kind::Attr;
        part.name = hashId(t.substr(0, e));
        t = trim(t.substr(e));
        if (t.empty()) { part.attrOp = AttrOp::Exists; return true; }
        if (t[0] == '=') { part.attrOp = AttrOp::Equals; t.remove_prefix(1); }
        else if (t.size() >= 2 && t[1] == '=') {
            switch (t[0]) {
            case '~': part.attrOp = AttrOp::Includes; break;
            case '|': part.attrOp = AttrOp::DashMatch; break;
            case '^': part.attrOp = AttrOp::Prefix; break;
            case '$': part.attrOp = AttrOp::Suffix; break;
            case '*': part.attrOp = AttrOp::Substring; break;
            default: return false;
            }
            t.remove_prefix(2);
        } else return false;
        t = trim(t);
        std::string_view value;
        if (!t.empty() && (t[0] == '"' || t[0] == '\'')) {
            const auto close = t.find(t[0], 1);
            if (close == std::string_view::npos) return false;
            value = t.substr(1, close - 1);
            t = trim(t.substr(close + 1));
        } else {
            const auto ve = identEnd(t, 0);
            if (ve == 0) return false;
            value = t.substr(0, ve);
            t = trim(t.substr(ve));
        }
        if (t == "i" || t == "I") part.noCase = true;
        else if (!(t.empty() || t == "s" || t == "S")) return false;
        part.text = u32(rule.strings.size());
        part.textLen = u32(value.size());
        rule.strings.append(value);
        return true;
    }

    bool pseudo(std::string_view name, std::string_view args, bool hasArgs, SimpleSelector& sel, std::vector<SelectorPart>& parts,
                u32& ids, u32& cls, u32& types, bool& sawRoot, int depth) {
        static constexpr struct { std::string_view name; u32 bit; } kStates[] = {
            {"hover", kStateHover}, {"pressed", kStatePressed}, {"active", kStatePressed}, {"focus", kStateFocus},
            {"focus-visible", kStateFocusVisible}, {"disabled", kStateDisabled}, {"checked", kStateChecked},
            {"selected", kStateSelected}, {"drag-over", kStateDragOver}};
        SelectorPart part;
        if (!hasArgs) {
            for (const auto& s : kStates)
                if (s.name == name) { sel.states |= s.bit; ++cls; return true; }
            if (name == "root") { part.kind = SelectorPart::Kind::Root; sawRoot = true; }
            else if (name == "scope") part.kind = SelectorPart::Kind::Scope;
            else if (name == "empty") part.kind = SelectorPart::Kind::Empty;
            else if (name == "first-child") { part.kind = SelectorPart::Kind::NthChild; part.b = 1; }
            else if (name == "last-child") { part.kind = SelectorPart::Kind::NthLastChild; part.b = 1; }
            else if (name == "only-child") {
                part.kind = SelectorPart::Kind::NthChild;
                part.b = 1;
                parts.push_back(part);
                part.kind = SelectorPart::Kind::NthLastChild;
            } else return false;
            parts.push_back(part);
            ++cls;
            return true;
        }
        if (name == "nth-child" || name == "nth-last-child") {
            part.kind = name == "nth-child" ? SelectorPart::Kind::NthChild : SelectorPart::Kind::NthLastChild;
            if (!parseNth(args, part.a, part.b)) return false;
            parts.push_back(part);
            ++cls;
            return true;
        }
        const bool isNot = name == "not", isIs = name == "is", isWhere = name == "where", isHas = name == "has";
        if (!(isNot || isIs || isWhere || isHas)) return false;
        part.kind = isNot ? SelectorPart::Kind::Not : isIs ? SelectorPart::Kind::Is : isWhere ? SelectorPart::Kind::Where : SelectorPart::Kind::Has;
        u32 maxSpec = 0;
        if (!list(args, isIs || isWhere, isHas, depth, part.firstList, part.listCount, maxSpec)) return false;
        parts.push_back(part);
        if (!isWhere) { // the most specific argument counts
            ids += (maxSpec >> 16) & 0xFFu;
            cls += (maxSpec >> 8) & 0xFFu;
            types += maxSpec & 0xFFu;
        }
        return true;
    }
};

// ---- media / container / supports conditions -----------------------------------------------------------------------
struct QueryParser {
    std::vector<QueryTerm>& out;
    bool container;
    bool needsHeight = false;

    void push(QueryTerm::Op op) { QueryTerm t; t.op = op; out.push_back(t); }
    bool value(QueryTerm::Feature f, std::string_view text, f32& v) {
        text = trim(text);
        std::string_view unit;
        switch (f) {
        case QueryTerm::Feature::Width:
        case QueryTerm::Feature::Height:
            if (!parseNumber(text, v, unit)) return false;
            if (unit == "em" || unit == "rem") v *= 16.f;
            else if (!(unit.empty() || unit == "px" || unit == "dp")) return false;
            return true;
        case QueryTerm::Feature::AspectRatio: {
            const auto slash = text.find('/');
            f32 a, b = 1.f;
            if (!parseNumber(text.substr(0, slash), a, unit) || !trim(unit).empty()) return false;
            if (slash != std::string_view::npos && (!parseNumber(text.substr(slash + 1), b, unit) || !unit.empty())) return false;
            if (!(b > 0.f)) return false;
            v = a / b;
            return true;
        }
        case QueryTerm::Feature::Orientation:
            if (text == "portrait") v = 0.f;
            else if (text == "landscape") v = 1.f;
            else return false;
            return true;
        case QueryTerm::Feature::PrefersDark:
            if (text == "dark") v = 1.f;
            else if (text == "light") v = 0.f;
            else return false;
            return true;
        case QueryTerm::Feature::PrefersReducedMotion:
            if (text == "reduce") v = 1.f;
            else if (text == "no-preference") v = 0.f;
            else return false;
            return true;
        case QueryTerm::Feature::Hover:
            if (text == "hover") v = 1.f;
            else if (text == "none") v = 0.f;
            else return false;
            return true;
        case QueryTerm::Feature::Pointer:
            if (text == "none") v = 0.f;
            else if (text == "coarse") v = 1.f;
            else if (text == "fine") v = 2.f;
            else return false;
            return true;
        case QueryTerm::Feature::Resolution:
            if (!parseNumber(text, v, unit)) return false;
            if (unit == "dpi") v /= 96.f;
            else if (!(unit == "dppx" || unit == "x")) return false;
            return true;
        }
        return false;
    }
    bool featureName(std::string_view name, QueryTerm::Feature& f) {
        using F = QueryTerm::Feature;
        if (name == "width" || name == "inline-size") f = F::Width;
        else if (name == "height" || name == "block-size") f = F::Height;
        else if (name == "aspect-ratio") f = F::AspectRatio;
        else if (name == "orientation") f = F::Orientation;
        else if (container) return false;
        else if (name == "prefers-color-scheme") f = F::PrefersDark;
        else if (name == "prefers-reduced-motion") f = F::PrefersReducedMotion;
        else if (name == "hover" || name == "any-hover") f = F::Hover;
        else if (name == "pointer" || name == "any-pointer") f = F::Pointer;
        else if (name == "resolution") f = F::Resolution;
        else return false;
        if (f == F::Height || f == F::AspectRatio || f == F::Orientation) needsHeight = true;
        return true;
    }
    void term(QueryTerm::Feature f, QueryTerm::Cmp cmp, f32 v) {
        QueryTerm t;
        t.op = QueryTerm::Op::Feature;
        t.feature = f;
        t.cmp = cmp;
        t.value = v;
        out.push_back(t);
    }
    /// The inside of one "( ... )": a feature test. Unknown features are false, as in CSS.
    void feature(std::string_view text) {
        using C = QueryTerm::Cmp;
        text = trim(text);
        QueryTerm::Feature f;
        f32 v = 0.f;
        // range syntax: "width < 300px", "400px <= width <= 500px"
        const auto firstCmp = text.find_first_of("<>=");
        if (firstCmp != std::string_view::npos && text.find(':') == std::string_view::npos) {
            struct Piece { std::string_view text; C cmp; };
            Piece pieces[3];
            int n = 0;
            std::size_t at = 0;
            while (n < 3) {
                const auto op = text.find_first_of("<>=", at);
                pieces[n].text = trim(text.substr(at, op == std::string_view::npos ? std::string_view::npos : op - at));
                if (op == std::string_view::npos) { ++n; break; }
                const bool eq = op + 1 < text.size() && text[op + 1] == '=';
                pieces[n].cmp = text[op] == '=' ? C::Eq : text[op] == '<' ? (eq ? C::Le : C::Lt) : (eq ? C::Ge : C::Gt);
                at = op + (eq && text[op] != '=' ? 2 : 1);
                ++n;
            }
            auto flip = [](C c) { return c == C::Lt ? C::Gt : c == C::Le ? C::Ge : c == C::Gt ? C::Lt : c == C::Ge ? C::Le : c; };
            if (n == 2 && featureName(pieces[0].text, f) && value(f, pieces[1].text, v)) return term(f, pieces[0].cmp, v);
            if (n == 2 && featureName(pieces[1].text, f) && value(f, pieces[0].text, v)) return term(f, flip(pieces[0].cmp), v);
            f32 v2 = 0.f;
            if (n == 3 && featureName(pieces[1].text, f) && value(f, pieces[0].text, v) && value(f, pieces[2].text, v2)) {
                term(f, flip(pieces[0].cmp), v);
                term(f, pieces[1].cmp, v2);
                return push(QueryTerm::Op::And);
            }
            return push(QueryTerm::Op::False);
        }
        const auto colon = text.find(':');
        if (colon == std::string_view::npos) { // boolean context
            if (featureName(text, f)) return term(f, C::Truthy, 0.f);
            return push(QueryTerm::Op::False);
        }
        std::string_view name = trim(text.substr(0, colon));
        C cmp = C::Eq;
        if (name.starts_with("min-")) { cmp = C::Ge; name.remove_prefix(4); }
        else if (name.starts_with("max-")) { cmp = C::Le; name.remove_prefix(4); }
        if (featureName(name, f) && value(f, text.substr(colon + 1), v)) return term(f, cmp, v);
        push(QueryTerm::Op::False);
    }
    /// "<in-parens> [and|or <in-parens>]*" or "not <in-parens>". False (and nothing usable in `out`) on a syntax error.
    bool condition(std::string_view text, int depth) {
        text = trim(text);
        if (depth > 8 || text.empty()) return false;
        if (text.starts_with("not ") || text.starts_with("not(")) {
            if (!condition(text.substr(3), depth + 1)) return false;
            push(QueryTerm::Op::Not);
            return true;
        }
        int terms = 0;
        QueryTerm::Op join = QueryTerm::Op::True; // undecided
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && isSpace(text[i])) ++i;
            if (i >= text.size()) break;
            if (terms > 0) { // a joining word
                const auto e = identEnd(text, i);
                const auto word = text.substr(i, e - i);
                const QueryTerm::Op op = word == "and" ? QueryTerm::Op::And : word == "or" ? QueryTerm::Op::Or : QueryTerm::Op::True;
                if (op == QueryTerm::Op::True || (join != QueryTerm::Op::True && join != op)) return false;
                join = op;
                i = e;
                while (i < text.size() && isSpace(text[i])) ++i;
            }
            if (i >= text.size() || text[i] != '(') return false;
            const auto close = closingBracket(text, i);
            if (close == std::string_view::npos) return false;
            const std::string_view inner = trim(text.substr(i + 1, close - i - 1));
            if (inner.starts_with("(") || inner.starts_with("not ") || inner.starts_with("not(")) {
                if (!condition(inner, depth + 1)) return false;
            } else feature(inner);
            if (terms > 0) push(join);
            ++terms;
            i = close + 1;
        }
        return terms > 0;
    }
    /// A media query list: "screen and (min-width: 600px), print". Queries that do not parse are false.
    void mediaList(std::string_view text) {
        int queries = 0;
        splitTop(text, ',', [&](std::string_view q) {
            const std::size_t mark = out.size();
            bool negate = false, ok = true;
            if (q.starts_with("not ") && !trim(q.substr(4)).starts_with("(")) { negate = true; q = trim(q.substr(4)); }
            else if (q.starts_with("only ")) q = trim(q.substr(5));
            if (!q.empty() && q[0] != '(' && !q.starts_with("not")) { // a media type
                const auto e = identEnd(q, 0);
                const auto type = q.substr(0, e);
                push(type == "all" || type == "screen" ? QueryTerm::Op::True : QueryTerm::Op::False);
                q = trim(q.substr(e));
                if (!q.empty()) {
                    if (!q.starts_with("and ") && !q.starts_with("and(")) ok = false;
                    else if ((ok = condition(q.substr(3), 0))) push(QueryTerm::Op::And);
                }
            } else ok = condition(q, 0);
            if (!ok) {
                out.resize(mark);
                push(QueryTerm::Op::False);
            } else if (negate) push(QueryTerm::Op::Not);
            if (queries++ > 0) push(QueryTerm::Op::Or);
            return true;
        });
        if (!queries) push(QueryTerm::Op::True); // "@media { }" applies
    }
};

void addDecl(StyleRule& rule, Prop p, std::string_view value, u32 line, std::vector<StyleDiagnostic>* diags) {
    Declaration d;
    d.prop = p;
    if (value.find("var(") != std::string_view::npos) {
        d.raw.assign(value);
        rule.decls.push_back(std::move(d));
        return;
    }
    if (!parsePropValue(p, value, d.value)) {
        if (diags) diags->push_back({StyleError::BadValue, line, std::string(propInfo(p).cssName) + ": " + std::string(value)});
        return;
    }
    rule.decls.push_back(std::move(d));
}

void splitWords(std::string_view v, std::string_view* out, int& n, int max) {
    n = 0;
    splitTop(v, ' ', [&](std::string_view w) {
        if (n < max) out[n++] = w;
        return true;
    });
}

bool isGradientFn(std::string_view v) { return v.starts_with("linear-gradient(") || v.starts_with("radial-gradient("); }

void addShorthand(StyleRule& rule, std::string_view name, std::string_view value, u32 line,
                  std::vector<StyleDiagnostic>* diags) {
    std::string_view w[8];
    int n = 0;
    auto four = [&](Prop t, Prop r, Prop b, Prop l) {
        splitWords(value, w, n, 4);
        if (n == 0) return;
        std::string_view top = w[0], right = n > 1 ? w[1] : w[0], bottom = n > 2 ? w[2] : w[0],
                         left = n > 3 ? w[3] : (n > 1 ? w[1] : w[0]);
        addDecl(rule, t, top, line, diags);
        addDecl(rule, r, right, line, diags);
        addDecl(rule, b, bottom, line, diags);
        addDecl(rule, l, left, line, diags);
    };
    auto bad = [&](std::string_view what) {
        if (diags) diags->push_back({StyleError::BadValue, line, std::string(what) + ": " + std::string(value)});
    };
    if (name == "padding") four(Prop::PaddingTop, Prop::PaddingRight, Prop::PaddingBottom, Prop::PaddingLeft);
    else if (name == "margin") four(Prop::MarginTop, Prop::MarginRight, Prop::MarginBottom, Prop::MarginLeft);
    else if (name == "border-radius") four(Prop::RadiusTL, Prop::RadiusTR, Prop::RadiusBR, Prop::RadiusBL);
    else if (name == "inset") four(Prop::Top, Prop::Right, Prop::Bottom, Prop::Left);
    else if (name == "border") {
        splitWords(value, w, n, 4);
        for (int k = 0; k < n; ++k) {
            PropValue tmp;
            if (parsePropValue(Prop::BorderWidth, w[k], tmp)) addDecl(rule, Prop::BorderWidth, w[k], line, diags);
            else if (w[k] == "solid" || w[k] == "none") continue;
            else addDecl(rule, Prop::BorderColor, w[k], line, diags);
        }
    } else if (name == "transition") { // <property|all|none> <duration> [easing] [delay], ...
        std::size_t at = 0;
        while (at <= value.size()) {
            const std::size_t comma = value.find(',', at);
            const std::string_view one = trim(value.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at));
            const std::size_t sp = one.find_first_of(" \t");
            const std::string_view target = one.substr(0, sp), rest = sp == std::string_view::npos ? std::string_view{} : one.substr(sp);
            // box shorthands animate their four longhands
            static constexpr std::string_view kBox[3][5] = {
                {"padding", "padding-top", "padding-right", "padding-bottom", "padding-left"},
                {"margin", "margin-top", "margin-right", "margin-bottom", "margin-left"},
                {"border-radius", "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}};
            bool expanded = false;
            for (const auto& box : kBox)
                if (target == box[0]) {
                    for (int k = 1; k < 5; ++k) addDecl(rule, Prop::Transition, std::string(box[k]) + std::string(rest), line, diags);
                    expanded = true;
                }
            if (!expanded && !one.empty()) addDecl(rule, Prop::Transition, one, line, diags);
            if (comma == std::string_view::npos) break;
            at = comma + 1;
        }
    } else if (name == "flex") {
        splitWords(value, w, n, 3);
        if (n > 0) addDecl(rule, Prop::Grow, w[0], line, diags);
        if (n > 1) addDecl(rule, Prop::Shrink, w[1], line, diags);
        if (n > 2) addDecl(rule, Prop::Basis, w[2], line, diags);
    } else if (name == "box-shadow") { // <x> <y> <blur> <color>
        splitWords(value, w, n, 4);
        if (n == 4) {
            addDecl(rule, Prop::ShadowX, w[0], line, diags);
            addDecl(rule, Prop::ShadowY, w[1], line, diags);
            addDecl(rule, Prop::ShadowBlur, w[2], line, diags);
            addDecl(rule, Prop::ShadowColor, w[3], line, diags);
        } else if (diags) diags->push_back({StyleError::BadValue, line, "box-shadow"});
    } else if (name == "background" && isGradientFn(value)) {
        // the two-colour form linear-gradient(vertical | horizontal | to bottom | to right, c0, c1) stays the plain
        // Rect gradient; anything else is a background-image
        std::string_view parts[4];
        int k = 0;
        std::string_view fn, args;
        if (splitFunction(value, fn, args))
            splitTop(args, ',', [&](std::string_view part) {
                if (k < 4) parts[k] = part;
                ++k;
                return true;
            });
        const bool legacyDir = k == 3 && fn == "linear-gradient" &&
                               (parts[0] == "vertical" || parts[0] == "horizontal" || parts[0] == "to bottom" || parts[0] == "to right");
        Color c0, c1;
        if (legacyDir && parseColor(parts[1], c0) && parseColor(parts[2], c1)) {
            addDecl(rule, Prop::Gradient, parts[0] == "to right" ? "horizontal" : (parts[0] == "to bottom" ? "vertical" : parts[0]), line, diags);
            addDecl(rule, Prop::Background, parts[1], line, diags);
            addDecl(rule, Prop::Background2, parts[2], line, diags);
        } else addDecl(rule, Prop::BackgroundImage, value, line, diags);
    } else if (name == "transform-origin") { // <x> [<y>]; a lone / leading vertical keyword is the y value
        splitWords(value, w, n, 3);
        if (n < 1 || n > 2) return bad(name);
        std::string_view x = w[0], y = n > 1 ? w[1] : std::string_view("center");
        if (x == "top" || x == "bottom" || y == "left" || y == "right") std::swap(x, y);
        addDecl(rule, Prop::TransformOriginX, x, line, diags);
        addDecl(rule, Prop::TransformOriginY, y, line, diags);
    } else if (name == "mask") { // <gradient> [alpha | luminance]
        splitWords(value, w, n, 3);
        if (n < 1 || n > 2) return bad(name);
        addDecl(rule, Prop::MaskImage, w[0], line, diags);
        if (n > 1) addDecl(rule, Prop::MaskMode, w[1], line, diags);
    } else if (name == "container") { // <name> [/ <type>]
        const auto slash = value.find('/');
        addDecl(rule, Prop::ContainerName, trim(value.substr(0, slash)), line, diags);
        if (slash != std::string_view::npos) addDecl(rule, Prop::ContainerType, trim(value.substr(slash + 1)), line, diags);
    } else if (name == "animation") {
        // <name> <duration> [easing] [delay] [count | infinite] [direction] [fill-mode] [play-state], ...
        std::string lists[8]; // name duration easing delay count direction fill play
        bool ok = true;
        int entries = 0;
        splitTop(value, ',', [&](std::string_view one) {
            std::string_view v[8] = {"none", "0s", "ease", "0s", "1", "normal", "none", "running"};
            bool haveName = false, haveDur = false, haveDelay = false, haveFill = false;
            splitTop(one, ' ', [&](std::string_view word) {
                f32 t;
                Easing e;
                std::string_view unit;
                if (word.find("var(") != std::string_view::npos) return ok = false;
                if (parseEasing(word, e)) v[2] = word;
                else if ((word.ends_with("s") || word.ends_with("ms")) && parseSeconds(word, t, true)) {
                    if (!haveDur) { v[1] = word; haveDur = true; }
                    else if (!haveDelay) { v[3] = word; haveDelay = true; }
                    else return ok = false;
                } else if (word == "infinite" || (parseNumber(word, t, unit) && unit.empty())) v[4] = word;
                else if (word == "normal" || word == "reverse" || word == "alternate" || word == "alternate-reverse") v[5] = word;
                else if (word == "forwards" || word == "backwards" || word == "both" || (word == "none" && haveName && !haveFill)) { v[6] = word; haveFill = true; }
                else if (word == "running" || word == "paused") v[7] = word;
                else if (!haveName) { v[0] = word; haveName = true; }
                else return ok = false;
                return true;
            });
            for (int k = 0; k < 8; ++k) {
                if (entries) lists[k] += ", ";
                lists[k].append(v[k]);
            }
            ++entries;
            return ok;
        });
        if (!ok || entries == 0 || entries > kMaxAnimations) return bad(name);
        static constexpr Prop kLonghands[8] = {Prop::AnimationName, Prop::AnimationDuration, Prop::AnimationTimingFunction, Prop::AnimationDelay,
                                               Prop::AnimationIterationCount, Prop::AnimationDirection, Prop::AnimationFillMode,
                                               Prop::AnimationPlayState};
        for (int k = 0; k < 8; ++k) addDecl(rule, kLonghands[k], lists[k], line, diags);
    } else if (diags) diags->push_back({StyleError::UnknownProperty, line, std::string(name)});
}

/// One "name: value" declaration into `rule` (custom properties also into `tokens`).
void addDeclaration(std::string_view decl, StyleRule& rule, std::vector<std::pair<u32, std::string>>* tokens, u32 line,
                    std::vector<StyleDiagnostic>* diags) {
    const auto colon = decl.find(':');
    if (colon == std::string_view::npos) {
        if (diags) diags->push_back({StyleError::Syntax, line, std::string(decl)});
        return;
    }
    const auto name = trim(decl.substr(0, colon));
    auto value = trim(decl.substr(colon + 1));
    if (name.starts_with("--")) {
        Declaration d;
        d.customHash = hashId(name);
        d.raw.assign(value);
        if (tokens) tokens->emplace_back(d.customHash, d.raw);
        rule.decls.push_back(std::move(d));
        return;
    }
    if (value.ends_with("!important")) { // no priority levels: applied as a normal declaration
        value = trim(value.substr(0, value.size() - 10));
        if (diags) diags->push_back({StyleError::BadValue, line, std::string(name) + ": !important is not supported (ignored)"});
    }
    Prop p;
    if (name == "background" && isGradientFn(value)) addShorthand(rule, name, value, line, diags);
    else if (findProp(name, p)) addDecl(rule, p, value, line, diags);
    else addShorthand(rule, name, value, line, diags);
}

/// @supports: "(prop: value)", "selector(...)", not / and / or. Unknown syntax is false.
bool supportsCondition(std::string_view text, int depth) {
    text = trim(text);
    if (depth > 8 || text.empty()) return false;
    if (text.starts_with("not ") || text.starts_with("not(")) return !supportsCondition(text.substr(3), depth + 1);
    bool result = false, first = true, isAnd = false, isOr = false;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isSpace(text[i])) ++i;
        if (i >= text.size()) break;
        bool andOp = false;
        if (!first) {
            const auto e = identEnd(text, i);
            const auto word = text.substr(i, e - i);
            if (word == "and") andOp = isAnd = true;
            else if (word == "or") isOr = true;
            else return false;
            if (isAnd && isOr) return false;
            i = e;
            while (i < text.size() && isSpace(text[i])) ++i;
        }
        bool value = false;
        if (text.substr(i).starts_with("selector(")) {
            const auto close = closingBracket(text, i + 8);
            if (close == std::string_view::npos) return false;
            StyleRule tmp;
            bool root = false;
            value = parseSelectorInto(text.substr(i + 9, close - i - 9), tmp, root);
            i = close + 1;
        } else if (i < text.size() && text[i] == '(') {
            const auto close = closingBracket(text, i);
            if (close == std::string_view::npos) return false;
            const std::string_view inner = trim(text.substr(i + 1, close - i - 1));
            if (inner.starts_with("(") || inner.starts_with("not ") || inner.starts_with("not(") || inner.starts_with("selector("))
                value = supportsCondition(inner, depth + 1);
            else if (inner.find(':') != std::string_view::npos && !inner.starts_with("--")) {
                StyleRule tmp;
                std::vector<StyleDiagnostic> d;
                addDeclaration(inner, tmp, nullptr, 0, &d);
                value = d.empty() && !tmp.decls.empty();
            } else value = inner.starts_with("--");
            i = close + 1;
        } else return false;
        result = first ? value : (andOp ? result && value : result || value);
        first = false;
    }
    return !first && result;
}

} // namespace

bool detail::parseSelectorInto(std::string_view text, StyleRule& rule, bool& isRoot) {
    rule.chain.clear();
    isRoot = false;
    SelectorParser sp{rule};
    std::vector<SimpleSelector> comps;
    u32 spec = 0;
    if (!sp.complex(text, comps, spec, nullptr, 0, isRoot)) return false;
    rule.chain = std::move(comps);
    rule.specificity = spec;
    return true;
}

// ---- Sheets --------------------------------------------------------------------------------------------------------
struct StyleParser {
    std::string_view css;
    std::vector<StyleDiagnostic>* diags;
    StyleSheet& sheet;
    u32 order = 0, anonymousLayers = 0;
    bool failed = false;
    StyleDiagnostic error{StyleError::Syntax, 0, {}};
    std::size_t linePos = 0;
    u32 lineNo = 1;

    struct Ctx {
        std::string parent; ///< the enclosing rule's selector list (nesting); empty at the top level
        bool parentIsList = false;
        i32 cond = -1;
        u16 layer = 0;
        std::string layerName;
        bool scoped = false;
        std::string scopeRoot, scopeLimit;
        int depth = 0;
    };

    u32 lineAt(std::size_t pos) {
        if (pos < linePos) { linePos = 0; lineNo = 1; }
        for (; linePos < pos && linePos < css.size(); ++linePos)
            if (css[linePos] == '\n') ++lineNo;
        return lineNo;
    }
    void fail(std::size_t pos, const char* what) {
        if (failed) return;
        failed = true;
        error = {StyleError::Syntax, lineAt(pos), what};
    }
    void diag(StyleError e, std::size_t pos, std::string msg) {
        if (diags) diags->push_back({e, lineAt(pos), std::move(msg)});
    }
    void skipWs(std::size_t& i, std::size_t hi) const {
        for (;;) {
            while (i < hi && isSpace(css[i])) ++i;
            if (i + 1 < hi && css[i] == '/' && css[i + 1] == '*') {
                const auto e = css.find("*/", i + 2);
                i = e == std::string_view::npos || e + 2 > hi ? hi : e + 2;
                continue;
            }
            break;
        }
    }
    /// First '{' or ';' at bracket depth 0 in [i, hi), outside strings and comments; hi when there is none.
    std::size_t scan(std::size_t i, std::size_t hi) const {
        int depth = 0;
        char quote = 0;
        for (; i < hi; ++i) {
            const char c = css[i];
            if (quote) {
                if (c == '\\') ++i;
                else if (c == quote || c == '\n') quote = 0;
                continue;
            }
            if (c == '/' && i + 1 < hi && css[i + 1] == '*') {
                const auto e = css.find("*/", i + 2);
                if (e == std::string_view::npos || e + 2 > hi) return hi;
                i = e + 1;
                continue;
            }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') { if (depth > 0) --depth; }
            else if ((c == '{' || c == ';') && depth == 0) return i;
        }
        return hi;
    }
    /// The '}' that closes the '{' at `open`; npos when the block is not closed before `hi`.
    std::size_t closingBrace(std::size_t open, std::size_t hi) const {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = open; i < hi; ++i) {
            const char c = css[i];
            if (quote) {
                if (c == '\\') ++i;
                else if (c == quote || c == '\n') quote = 0;
                continue;
            }
            if (c == '/' && i + 1 < hi && css[i + 1] == '*') {
                const auto e = css.find("*/", i + 2);
                if (e == std::string_view::npos || e + 2 > hi) return std::string_view::npos;
                i = e + 1;
                continue;
            }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) return i;
        }
        return std::string_view::npos;
    }
    /// Text of [lo, hi) without comments.
    std::string clean(std::size_t lo, std::size_t hi) const {
        std::string out;
        for (std::size_t i = lo; i < hi; ++i) {
            if (css[i] == '/' && i + 1 < hi && css[i + 1] == '*') {
                const auto e = css.find("*/", i + 2);
                if (e == std::string_view::npos || e + 2 > hi) break;
                i = e + 1;
                out.push_back(' ');
                continue;
            }
            out.push_back(css[i]);
        }
        return out;
    }

    u16 layerIndex(const std::string& name) { // registers `name` and the layers it is nested in
        for (std::size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) layerIndex(name.substr(0, dot));
        auto& layers = sheet.m_layers;
        for (std::size_t i = 0; i < layers.size(); ++i)
            if (layers[i] == name) return u16(i + 1);
        if (layers.size() >= 4096) return 0;
        layers.push_back(name);
        return u16(layers.size());
    }
    static bool layerNameOk(std::string_view n) {
        if (n.empty() || n.front() == '.' || n.back() == '.') return false;
        for (char c : n)
            if (!identChar(c) && c != '.') return false;
        return n.find("..") == std::string_view::npos;
    }

    /// The items of a block. Top level (`decls` null): rules and at-rules. Inside a rule: `declPass` collects its
    /// declarations, the other pass handles nested rules and at-rules.
    void walk(std::size_t lo, std::size_t hi, const Ctx& ctx, StyleRule* decls, std::vector<std::pair<u32, std::string>>* tokens,
              bool declPass) {
        std::size_t i = lo;
        while (!failed) {
            skipWs(i, hi);
            if (i >= hi) break;
            if (css[i] == ';') { ++i; continue; }
            const std::size_t start = i, stop = scan(i, hi);
            if (stop < hi && css[stop] == '{') {
                const std::size_t close = closingBrace(stop, hi);
                if (close == std::string_view::npos) return fail(start, "expected '}'");
                if (!declPass) block(start, stop, close, ctx);
                i = close + 1;
                continue;
            }
            const std::string text = clean(start, stop);
            const std::string_view t = trim(text);
            if (t.starts_with("@")) {
                if (!declPass) statement(t, start, ctx);
            } else if (decls) {
                if (declPass && !t.empty()) addDeclaration(t, *decls, tokens, lineAt(start), diags);
            } else {
                return fail(start, t.find('}') != std::string_view::npos ? "unexpected '}'" : "expected '{'");
            }
            i = stop < hi ? stop + 1 : hi;
        }
    }

    /// "@layer a, b;" (other statement at-rules are skipped).
    void statement(std::string_view t, std::size_t pos, const Ctx& ctx) {
        if (t.starts_with("@layer") && (t.size() == 6 || isSpace(t[6]))) {
            splitTop(t.substr(6), ',', [&](std::string_view n) {
                if (layerNameOk(n)) layerIndex(ctx.layerName.empty() ? std::string(n) : ctx.layerName + "." + std::string(n));
                else diag(StyleError::Syntax, pos, "bad layer name: " + std::string(n));
                return true;
            });
            return;
        }
        diag(StyleError::Syntax, pos, "unsupported at-rule: " + std::string(t.substr(0, t.find_first_of(" \t\n("))));
    }

    void block(std::size_t start, std::size_t open, std::size_t close, const Ctx& ctx) {
        const std::string preludeText = clean(start, open);
        const std::string_view prelude = trim(preludeText);
        const std::size_t lo = open + 1, hi = close;
        if (ctx.depth >= kMaxNesting) return diag(StyleError::Syntax, start, "blocks nested too deep");
        if (!prelude.starts_with("@")) return styleRule(prelude, lo, hi, ctx, start);
        const auto ne = identEnd(prelude, 1);
        const std::string_view name = prelude.substr(1, ne - 1), rest = trim(prelude.substr(ne));
        Ctx in = ctx;
        ++in.depth;
        auto inner = [&] { // a conditional group: its body holds rules, or declarations when it sits inside a rule
            if (in.parent.empty()) walk(lo, hi, in, nullptr, nullptr, false);
            else styleRule("&", lo, hi, in, start);
        };
        if (name == "media" || name == "container") {
            StyleCondition c;
            c.kind = name == "media" ? StyleCondition::Kind::Media : StyleCondition::Kind::Container;
            c.parent = ctx.cond;
            QueryParser qp{c.program, name == "container"};
            if (name == "media") qp.mediaList(rest);
            else {
                std::string_view q = rest;
                if (!q.empty() && q[0] != '(' && !q.starts_with("not")) { // a container name
                    const auto e = identEnd(q, 0);
                    c.containerName = animationNameId(q.substr(0, e));
                    q = trim(q.substr(e));
                }
                if (!qp.condition(q, 0)) {
                    c.program.clear();
                    qp.push(QueryTerm::Op::False);
                    diag(StyleError::Syntax, start, "bad container query: " + std::string(rest));
                }
                c.needsHeight = qp.needsHeight;
            }
            if (sheet.m_conditions.size() >= 60000) return;
            sheet.m_conditions.push_back(std::move(c));
            in.cond = i32(sheet.m_conditions.size() - 1);
            return inner();
        }
        if (name == "supports") {
            if (supportsCondition(rest, 0)) inner();
            return;
        }
        if (name == "layer") {
            std::string full;
            if (rest.empty()) full = "\x01" + std::to_string(anonymousLayers++); // anonymous: a name nothing else can use
            else if (layerNameOk(rest)) full = std::string(rest);
            else return diag(StyleError::Syntax, start, "bad layer name: " + std::string(rest));
            if (!ctx.layerName.empty()) full = ctx.layerName + "." + full;
            in.layer = layerIndex(full);
            in.layerName = full;
            return inner();
        }
        if (name == "scope") { // (root) [to (limit)]
            std::string_view r = rest;
            if (r.empty() || r[0] != '(') return diag(StyleError::Syntax, start, "@scope needs (root)");
            const auto rc = closingBracket(r, 0);
            if (rc == std::string_view::npos) return diag(StyleError::Syntax, start, "bad @scope");
            in.scopeRoot = std::string(trim(r.substr(1, rc - 1)));
            in.scopeLimit.clear();
            r = trim(r.substr(rc + 1));
            if (!r.empty()) {
                if (!r.starts_with("to") || trim(r.substr(2)).empty() || trim(r.substr(2))[0] != '(') return diag(StyleError::Syntax, start, "bad @scope");
                r = trim(r.substr(2));
                const auto lc = closingBracket(r, 0);
                if (lc == std::string_view::npos || !trim(r.substr(lc + 1)).empty()) return diag(StyleError::Syntax, start, "bad @scope");
                in.scopeLimit = std::string(trim(r.substr(1, lc - 1)));
            }
            in.scoped = true;
            return inner();
        }
        if (name == "keyframes") {
            KeyframesRule kf;
            const auto e = identEnd(rest, 0);
            if (e == 0 || e != rest.size()) return diag(StyleError::Syntax, start, "bad @keyframes name");
            kf.name = animationNameId(rest);
            keyframes(lo, hi, kf);
            if (!failed) sheet.m_keyframes.push_back(std::move(kf));
            return;
        }
        diag(StyleError::Syntax, start, "unsupported at-rule: @" + std::string(name));
    }

    void keyframes(std::size_t lo, std::size_t hi, KeyframesRule& kf) {
        std::size_t i = lo;
        while (!failed) {
            skipWs(i, hi);
            if (i >= hi) break;
            const std::size_t start = i, stop = scan(i, hi);
            if (stop >= hi || css[stop] != '{') { // not a keyframe block
                diag(StyleError::Syntax, start, "expected a keyframe block");
                i = stop < hi ? stop + 1 : hi;
                continue;
            }
            const std::size_t close = closingBrace(stop, hi);
            if (close == std::string_view::npos) return fail(start, "expected '}'");
            StyleRule body;
            walk(stop + 1, close, Ctx{}, &body, nullptr, true);
            KeyframeBlock proto;
            for (Declaration& d : body.decls) {
                if (d.prop == Prop::Count) continue;
                if (d.prop == Prop::AnimationTimingFunction) {
                    if (d.raw.empty() && d.value.fCount) { proto.hasEase = true; proto.ease = Easing(std::min<u8>(u8(d.value.f[0]), u8(Easing::Out))); }
                    continue;
                }
                proto.decls.push_back(std::move(d));
            }
            const std::string sel = clean(start, stop);
            splitTop(sel, ',', [&](std::string_view s) {
                f32 at;
                std::string_view unit;
                if (s == "from") at = 0.f;
                else if (s == "to") at = 1.f;
                else if (parseNumber(s, at, unit) && unit == "%" && at >= 0.f && at <= 100.f) at /= 100.f;
                else { diag(StyleError::Syntax, start, "bad keyframe selector: " + std::string(s)); return true; }
                if (kf.frames.size() >= 256) return true;
                KeyframeBlock b = proto;
                b.offset = at;
                kf.frames.push_back(std::move(b));
                return true;
            });
            i = close + 1;
        }
        std::stable_sort(kf.frames.begin(), kf.frames.end(), [](const KeyframeBlock& a, const KeyframeBlock& b) { return a.offset < b.offset; });
    }

    void styleRule(std::string_view selectors, std::size_t lo, std::size_t hi, const Ctx& ctx, std::size_t pos) {
        StyleRule proto;
        std::vector<std::pair<u32, std::string>> tokens;
        walk(lo, hi, ctx, &proto, &tokens, true);
        if (failed) return;
        const bool implicit = selectors == "&"; // the body of a conditional group inside a rule
        const std::string parentRef = ctx.parentIsList ? ":is(" + ctx.parent + ")" : ctx.parent;
        std::string resolved; // this rule's selector list as nested rules see it
        int count = 0;
        splitTop(selectors, ',', [&](std::string_view one) {
            if (one.empty()) { diag(StyleError::Syntax, pos, "bad selector: empty"); return true; }
            std::string text;
            if (ctx.parent.empty()) text = std::string(one);
            else if (one.find('&') != std::string_view::npos) {
                for (char c : one) {
                    if (c == '&') text += parentRef;
                    else text.push_back(c);
                }
            } else text = parentRef + " " + std::string(one);
            if (text.size() > 4096) { diag(StyleError::Syntax, pos, "selector too long"); return true; }
            StyleRule rule;
            bool isRoot = false;
            bool ok = parseSelectorInto(text, rule, isRoot);
            if (ok && ctx.scoped) {
                SelectorParser sp{rule};
                u32 spec = 0;
                ok = sp.list(ctx.scopeRoot, false, false, 0, rule.scopeFirst, rule.scopeCount, spec);
                if (ok && !ctx.scopeLimit.empty()) ok = sp.list(ctx.scopeLimit, false, false, 0, rule.limitFirst, rule.limitCount, spec);
                rule.scoped = ok;
                isRoot = false;
            }
            if (!ok) {
                diag(StyleError::Syntax, pos, "bad selector: " + text);
                return true;
            }
            if (count++) resolved += ", ";
            resolved += text;
            if (implicit && proto.decls.empty()) return true; // nothing of its own to add
            rule.decls = proto.decls;
            rule.order = order++;
            rule.root = isRoot;
            rule.cond = ctx.cond;
            rule.layer = ctx.layer;
            if (isRoot)
                for (auto& t : tokens) sheet.m_tokens.push_back(t);
            if (sheet.m_rules.size() < 1000000) sheet.m_rules.push_back(std::move(rule));
            return true;
        });
        if (resolved.empty() || resolved.size() > 4096) return; // nothing nested rules could attach to
        Ctx in = ctx;
        in.parent = implicit ? ctx.parent : resolved;
        in.parentIsList = implicit ? ctx.parentIsList : count > 1;
        ++in.depth;
        if (in.depth >= kMaxNesting) return;
        StyleRule unused;
        walk(lo, hi, in, &unused, nullptr, false);
    }
};

std::expected<StyleSheet, StyleDiagnostic> StyleSheet::parse(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    StyleSheet sheet;
    StyleParser p{css, diags, sheet};
    p.walk(0, css.size(), StyleParser::Ctx{}, nullptr, nullptr, false);
    if (p.failed) return std::unexpected(p.error);
    return sheet;
}

bool parseKeyframes(std::string_view body, KeyframesRule& out, std::vector<StyleDiagnostic>* diagnostics) {
    StyleSheet scratch;
    StyleParser p{body, diagnostics, scratch};
    out.frames.clear();
    p.keyframes(0, body.size(), out);
    return !p.failed && !out.frames.empty();
}

} // namespace fuse::ui
