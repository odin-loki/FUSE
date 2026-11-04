// fuse_ui_extract_strings: collects translatable strings and writes / merges a string-table file (the format
// StringTable::add and l10n::parseTable read), like Qt's lupdate.
//   fuse_ui_extract_strings --out=<table> [--locale=<tag>] [--source] [--fn=tr,...] [--no-locations]
//                           [--drop-obsolete] inputs...
// Inputs are files or directories (searched recursively, in sorted order) of .fuseui documents and C++ sources.
// .fuseui: constant "text" (not bindings, not line / text edit contents), "items" and "a11yName" strings. C++: calls
// fn("literal" ...) whose first argument is one string literal or adjacent literals (escapes decoded, raw and u8
// literals included); calls with any other first argument are skipped.
// An existing <table> is merged: its translations are kept, keys still missing a translation are written as
// "#? key", new keys are flagged "#, new", and translations whose key left the sources are kept as "#~ key=value"
// (obsolete, restored if the key comes back) unless --drop-obsolete. With --source (the source-language table) new
// keys are written active as key=key. Output is sorted by key; a summary and the new / obsolete keys go to stdout.
#include "fuse/ui/doc/json.hpp"
#include "fuse/ui/l10n/l10n.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace l10n = fuse::ui::l10n;

namespace {

// "file:where", ordered by file, then numerically by line
struct LocationLess {
    bool operator()(const std::string& a, const std::string& b) const {
        const std::size_t ca = a.rfind(':'), cb = b.rfind(':');
        const std::string_view fa = std::string_view(a).substr(0, ca), fb = std::string_view(b).substr(0, cb);
        if (fa != fb) return fa < fb;
        const std::string_view wa = std::string_view(a).substr(ca + 1), wb = std::string_view(b).substr(cb + 1);
        const bool na = !wa.empty() && wa.find_first_not_of("0123456789") == wa.npos;
        const bool nb = !wb.empty() && wb.find_first_not_of("0123456789") == wb.npos;
        if (na && nb && wa.size() != wb.size()) return wa.size() < wb.size();
        return wa < wb;
    }
};
using Strings = std::map<std::string, std::set<std::string, LocationLess>>; // key -> locations

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool isCpp(const fs::path& p) {
    const std::string e = p.extension().string();
    return e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c" || e == ".h" || e == ".hpp" || e == ".hh" ||
           e == ".hxx" || e == ".inl" || e == ".ipp";
}

// ---- .fuseui ----
void walkNode(const fuse::ui::doc::JsonValue& n, const std::string& file, const std::string& path, Strings& out) {
    if (!n.isObject()) return;
    const auto* cls = n.get("class");
    const auto* id = n.get("id");
    const std::string where = file + ":" + (id && id->isString() && !id->string.empty() ? id->string : path);
    auto add = [&](const std::string& s) {
        if (!s.empty() && s.find('{') == std::string::npos) out[s].insert(where); // '{' = a binding template
    };
    const bool editor = cls && cls->isString() && (cls->string == "lineedit" || cls->string == "textedit");
    if (const auto* t = n.get("text"); t && t->isString() && !editor) add(t->string);
    if (const auto* a = n.get("a11yName"); a && a->isString()) add(a->string);
    if (const auto* items = n.get("items"); items && items->isArray())
        for (const auto& it : items->array)
            if (it.isString()) add(it.string);
    if (const auto* ch = n.get("children"); ch && ch->isArray())
        for (std::size_t k = 0; k < ch->array.size(); ++k)
            walkNode(ch->array[k], file, path + "/children[" + std::to_string(k) + "]", out);
}

bool scanFuseui(const std::string& text, const std::string& file, Strings& out) {
    auto doc = fuse::ui::doc::parseJson(text);
    if (!doc) {
        std::fprintf(stderr, "%s:%u:%u: %s\n", file.c_str(), doc.error().line, doc.error().column,
                     doc.error().message.c_str());
        return false;
    }
    if (const auto* root = doc->get("root")) walkNode(*root, file, "root", out);
    return true;
}

// ---- C++ ----
void appendUtf8(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool identStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool identChar(char c) { return identStart(c) || (c >= '0' && c <= '9'); }

class CppScanner {
public:
    CppScanner(std::string_view src, std::string file, const std::vector<std::string>& fns, Strings& out)
        : m_s(src), m_file(std::move(file)), m_fns(fns), m_out(out) {}

    void run() {
        while (m_k < m_s.size()) {
            const char c = m_s[m_k];
            if (skipComment()) continue;
            if (c == '"') { std::string ignored; literal(ignored, false); continue; }
            if (c == '\'') { charLiteral(); continue; }
            if (c >= '0' && c <= '9') { // numbers, with ' digit separators
                while (m_k < m_s.size() && (identChar(m_s[m_k]) || m_s[m_k] == '\'' || m_s[m_k] == '.')) ++m_k;
                continue;
            }
            if (!identStart(c)) { ++m_k; continue; }
            const std::size_t b = m_k;
            while (m_k < m_s.size() && identChar(m_s[m_k])) ++m_k;
            const std::string_view id = m_s.substr(b, m_k - b);
            if (m_k < m_s.size() && m_s[m_k] == '"' && isPrefix(id)) { // prefixed literal
                std::string ignored;
                literal(ignored, id.back() == 'R');
                continue;
            }
            if (std::find(m_fns.begin(), m_fns.end(), id) != m_fns.end()) call(b);
        }
    }

private:
    std::string_view m_s;
    std::string m_file;
    const std::vector<std::string>& m_fns;
    Strings& m_out;
    std::size_t m_k = 0;

    static bool isPrefix(std::string_view id) {
        return id == "u8" || id == "u" || id == "U" || id == "L" || id == "R" || id == "u8R" || id == "uR" ||
               id == "UR" || id == "LR";
    }

    bool skipComment() {
        if (m_s.compare(m_k, 2, "//") == 0) {
            const std::size_t e = m_s.find('\n', m_k);
            m_k = e == m_s.npos ? m_s.size() : e;
            return true;
        }
        if (m_s.compare(m_k, 2, "/*") == 0) {
            const std::size_t e = m_s.find("*/", m_k + 2);
            m_k = e == m_s.npos ? m_s.size() : e + 2;
            return true;
        }
        return false;
    }
    void skipSpace() {
        for (;;) {
            while (m_k < m_s.size() && (m_s[m_k] == ' ' || m_s[m_k] == '\t' || m_s[m_k] == '\n' || m_s[m_k] == '\r'))
                ++m_k;
            if (!skipComment()) return;
        }
    }
    void charLiteral() {
        ++m_k;
        while (m_k < m_s.size() && m_s[m_k] != '\'' && m_s[m_k] != '\n') m_k += m_s[m_k] == '\\' ? 2 : 1;
        ++m_k;
    }

    // at '"' (after any prefix): appends the decoded literal; false if malformed
    bool literal(std::string& out, bool raw) {
        ++m_k;
        if (raw) {
            const std::size_t open = m_s.find('(', m_k);
            if (open == m_s.npos) { m_k = m_s.size(); return false; }
            const std::string close = ")" + std::string(m_s.substr(m_k, open - m_k)) + "\"";
            const std::size_t e = m_s.find(close, open + 1);
            if (e == m_s.npos) { m_k = m_s.size(); return false; }
            out += m_s.substr(open + 1, e - open - 1);
            m_k = e + close.size();
            return true;
        }
        while (m_k < m_s.size()) {
            const char c = m_s[m_k++];
            if (c == '"') return true;
            if (c == '\n') return false;
            if (c != '\\') { out += c; continue; }
            if (m_k >= m_s.size()) return false;
            const char e = m_s[m_k++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case '\n': break; // line continuation
            case 'x': {
                unsigned v = 0;
                while (m_k < m_s.size() && hexVal(m_s[m_k]) >= 0) v = v * 16 + unsigned(hexVal(m_s[m_k++]));
                out += char(v & 0xFF);
                break;
            }
            case 'u': case 'U': {
                std::uint32_t v = 0;
                for (int n = e == 'u' ? 4 : 8; n > 0 && m_k < m_s.size() && hexVal(m_s[m_k]) >= 0; --n)
                    v = v * 16 + std::uint32_t(hexVal(m_s[m_k++]));
                appendUtf8(out, v);
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    unsigned v = unsigned(e - '0');
                    for (int n = 0; n < 2 && m_k < m_s.size() && m_s[m_k] >= '0' && m_s[m_k] <= '7'; ++n)
                        v = v * 8 + unsigned(m_s[m_k++] - '0');
                    out += char(v & 0xFF);
                } else {
                    out += e; // \\ \" \' \?
                }
            }
        }
        return false;
    }

    // after the function name at `at`: fn ( literal literal ... [,)]
    void call(std::size_t at) {
        const std::size_t save = m_k;
        skipSpace();
        if (m_k >= m_s.size() || m_s[m_k] != '(') { m_k = save; return; }
        ++m_k;
        std::string text;
        int pieces = 0;
        for (;;) {
            skipSpace();
            std::size_t b = m_k;
            while (m_k < m_s.size() && identChar(m_s[m_k])) ++m_k;
            const std::string_view prefix = m_s.substr(b, m_k - b);
            if (m_k >= m_s.size() || m_s[m_k] != '"' || !(prefix.empty() || prefix == "u8" || prefix == "R" || prefix == "u8R")) {
                m_k = b;
                break;
            }
            if (!literal(text, !prefix.empty() && prefix.back() == 'R')) { m_k = save; return; }
            ++pieces;
        }
        skipSpace();
        if (pieces == 0 || m_k >= m_s.size() || (m_s[m_k] != ',' && m_s[m_k] != ')')) { m_k = save; return; }
        const std::size_t line = 1 + std::size_t(std::count(m_s.begin(), m_s.begin() + std::ptrdiff_t(at), '\n'));
        if (!text.empty()) m_out[text].insert(m_file + ":" + std::to_string(line));
    }
};

// ---- table file ----
struct Existing {
    std::map<std::string, std::string> translated, obsolete;
    std::set<std::string> untranslated;
};

Existing readExisting(const std::string& text) {
    Existing ex;
    std::string_view t = text;
    while (!t.empty()) {
        const std::size_t nl = t.find('\n');
        std::string_view line = t.substr(0, nl);
        t = nl == t.npos ? std::string_view{} : t.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::string k, v;
        if (line.starts_with("#? ")) ex.untranslated.insert(l10n::unescapeTable(line.substr(3)));
        else if (line.starts_with("#~ ")) { if (l10n::parseTableLine(line.substr(3), k, v)) ex.obsolete[k] = v; }
        else if (!line.empty() && line[0] != '#' && l10n::parseTableLine(line, k, v)) ex.translated[k] = v;
    }
    return ex;
}

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        const std::size_t c = s.find(',');
        if (c) out.emplace_back(s.substr(0, c));
        s = c == s.npos ? std::string_view{} : s.substr(c + 1);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string out, locale;
    bool source = false, locations = true, dropObsolete = false;
    std::vector<std::string> fns{"tr"};
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.starts_with("--out=")) out = a.substr(6);
        else if (a.starts_with("--locale=")) locale = a.substr(9);
        else if (a.starts_with("--fn=")) fns = split(a.substr(5));
        else if (a == "--source") source = true;
        else if (a == "--no-locations") locations = false;
        else if (a == "--drop-obsolete") dropObsolete = true;
        else if (a.starts_with("--")) { std::fprintf(stderr, "fuse_ui_extract_strings: unknown option %s\n", argv[i]); return 1; }
        else inputs.emplace_back(a);
    }
    if (out.empty() || inputs.empty() || fns.empty()) {
        std::fprintf(stderr, "usage: fuse_ui_extract_strings --out=<table> [--locale=<tag>] [--source] [--fn=tr,...] "
                             "[--no-locations] [--drop-obsolete] inputs...\n");
        return 1;
    }

    // inputs: files as given, directories expanded and sorted
    std::vector<fs::path> files;
    for (const fs::path& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            std::vector<fs::path> found;
            for (const auto& e : fs::recursive_directory_iterator(in, ec))
                if (e.is_regular_file() && (e.path().extension() == ".fuseui" || isCpp(e.path()))) found.push_back(e.path());
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else {
            files.push_back(in);
        }
    }
    Strings strings;
    int errors = 0;
    for (const fs::path& f : files) {
        std::string text;
        const std::string name = f.generic_string();
        if (!readFile(f, text)) { std::fprintf(stderr, "%s: cannot read\n", name.c_str()); ++errors; continue; }
        if (f.extension() == ".fuseui") { if (!scanFuseui(text, name, strings)) ++errors; }
        else CppScanner(text, name, fns, strings).run();
    }
    if (errors) return 1;

    Existing ex;
    if (std::string old; readFile(out, old)) ex = readExisting(old);

    std::string o = "# fuse_ui string table" + (locale.empty() ? std::string{} : ": " + locale) + "\n";
    o += "# written by fuse_ui_extract_strings: key=value, sorted by key; #? untranslated, #, new, #~ obsolete\n";
    std::size_t translated = 0, untranslated = 0;
    std::vector<std::string> fresh, gone;
    for (const auto& [key, where] : strings) {
        o += '\n';
        if (locations)
            for (const std::string& w : where) o += "#: " + w + "\n";
        const bool known = ex.translated.count(key) || ex.untranslated.count(key) || ex.obsolete.count(key);
        if (!known) { o += "#, new\n"; fresh.push_back(key); }
        const auto t = ex.translated.find(key);
        const auto ob = ex.obsolete.find(key);
        if (t != ex.translated.end() || ob != ex.obsolete.end()) {
            o += l10n::escapeTableKey(key) + "=" + l10n::escapeTableValue(t != ex.translated.end() ? t->second : ob->second) + "\n";
            ++translated;
        } else if (source) {
            o += l10n::escapeTableKey(key) + "=" + l10n::escapeTableValue(key) + "\n";
            ++translated;
        } else {
            o += "#? " + l10n::escapeTableKey(key) + "\n";
            ++untranslated;
        }
    }
    std::map<std::string, std::string> obsolete = ex.obsolete;
    for (const auto& [k, v] : ex.translated) obsolete[k] = v;
    std::erase_if(obsolete, [&](const auto& kv) { return strings.count(kv.first) != 0; });
    for (const auto& [k, v] : obsolete) gone.push_back(k);
    if (!obsolete.empty() && !dropObsolete) {
        o += "\n# obsolete: no longer in the sources\n";
        for (const auto& [k, v] : obsolete) o += "#~ " + l10n::escapeTableKey(k) + "=" + l10n::escapeTableValue(v) + "\n";
    }

    std::ofstream f(out, std::ios::binary);
    if (!f || !f.write(o.data(), std::streamsize(o.size()))) {
        std::fprintf(stderr, "%s: cannot write\n", out.c_str());
        return 1;
    }
    std::printf("fuse_ui_extract_strings: %s: %zu strings, %zu translated, %zu untranslated, %zu new, %zu obsolete%s\n",
                out.c_str(), strings.size(), translated, untranslated, fresh.size(), gone.size(),
                dropObsolete && !gone.empty() ? " (dropped)" : "");
    for (const std::string& k : fresh) std::printf("new: %s\n", l10n::escapeTableKey(k).c_str());
    for (const std::string& k : gone) std::printf("obsolete: %s\n", l10n::escapeTableKey(k).c_str());
    return 0;
}
