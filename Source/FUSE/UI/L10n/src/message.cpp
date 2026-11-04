// ICU MessageFormat (1) subset: a recursive-descent formatter that formats the selected sub-messages and only
// validates the others (out == nullptr).
#include "fuse/ui/l10n/l10n.hpp"

#include <charconv>
#include <cmath>

namespace fuse::ui::l10n {

const Value* Args::find(std::string_view name) const {
    for (const Arg& a : m_args)
        if (a.name == name) return &a.value;
    return nullptr;
}

namespace {
bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string shortest(double x) {
    char buf[400];
    const auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed);
    return r.ec == std::errc{} ? std::string(buf, std::size_t(r.ptr - buf)) : std::string{};
}

// the number a plural argument selects with and '#' prints
struct PluralNum {
    double value = 0;     // after the offset
    PluralOperands op;
    int minFraction = 0;  // visible fraction digits of a decimal-string argument
};

class Formatter {
public:
    Formatter(std::string_view locale, std::string_view pattern, const Args& args)
        : m_loc(locale), m_p(pattern), m_args(args) {}

    std::expected<std::string, MessageError> run() {
        std::string out;
        if (!message(&out, 0, nullptr)) return std::unexpected(MessageError{m_errPos, m_err});
        if (m_k < m_p.size()) return std::unexpected(MessageError{m_k, "unmatched '}'"});
        return out;
    }

private:
    std::string_view m_loc, m_p;
    const Args& m_args;
    std::size_t m_k = 0;
    std::string m_err;
    std::size_t m_errPos = 0;

    bool fail(std::string msg) {
        if (m_err.empty()) { m_err = std::move(msg); m_errPos = m_k; }
        return false;
    }
    void ws() { while (m_k < m_p.size() && isWs(m_p[m_k])) ++m_k; }
    static void put(std::string* out, std::string_view s) { if (out) *out += s; }

    // text up to an unquoted '}' (depth > 0) or the end; pc set directly inside a plural sub-message
    bool message(std::string* out, int depth, const PluralNum* pc) {
        while (m_k < m_p.size()) {
            const char c = m_p[m_k];
            if (c == '}') {
                if (depth == 0) return fail("unmatched '}'");
                return true;
            }
            if (c == '{') {
                ++m_k;
                if (!argument(out, depth + 1)) return false;
                continue;
            }
            if (c == '#' && pc) {
                ++m_k;
                if (out) *out += formatNumber(m_loc, pc->value, {pc->minFraction, pc->minFraction > 3 ? pc->minFraction : 3, true});
                continue;
            }
            if (c == '\'') {
                quote(out, pc != nullptr);
                continue;
            }
            if (out) *out += c;
            ++m_k;
        }
        if (depth > 0) return fail("unterminated '{'");
        return true;
    }

    // ICU DOUBLE_OPTIONAL apostrophes
    void quote(std::string* out, bool inPlural) {
        const char n = m_k + 1 < m_p.size() ? m_p[m_k + 1] : '\0';
        if (n == '\'') { put(out, "'"); m_k += 2; return; }
        if (!(n == '{' || n == '}' || n == '|' || (n == '#' && inPlural))) { put(out, "'"); ++m_k; return; }
        ++m_k;
        while (m_k < m_p.size()) {
            if (m_p[m_k] == '\'') {
                if (m_k + 1 < m_p.size() && m_p[m_k + 1] == '\'') { put(out, "'"); m_k += 2; continue; }
                ++m_k;
                return;
            }
            if (out) *out += m_p[m_k];
            ++m_k;
        }
    }

    std::string_view token() {
        const std::size_t b = m_k;
        while (m_k < m_p.size() && !isWs(m_p[m_k]) && m_p[m_k] != ',' && m_p[m_k] != '{' && m_p[m_k] != '}')
            ++m_k;
        return m_p.substr(b, m_k - b);
    }

    void missing(std::string* out, std::string_view name) {
        if (!out) return;
        *out += '{';
        *out += name;
        *out += '}';
    }

    // after '{': name [, type [, style]] '}'
    bool argument(std::string* out, int depth) {
        ws();
        const std::string_view name = token();
        if (name.empty()) return fail("argument name expected");
        ws();
        const Value* v = m_args.find(name);
        if (m_k < m_p.size() && m_p[m_k] == '}') {
            ++m_k;
            if (!out) return true;
            if (!v) missing(out, name);
            else if (const auto* s = std::get_if<std::string>(v)) *out += *s;
            else if (const auto* x = std::get_if<double>(v)) *out += formatNumber(m_loc, *x);
            else *out += formatDate(m_loc, std::get<Date>(*v), DateStyle::Medium);
            return true;
        }
        if (m_k >= m_p.size() || m_p[m_k] != ',') return fail("',' or '}' expected after the argument name");
        ++m_k;
        ws();
        const std::string_view type = token();
        ws();
        if (type == "plural" || type == "selectordinal" || type == "select") {
            if (m_k >= m_p.size() || m_p[m_k] != ',') return fail("',' expected after " + std::string(type));
            ++m_k;
            return type == "select" ? select(out, v, name, depth) : plural(out, v, name, depth, type == "plural");
        }
        if (type != "number" && type != "date") return fail("unsupported argument type '" + std::string(type) + "'");
        std::string style; // simple style: text up to the closing brace, apostrophe-quoting allowed
        if (m_k < m_p.size() && m_p[m_k] == ',') {
            ++m_k;
            while (m_k < m_p.size() && m_p[m_k] != '}') {
                if (m_p[m_k] == '{') return fail("'{' in an argument style");
                if (m_p[m_k] == '\'') { quote(&style, false); continue; }
                style += m_p[m_k++];
            }
            while (!style.empty() && isWs(style.back())) style.pop_back();
            std::size_t b = 0;
            while (b < style.size() && isWs(style[b])) ++b;
            style.erase(0, b);
        }
        if (m_k >= m_p.size() || m_p[m_k] != '}') return fail("'}' expected");
        ++m_k;
        if (type == "number") {
            if (!style.empty() && style != "integer" && style != "percent")
                return fail("unsupported number style '" + style + "'");
        } else if (!style.empty() && style != "short" && style != "medium" && style != "long" && style != "full") {
            return fail("unsupported date style '" + style + "'");
        }
        if (!out) return true;
        if (!v) { missing(out, name); return true; }
        if (const auto* s = std::get_if<std::string>(v)) { *out += *s; return true; }
        if (type == "number") {
            const double x = std::holds_alternative<double>(*v) ? std::get<double>(*v) : 0.0;
            if (!std::holds_alternative<double>(*v)) { missing(out, name); return true; }
            if (style == "percent") *out += formatPercent(m_loc, x);
            else *out += formatNumber(m_loc, x, {0, style == "integer" ? 0 : 3, true});
            return true;
        }
        if (!std::holds_alternative<Date>(*v)) { missing(out, name); return true; }
        const DateStyle ds = style == "short" ? DateStyle::Short : (style == "long" || style == "full") ? DateStyle::Long
                                                                                                         : DateStyle::Medium;
        *out += formatDate(m_loc, std::get<Date>(*v), ds);
        return true;
    }

    // selector {message} pairs up to the closing '}': every case is validated, the best ranked one (lowest
    // pick() >= 0, else "other") is formatted
    template <class Pick> bool cases(std::string* out, int depth, const PluralNum* pc, Pick pick) {
        std::size_t chosen = std::string_view::npos, other = std::string_view::npos;
        int chosenRank = 1 << 30;
        for (;;) {
            ws();
            if (m_k >= m_p.size()) return fail("unterminated '{'");
            if (m_p[m_k] == '}') break;
            const std::string_view sel = token();
            if (sel.empty()) return fail("selector expected");
            ws();
            if (m_k >= m_p.size() || m_p[m_k] != '{') return fail("'{' expected after selector '" + std::string(sel) + "'");
            ++m_k;
            const std::size_t start = m_k;
            if (!message(nullptr, depth + 1, pc)) return false;
            ++m_k; // '}'
            const int rank = pick(sel);
            if (rank >= 0 && rank < chosenRank) { chosenRank = rank; chosen = start; }
            if (sel == "other") other = start;
        }
        if (other == std::string_view::npos) return fail("an 'other' case is required");
        const std::size_t end = ++m_k; // past the argument's '}'
        if (out) {
            m_k = chosen != std::string_view::npos ? chosen : other;
            if (!message(out, depth + 1, pc)) return false;
            m_k = end;
        }
        return true;
    }

    bool select(std::string* out, const Value* v, std::string_view name, int depth) {
        std::string key;
        bool have = v != nullptr;
        if (v) {
            if (const auto* s = std::get_if<std::string>(v)) key = *s;
            else if (const auto* x = std::get_if<double>(v)) key = shortest(*x);
            else have = false;
        }
        if (!cases(have ? out : nullptr, depth, nullptr, [&](std::string_view sel) { return sel == key ? 0 : -1; }))
            return false;
        if (!have) missing(out, name);
        return true;
    }

    bool plural(std::string* out, const Value* v, std::string_view name, int depth, bool cardinal) {
        ws();
        double offset = 0;
        if (m_p.substr(m_k).starts_with("offset:")) {
            m_k += 7;
            ws();
            const std::size_t b = m_k;
            while (m_k < m_p.size() && ((m_p[m_k] >= '0' && m_p[m_k] <= '9') || m_p[m_k] == '.')) ++m_k;
            if (std::from_chars(m_p.data() + b, m_p.data() + m_k, offset).ec != std::errc{}) return fail("bad offset");
        }
        PluralNum pn;
        double raw = 0;
        bool have = false;
        if (v) {
            if (const auto* x = std::get_if<double>(v)) {
                raw = *x;
                have = std::isfinite(raw);
                pn.op = PluralOperands::fromDouble(raw - offset);
            } else if (const auto* s = std::get_if<std::string>(v)) {
                PluralOperands op; // a decimal string keeps its visible fraction digits ("1.50": v = 2)
                if (PluralOperands::parse(*s, op)) {
                    have = true;
                    raw = (*s)[0] == '-' ? -op.n : op.n;
                    if (offset == 0) { pn.op = op; pn.minFraction = int(op.v); }
                    else pn.op = PluralOperands::fromDouble(raw - offset);
                }
            }
        }
        pn.value = raw - offset;
        const Plural cat = cardinal ? pluralCardinal(m_loc, pn.op) : pluralOrdinal(m_loc, pn.op);
        auto pick = [&](std::string_view sel) -> int {
            if (sel.size() > 1 && sel[0] == '=') { // exact match on the value before the offset
                double e = 0;
                const auto r = std::from_chars(sel.data() + 1, sel.data() + sel.size(), e);
                return r.ec == std::errc{} && r.ptr == sel.data() + sel.size() && e == raw ? 0 : -1;
            }
            return sel == pluralName(cat) ? 1 : -1;
        };
        if (!cases(have ? out : nullptr, depth, &pn, pick)) return false;
        if (!have) missing(out, name);
        return true;
    }
};
} // namespace

std::expected<std::string, MessageError> formatMessage(std::string_view locale, std::string_view pattern,
                                                       const Args& args) {
    return Formatter(locale, pattern, args).run();
}

std::string formatMessageOr(std::string_view locale, std::string_view pattern, const Args& args) {
    auto r = formatMessage(locale, pattern, args);
    return r ? std::move(*r) : std::string(pattern);
}

} // namespace fuse::ui::l10n
