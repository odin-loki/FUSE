// gettext .po catalogues as string-table entries (l10n.hpp: parsePo / poToTable). The reader of the gui-framework
// branch's extras/i18n.hpp, folded into this library: plural forms become ICU plural messages on the CLDR categories,
// so a .po catalogue goes through the same StringTable / formatMessage path as a "key=value" table.
#include "fuse/ui/l10n/l10n.hpp"

#include <cstdint>
#include <cstdlib>

namespace fuse::ui::l10n {

namespace {
std::string_view trimWs(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

/// The quoted part of a .po line ("text with \n escapes"), appended to `out`. False when the quotes are malformed.
bool appendQuoted(std::string_view s, std::string& out) {
    s = trimWs(s);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    s = s.substr(1, s.size() - 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            if (s[i] == '"') return false; // an unescaped quote inside
            out += s[i];
            continue;
        }
        if (++i >= s.size()) return false;
        switch (s[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        default: out += '\\'; out += s[i]; break;
        }
    }
    return true;
}

/// The C expression of a Plural-Forms header ("n%10==1 && n%100!=11 ? 0 : n != 1"), evaluated for one n.
struct PluralExpr {
    std::string_view s;
    std::size_t i = 0;
    std::uint64_t n = 0;
    bool ok = true;
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; }
    bool eat(std::string_view tok) {
        ws();
        if (s.substr(i, tok.size()) != tok) return false;
        i += tok.size();
        return true;
    }
    std::int64_t primary() {
        ws();
        if (eat("(")) {
            const std::int64_t v = ternary();
            if (!eat(")")) ok = false;
            return v;
        }
        if (eat("!")) return !primary();
        if (i < s.size() && s[i] == 'n') { ++i; return std::int64_t(n); }
        if (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            std::int64_t v = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
            return v;
        }
        ok = false;
        return 0;
    }
    std::int64_t mul() {
        std::int64_t v = primary();
        for (;;) {
            if (eat("*")) v *= primary();
            else if (eat("/")) { const std::int64_t d = primary(); v = d ? v / d : 0; }
            else if (eat("%")) { const std::int64_t d = primary(); v = d ? v % d : 0; }
            else return v;
        }
    }
    std::int64_t add() {
        std::int64_t v = mul();
        for (;;) {
            if (eat("+")) v += mul();
            else if (eat("-")) v -= mul();
            else return v;
        }
    }
    std::int64_t rel() {
        std::int64_t v = add();
        for (;;) {
            if (eat("<=")) v = v <= add();
            else if (eat(">=")) v = v >= add();
            else if (eat("<")) v = v < add();
            else if (eat(">")) v = v > add();
            else return v;
        }
    }
    std::int64_t eq() {
        std::int64_t v = rel();
        for (;;) {
            if (eat("==")) v = v == rel();
            else if (eat("!=")) v = v != rel();
            else return v;
        }
    }
    std::int64_t land() {
        std::int64_t v = eq();
        while (eat("&&")) { const std::int64_t r = eq(); v = v && r; }
        return v;
    }
    std::int64_t lor() {
        std::int64_t v = land();
        while (eat("||")) { const std::int64_t r = land(); v = v || r; }
        return v;
    }
    std::int64_t ternary() {
        const std::int64_t c = lor();
        if (!eat("?")) return c;
        const std::int64_t a = ternary();
        if (!eat(":")) ok = false;
        const std::int64_t b = ternary();
        return c ? a : b;
    }
};
/// Form index of `n` under `expr` (empty: the Germanic rule n != 1); -1 when the expression does not parse.
int formIndex(std::string_view expr, std::uint64_t n) {
    if (expr.empty()) return n != 1 ? 1 : 0;
    PluralExpr e{expr, 0, n};
    const std::int64_t v = e.ternary();
    e.ws();
    return e.ok && e.i == expr.size() && v >= 0 ? int(v) : -1;
}

/// Literal translation text as an ICU sub-message: quotes the syntax characters, `%n` / `%d` become '#'.
std::string icuPluralText(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == 'd')) { out += '#'; ++i; }
        else if (c == '\'') out += "''";
        else if (c == '{' || c == '}' || c == '#') { out += '\''; out += c; out += '\''; }
        else out += c;
    }
    return out;
}

struct Entry {
    std::string ctxt, id, idPlural, str;
    std::vector<std::string> forms;
    bool fuzzy = false, hasId = false, hasPlural = false, sawStr = false; ///< sawStr: a msgstr line was read
};
} // namespace

std::string poKey(std::string_view context, std::string_view msgid) {
    if (context.empty()) return std::string(msgid);
    std::string k(context);
    k += '\x04'; // gettext's own context separator
    k += msgid;
    return k;
}

std::expected<PoCatalog, MessageError> parsePo(std::string_view po) {
    PoCatalog cat;
    std::string pluralExpr;
    Entry cur;
    std::string* target = nullptr; // the string the next continuation line extends
    std::size_t lineStart = 0;
    const auto fail = [&](const char* what) { return std::unexpected(MessageError{lineStart, what}); };
    const auto flush = [&] {
        const Entry e = std::move(cur);
        cur = {};
        target = nullptr;
        if (!e.hasId) return;
        if (e.id.empty() && e.ctxt.empty()) { // the header entry
            for (std::size_t at = 0; at < e.str.size();) {
                std::size_t nl = e.str.find('\n', at);
                if (nl == std::string::npos) nl = e.str.size();
                const std::string_view line = std::string_view(e.str).substr(at, nl - at);
                at = nl + 1;
                if (line.starts_with("Language:")) cat.language = std::string(trimWs(line.substr(9)));
                else if (line.starts_with("Plural-Forms:")) {
                    const std::size_t p = line.find("plural=");
                    if (p != std::string_view::npos) {
                        std::string_view expr = line.substr(p + 7);
                        if (const std::size_t semi = expr.find(';'); semi != std::string_view::npos) expr = expr.substr(0, semi);
                        pluralExpr = std::string(trimWs(expr));
                    }
                }
            }
            return;
        }
        if (e.fuzzy) { ++cat.skippedFuzzy; return; }
        if (!e.hasPlural) {
            if (e.str.empty()) { ++cat.untranslated; return; }
            cat.entries.push_back({poKey(e.ctxt, e.id), e.str});
            return;
        }
        bool any = false;
        for (const std::string& f : e.forms) any = any || !f.empty();
        if (!any) { ++cat.untranslated; return; }
        // msgstr[i] -> the CLDR categories whose numbers gettext sends to form i (sampled over 0..200 and large ones)
        int formOf[6] = {-1, -1, -1, -1, -1, -1};
        const auto sample = [&](std::uint64_t n) {
            const int form = formIndex(pluralExpr, n);
            const Plural p = pluralCardinal(cat.language.empty() ? std::string_view("en") : std::string_view(cat.language),
                                            PluralOperands::fromInt(std::int64_t(n)));
            if (form >= 0 && formOf[int(p)] < 0) formOf[int(p)] = form;
        };
        for (std::uint64_t n = 0; n <= 200; ++n) sample(n);
        for (std::uint64_t n : {1000ull, 1001ull, 1002ull, 1005ull, 1000000ull, 1000001ull, 2000000ull}) sample(n);
        const int last = int(e.forms.size()) - 1;
        if (formOf[int(Plural::Other)] < 0) formOf[int(Plural::Other)] = last;
        std::string msg = "{n, plural,";
        for (int p = 0; p < 6; ++p) {
            if (formOf[p] < 0) continue;
            const std::string& form = e.forms[std::size_t(formOf[p] <= last ? formOf[p] : last)];
            msg += ' ';
            msg += pluralName(Plural(p));
            msg += " {";
            msg += icuPluralText(form.empty() ? (p == int(Plural::One) ? e.id : e.idPlural) : form);
            msg += '}';
        }
        msg += '}';
        cat.entries.push_back({poKey(e.ctxt, e.id), std::move(msg)});
    };

    for (std::size_t at = 0; at <= po.size();) {
        std::size_t nl = po.find('\n', at);
        if (nl == std::string_view::npos) nl = po.size();
        lineStart = at;
        const std::string_view line = trimWs(po.substr(at, nl - at));
        at = nl + 1;
        if (line.empty()) { flush(); continue; }
        if (line[0] == '#') {
            if (cur.sawStr) flush(); // a comment after the msgstr belongs to the next entry (no blank line between)
            if (line.starts_with("#,") && line.find("fuzzy") != std::string_view::npos) cur.fuzzy = true;
            continue; // "#~" obsolete entries are comments too
        }
        if (line[0] == '"') {
            if (!target || !appendQuoted(line, *target)) return fail("stray or malformed string line");
            continue;
        }
        const auto field = [&](std::string_view name, std::string& into) {
            if (!line.starts_with(name)) return false;
            const std::string_view rest = line.substr(name.size());
            if (rest.empty() || (rest[0] != ' ' && rest[0] != '\t' && rest[0] != '"')) return false;
            into.clear();
            target = appendQuoted(rest, into) ? &into : nullptr;
            return true;
        };
        if (line.starts_with("msgid_plural")) {
            if (!field("msgid_plural", cur.idPlural) || !target) return fail("malformed msgid_plural");
            cur.hasPlural = true;
        } else if (line.starts_with("msgid")) {
            if (cur.sawStr) flush(); // entries not separated by a blank line
            if (!field("msgid", cur.id) || !target) return fail("malformed msgid");
            cur.hasId = true;
        } else if (line.starts_with("msgctxt")) {
            if (cur.sawStr) flush();
            if (!field("msgctxt", cur.ctxt) || !target) return fail("malformed msgctxt");
        } else if (line.starts_with("msgstr[")) {
            const std::size_t close = line.find(']');
            if (close == std::string_view::npos) return fail("malformed msgstr[n]");
            const int index = std::atoi(std::string(line.substr(7, close - 7)).c_str());
            if (index < 0 || index > 15) return fail("msgstr[n] index out of range");
            if (cur.forms.size() <= std::size_t(index)) cur.forms.resize(std::size_t(index) + 1);
            std::string& into = cur.forms[std::size_t(index)];
            into.clear();
            if (!appendQuoted(line.substr(close + 1), into)) return fail("malformed msgstr[n]");
            target = &into;
            cur.sawStr = true;
        } else if (line.starts_with("msgstr")) {
            if (!field("msgstr", cur.str) || !target) return fail("malformed msgstr");
            cur.sawStr = true;
        } else {
            return fail("unknown keyword");
        }
    }
    flush();
    return cat;
}

std::string poToTable(const PoCatalog& cat) {
    std::string out;
    for (const TableEntry& e : cat.entries) {
        out += escapeTableKey(e.key);
        out += '=';
        out += escapeTableValue(e.value);
        out += '\n';
    }
    return out;
}

} // namespace fuse::ui::l10n
