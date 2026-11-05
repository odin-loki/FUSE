// String-table files: "key=value" lines with backslash escapes.
#include "fuse/ui/l10n/l10n.hpp"

namespace fuse::ui::l10n {

std::string unescapeTable(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t k = 0; k < s.size(); ++k) {
        if (s[k] != '\\' || k + 1 == s.size()) { out += s[k]; continue; }
        const char c = s[++k];
        out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c; // \\ \= \# and unknown escapes: the char
    }
    return out;
}

bool parseTableLine(std::string_view line, std::string& key, std::string& value) {
    for (std::size_t k = 0; k < line.size(); ++k) {
        if (line[k] == '\\') { ++k; continue; }
        if (line[k] == '=') {
            key = unescapeTable(line.substr(0, k));
            value = unescapeTable(line.substr(k + 1));
            return true;
        }
    }
    return false;
}

std::vector<TableEntry> parseTable(std::string_view text) {
    std::vector<TableEntry> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == text.npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        TableEntry e;
        if (parseTableLine(line, e.key, e.value)) out.push_back(std::move(e));
    }
    return out;
}

namespace {
std::string escape(std::string_view s, bool key) {
    std::string out;
    for (std::size_t k = 0; k < s.size(); ++k) {
        const char c = s[k];
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (key && (c == '=' || (c == '#' && k == 0))) { out += '\\'; out += c; }
        else out += c;
    }
    return out;
}
} // namespace

std::string escapeTableKey(std::string_view key) { return escape(key, true); }
std::string escapeTableValue(std::string_view value) { return escape(value, false); }

} // namespace fuse::ui::l10n
