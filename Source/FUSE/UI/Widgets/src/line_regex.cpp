// Partial-match regex for regexValidator (QRegularExpressionValidator semantics): a Thompson NFA over code points.
// After the whole input is consumed, a live state set means "a prefix of some match" (Intermediate), the match state
// in it means Acceptable, and an empty set Invalid. Every NFA state lies on a path to the match state (no dead
// classes are built), so "live" really is "extendable".
#include "fuse/ui/widgets/widgets.hpp"

#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace fuse::ui::widgets {
namespace {

struct Range { char32_t lo, hi; };
struct CharSet {
    std::vector<Range> r;
    bool neg = false;
    bool has(char32_t c) const {
        bool in = false;
        for (const Range& x : r) in |= c >= x.lo && c <= x.hi;
        return in != neg;
    }
};
void addDigit(CharSet& s) { s.r.push_back({'0', '9'}); }
void addWord(CharSet& s) { s.r.insert(s.r.end(), {{'a', 'z'}, {'A', 'Z'}, {'0', '9'}, {'_', '_'}}); }
void addSpace(CharSet& s) { s.r.insert(s.r.end(), {{' ', ' '}, {'\t', '\r'}, {0xA0, 0xA0}, {0x2028, 0x2029}, {0xFEFF, 0xFEFF}}); }

struct Ast {
    enum K { Set, Cat, Alt, Rep, Empty } k = Empty;
    CharSet set;
    std::vector<Ast> kids;
    int lo = 0, hi = 0; ///< Rep: hi < 0 = unbounded
};

std::u32string decode(std::string_view s) {
    std::u32string o;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        char32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        ++i;
        for (; n > 0 && i < s.size(); --n, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        o += cp;
    }
    return o;
}

struct Parser {
    std::u32string p;
    std::size_t i = 0;
    bool ok = true;
    bool eof() const { return i >= p.size(); }
    char32_t peek() const { return eof() ? 0 : p[i]; }
    Ast alt() {
        Ast a;
        a.k = Ast::Alt;
        a.kids.push_back(cat());
        while (ok && peek() == '|') { ++i; a.kids.push_back(cat()); }
        return a.kids.size() == 1 ? std::move(a.kids[0]) : a;
    }
    Ast cat() {
        Ast a;
        a.k = Ast::Cat;
        while (ok && !eof() && peek() != '|' && peek() != ')') {
            if (peek() == '^') { if (i != 0) ok = false; ++i; continue; }
            if (peek() == '$') { if (i + 1 != p.size()) ok = false; ++i; continue; }
            Ast at = atom();
            while (ok && !eof()) {
                int lo, hi;
                const char32_t q = peek();
                if (q == '*') lo = 0, hi = -1, ++i;
                else if (q == '+') lo = 1, hi = -1, ++i;
                else if (q == '?') lo = 0, hi = 1, ++i;
                else if (q == '{' && braces(lo, hi)) {}
                else break;
                if (peek() == '?') ++i; // lazy: same language
                Ast r;
                r.k = Ast::Rep;
                r.lo = lo, r.hi = hi;
                r.kids.push_back(std::move(at));
                at = std::move(r);
            }
            a.kids.push_back(std::move(at));
        }
        return a;
    }
    bool braces(int& lo, int& hi) {
        std::size_t j = i + 1;
        auto num = [&](int& v) { const std::size_t s = j; v = 0; while (j < p.size() && p[j] >= '0' && p[j] <= '9') v = std::min(v * 10 + int(p[j++] - '0'), 100000); return j > s; };
        if (!num(lo)) return false; // a literal '{'
        hi = lo;
        if (j < p.size() && p[j] == ',') { ++j; if (!num(hi)) hi = -1; }
        if (j >= p.size() || p[j] != '}') return false;
        if (hi >= 0 && hi < lo) { ok = false; return false; }
        i = j + 1;
        return true;
    }
    bool escapeClass(char32_t e, CharSet& s, bool inClass) {
        switch (e) {
        case 'd': addDigit(s); return true;
        case 'w': addWord(s); return true;
        case 's': addSpace(s); return true;
        case 'D': case 'W': case 'S':
            if (inClass) { ok = false; return true; }
            if (e == 'D') addDigit(s); else if (e == 'W') addWord(s); else addSpace(s);
            s.neg = true;
            return true;
        default: return false;
        }
    }
    char32_t escapeChar(char32_t e) {
        switch (e) {
        case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r'; case 'f': return '\f'; case 'v': return '\v';
        case '0': return 0;
        default:
            if ((e >= '1' && e <= '9') || e == 'b' || e == 'B' || e == 'k' || e == 'p' || e == 'P' || e == 'c' || e == 'x' || e == 'u') ok = false; // backrefs, \b, classes: unsupported
            return e;
        }
    }
    Ast atom() {
        Ast a;
        a.k = Ast::Set;
        const char32_t c = p[i++];
        if (c == '(') {
            if (peek() == '?') {
                if (i + 1 < p.size() && p[i + 1] == ':') i += 2;
                else { ok = false; return a; } // lookaround, named groups
            }
            Ast in = alt();
            if (peek() != ')') { ok = false; return a; }
            ++i;
            return in;
        }
        if (c == '[') { klass(a.set); return a; }
        if (c == '.') { a.set.r = {{'\n', '\n'}, {'\r', '\r'}, {0x2028, 0x2029}}; a.set.neg = true; return a; }
        if (c == '\\') {
            if (eof()) { ok = false; return a; }
            const char32_t e = p[i++];
            if (!escapeClass(e, a.set, false)) { const char32_t l = escapeChar(e); a.set.r = {{l, l}}; }
            return a;
        }
        if (c == '*' || c == '+' || c == '?' || c == ')') { ok = false; return a; }
        a.set.r = {{c, c}};
        return a;
    }
    void klass(CharSet& s) {
        if (peek() == '^') { s.neg = true; ++i; }
        bool first = true;
        while (ok) {
            if (eof()) { ok = false; return; }
            char32_t c = p[i++];
            if (c == ']' && !first) return;
            first = false;
            if (c == '\\') {
                if (eof()) { ok = false; return; }
                const char32_t e = p[i++];
                if (escapeClass(e, s, true)) continue;
                c = e == 'b' ? char32_t('\b') : escapeChar(e);
            }
            char32_t hi = c;
            if (peek() == '-' && i + 1 < p.size() && p[i + 1] != ']') {
                ++i;
                hi = p[i++];
                if (hi == '\\') { if (eof()) { ok = false; return; } hi = escapeChar(p[i++]); }
                if (hi < c) { ok = false; return; }
            }
            s.r.push_back({c, hi});
        }
    }
};

// ---- NFA ----
struct State {
    enum K { Char, Split, Match } k;
    CharSet set;
    int out = -1, out2 = -1;
};
struct Nfa {
    std::vector<State> st;
    bool tooBig = false;
    int add(State s) {
        if (st.size() > 20000) tooBig = true;
        st.push_back(std::move(s));
        return int(st.size()) - 1;
    }
};
// patch lists are state indices + which out (0/1), since the state vector reallocates
struct Hole { int s, which; };
struct F { int start; std::vector<Hole> outs; };
void patch(Nfa& n, const std::vector<Hole>& h, int to) {
    for (const Hole& x : h) (x.which ? n.st[std::size_t(x.s)].out2 : n.st[std::size_t(x.s)].out) = to;
}
F emptyFrag(Nfa& n) { // a Split whose both outs go to the same place: an epsilon
    const int s = n.add({State::Split, {}, -1, -1});
    return {s, {{s, 0}, {s, 1}}};
}
F build(Nfa& n, const Ast& a) {
    if (n.tooBig) return emptyFrag(n);
    switch (a.k) {
    case Ast::Set: {
        const int s = n.add({State::Char, a.set, -1, -1});
        return {s, {{s, 0}}};
    }
    case Ast::Empty: return emptyFrag(n);
    case Ast::Cat: {
        if (a.kids.empty()) return emptyFrag(n);
        F f = build(n, a.kids[0]);
        for (std::size_t k = 1; k < a.kids.size(); ++k) {
            F g = build(n, a.kids[k]);
            patch(n, f.outs, g.start);
            f.outs = std::move(g.outs);
        }
        return f;
    }
    case Ast::Alt: {
        F f = build(n, a.kids[0]);
        for (std::size_t k = 1; k < a.kids.size(); ++k) {
            F g = build(n, a.kids[k]);
            const int s = n.add({State::Split, {}, f.start, g.start});
            f.start = s;
            f.outs.insert(f.outs.end(), g.outs.begin(), g.outs.end());
        }
        return f;
    }
    case Ast::Rep: {
        const Ast& c = a.kids[0];
        F f{-1, {}};
        auto append = [&](F g) {
            if (f.start < 0) f = std::move(g);
            else { patch(n, f.outs, g.start); f.outs = std::move(g.outs); }
        };
        for (int k = 0; k < a.lo; ++k) append(build(n, c));
        if (a.hi < 0) { // star
            F g = build(n, c);
            const int s = n.add({State::Split, {}, g.start, -1});
            patch(n, g.outs, s);
            append({s, {{s, 1}}});
        } else {
            // optional copies nested: (c(c(c)?)?)?
            std::vector<Hole> skips;
            for (int k = a.lo; k < a.hi; ++k) {
                F g = build(n, c);
                const int s = n.add({State::Split, {}, g.start, -1});
                skips.push_back({s, 1});
                if (f.start < 0) f = {s, {}};
                else patch(n, f.outs, s);
                f.outs = g.outs; // the next optional copy hangs off this one's end
            }
            f.outs.insert(f.outs.end(), skips.begin(), skips.end());
            if (f.start < 0) return emptyFrag(n);
        }
        return f;
    }
    }
    return emptyFrag(n);
}

struct Compiled {
    Nfa nfa;
    int start = -1, match = -1;
    bool ok = false;
    std::shared_ptr<const std::regex> fallback;
};
std::shared_ptr<Compiled> compile(std::string_view pattern) {
    auto c = std::make_shared<Compiled>();
    Parser ps{decode(pattern)};
    Ast a;
    if (!ps.p.empty()) a = ps.alt();
    if (ps.ok && !ps.eof()) ps.ok = false; // stray ')'
    if (ps.ok) {
        F f = build(c->nfa, a);
        c->match = c->nfa.add({State::Match, {}, -1, -1});
        patch(c->nfa, f.outs, c->match);
        c->start = f.start;
        c->ok = !c->nfa.tooBig;
    }
    if (!c->ok) {
        try {
            c->fallback = std::make_shared<const std::regex>(std::string(pattern), std::regex::ECMAScript);
        } catch (const std::regex_error&) {
            c->fallback = nullptr;
        }
    }
    return c;
}
void closure(const Nfa& n, int s, std::vector<int>& set, std::vector<unsigned>& mark, unsigned gen) {
    if (s < 0 || mark[std::size_t(s)] == gen) return;
    mark[std::size_t(s)] = gen;
    const State& x = n.st[std::size_t(s)];
    if (x.k == State::Split) { closure(n, x.out, set, mark, gen); closure(n, x.out2, set, mark, gen); return; }
    set.push_back(s);
}
Validation run(const Compiled& c, std::string_view text) {
    if (!c.ok) {
        if (!c.fallback) return Validation::Invalid;
        return std::regex_match(text.begin(), text.end(), *c.fallback) ? Validation::Acceptable : Validation::Intermediate;
    }
    const Nfa& n = c.nfa;
    std::vector<unsigned> mark(n.st.size(), 0);
    unsigned gen = 1;
    std::vector<int> cur, next;
    closure(n, c.start, cur, mark, gen);
    for (char32_t ch : decode(text)) {
        next.clear();
        ++gen;
        for (int s : cur) {
            const State& x = n.st[std::size_t(s)];
            if (x.k == State::Char && x.set.has(ch)) closure(n, x.out, next, mark, gen);
        }
        cur.swap(next);
        if (cur.empty()) return Validation::Invalid;
    }
    for (int s : cur)
        if (s == c.match) return Validation::Acceptable;
    return cur.empty() ? Validation::Invalid : Validation::Intermediate;
}

} // namespace

Validation regexPartialMatch(std::string_view pattern, std::string_view text, bool* supported) {
    const auto c = compile(pattern);
    if (supported) *supported = c->ok;
    return run(*c, text);
}
Validator regexValidator(std::string_view pattern) {
    std::shared_ptr<const Compiled> c = compile(pattern);
    return [c](std::string_view t) { return run(*c, t); };
}

} // namespace fuse::ui::widgets
