// Rich text document, editor, layout and the richtextedit widget (see richtext.hpp).
#include "fuse/ui/extras/richtext.hpp"
#include "fuse/ui/extras/widgets_extra.hpp"

#include "fuse/text/text.hpp"
#include "fuse/ui/style.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace fuse::ui::rich {
namespace {
bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }
std::size_t prevCp(const std::string& s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && cont((unsigned char)s[i])) --i;
    return i;
}
std::size_t nextCp(const std::string& s, std::size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && cont((unsigned char)s[i])) ++i;
    return i;
}
bool wordChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }
std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}
void appendUtf8(std::string& o, u32 cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
}
std::string htmlEscape(std::string_view s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}
std::string decodeEntities(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const auto semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) { o += '&'; continue; }
        const std::string_view e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") o += '&';
        else if (e == "lt") o += '<';
        else if (e == "gt") o += '>';
        else if (e == "quot") o += '"';
        else if (e == "apos" || e == "#39") o += '\'';
        else if (e == "nbsp") appendUtf8(o, 0xA0);
        else if (!e.empty() && e[0] == '#') {
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            appendUtf8(o, u32(std::strtoul(std::string(e.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10)));
        } else { o += '&'; continue; }
        i = semi;
    }
    return o;
}
std::string colorHex(Color c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", int(std::lround(c.r * 255.f)), int(std::lround(c.g * 255.f)), int(std::lround(c.b * 255.f)));
    return b;
}
std::string fmtNum(f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", double(v));
    return b;
}
template <class F>
void forEachDecl(std::string_view style, F&& f) {
    while (!style.empty()) {
        const auto semi = style.find(';');
        const std::string_view d = style.substr(0, semi);
        if (const auto colon = d.find(':'); colon != std::string_view::npos) f(lower(trim(d.substr(0, colon))), lower(trim(d.substr(colon + 1))));
        if (semi == std::string_view::npos) break;
        style.remove_prefix(semi + 1);
    }
}
} // namespace

// ---- document -------------------------------------------------------------------------------------------------------------
RichDocument::RichDocument() { clear(); }
void RichDocument::clear() {
    m_text.clear();
    m_fmt.clear();
    m_formats.assign(1, CharFormat{});
    m_blocks.assign(1, BlockFormat{});
}
u32 RichDocument::intern(const CharFormat& f) {
    for (std::size_t i = 0; i < m_formats.size(); ++i) if (m_formats[i] == f) return u32(i);
    m_formats.push_back(f);
    return u32(m_formats.size() - 1);
}
void RichDocument::setPlainText(std::string_view s) { clear(); insert(0, s, {}); }
std::size_t RichDocument::blockAt(std::size_t pos) const {
    pos = std::min(pos, m_text.size());
    return std::size_t(std::count(m_text.begin(), m_text.begin() + std::ptrdiff_t(pos), '\n'));
}
std::size_t RichDocument::blockStart(std::size_t block) const {
    std::size_t p = 0;
    for (std::size_t k = 0; k < block; ++k) {
        p = m_text.find('\n', p);
        if (p == std::string::npos) return m_text.size();
        ++p;
    }
    return p;
}
std::size_t RichDocument::blockEnd(std::size_t block) const {
    const auto e = m_text.find('\n', blockStart(block));
    return e == std::string::npos ? m_text.size() : e;
}
void RichDocument::insert(std::size_t pos, std::string_view s, const CharFormat& fmt) {
    pos = std::min(pos, m_text.size());
    const std::size_t b = blockAt(pos);
    const auto nl = std::size_t(std::count(s.begin(), s.end(), '\n'));
    m_blocks.insert(m_blocks.begin() + std::ptrdiff_t(b + 1), nl, m_blocks[b]);
    m_text.insert(pos, s);
    m_fmt.insert(m_fmt.begin() + std::ptrdiff_t(pos), s.size(), intern(fmt));
}
void RichDocument::erase(std::size_t pos, std::size_t len) {
    pos = std::min(pos, m_text.size());
    len = std::min(len, m_text.size() - pos);
    if (!len) return;
    const std::size_t b = blockAt(pos);
    const auto nl = std::size_t(std::count(m_text.begin() + std::ptrdiff_t(pos), m_text.begin() + std::ptrdiff_t(pos + len), '\n'));
    m_blocks.erase(m_blocks.begin() + std::ptrdiff_t(b + 1), m_blocks.begin() + std::ptrdiff_t(b + 1 + nl));
    m_text.erase(pos, len);
    m_fmt.erase(m_fmt.begin() + std::ptrdiff_t(pos), m_fmt.begin() + std::ptrdiff_t(pos + len));
}
void RichDocument::modifyFormat(std::size_t a, std::size_t b, const std::function<void(CharFormat&)>& fn) {
    b = std::min(b, m_text.size());
    for (std::size_t i = a; i < b; ++i) {
        CharFormat f = m_formats[m_fmt[i]];
        fn(f);
        m_fmt[i] = intern(f);
    }
}
CharFormat RichDocument::formatAt(std::size_t pos) const {
    if (pos > 0 && pos <= m_text.size() && m_text[pos - 1] != '\n') return m_formats[m_fmt[pos - 1]];
    if (pos < m_text.size() && m_text[pos] != '\n') return m_formats[m_fmt[pos]];
    return {};
}
std::vector<Run> RichDocument::runs(std::size_t block) const {
    std::vector<Run> r;
    const std::size_t a = blockStart(block), e = blockEnd(block);
    for (std::size_t i = a; i < e; ++i) {
        if (r.empty() || !(r.back().fmt == m_formats[m_fmt[i]])) r.push_back({{}, m_formats[m_fmt[i]]});
        r.back().text += m_text[i];
    }
    return r;
}
u32 RichDocument::listNumber(std::size_t block) const {
    const BlockFormat& f = m_blocks[block];
    u32 n = 1;
    for (std::size_t k = block; k-- > 0;) {
        const BlockFormat& p = m_blocks[k];
        if (p.list == ListKind::None) break;
        if (p.indent > f.indent) continue;
        if (p.indent < f.indent || p.list != f.list) break;
        ++n;
    }
    return n;
}
bool operator==(const RichDocument& a, const RichDocument& b) {
    if (a.m_text != b.m_text || a.m_blocks != b.m_blocks) return false;
    for (std::size_t i = 0; i < a.m_text.size(); ++i)
        if (a.m_text[i] != '\n' && !(a.formatOfByte(i) == b.formatOfByte(i))) return false;
    return true;
}

// ---- HTML ---------------------------------------------------------------------------------------------------------------
std::string RichDocument::toHtml() const {
    std::string o = "<html><body>\n";
    ListKind open = ListKind::None;
    for (std::size_t k = 0; k < m_blocks.size(); ++k) {
        const BlockFormat& bf = m_blocks[k];
        if (bf.list != open) {
            if (open != ListKind::None) o += open == ListKind::Ordered ? "</ol>\n" : "</ul>\n";
            if (bf.list != ListKind::None) o += bf.list == ListKind::Ordered ? "<ol>\n" : "<ul>\n";
            open = bf.list;
        }
        std::string style;
        if (bf.align != Align::Left) style += std::string("text-align:") + (bf.align == Align::Center ? "center" : bf.align == Align::Right ? "right" : "justify") + ";";
        if (bf.indent) style += "margin-left:" + std::to_string(bf.indent * 2) + "em;";
        const std::string tag = bf.list != ListKind::None ? "li" : bf.heading ? "h" + std::to_string(bf.heading) : "p";
        o += "<" + tag + (style.empty() ? "" : " style=\"" + style + "\"") + ">";
        for (const Run& r : runs(k)) {
            std::string pre, post;
            const CharFormat& f = r.fmt;
            if (!f.link.empty()) { pre += "<a href=\"" + htmlEscape(f.link) + "\">"; post = "</a>" + post; }
            if (f.bold) { pre += "<b>"; post = "</b>" + post; }
            if (f.italic) { pre += "<i>"; post = "</i>" + post; }
            if (f.underline) { pre += "<u>"; post = "</u>" + post; }
            if (f.strike) { pre += "<s>"; post = "</s>" + post; }
            if (f.hasColor || f.size > 0.f) {
                std::string st;
                if (f.hasColor) st += "color:" + colorHex(f.color) + ";";
                if (f.size > 0.f) st += "font-size:" + fmtNum(f.size) + "px;";
                pre += "<span style=\"" + st + "\">";
                post = "</span>" + post;
            }
            o += pre + htmlEscape(r.text) + post;
        }
        o += "</" + tag + ">\n";
    }
    if (open != ListKind::None) o += open == ListKind::Ordered ? "</ol>\n" : "</ul>\n";
    return o + "</body></html>\n";
}

void RichDocument::fromHtml(std::string_view h) {
    clear();
    struct Frame { std::string tag; CharFormat fmt; };
    std::vector<Frame> stack{{"", {}}};
    std::vector<ListKind> lists;
    bool started = false, closed = false, atStart = true;
    int skip = 0;
    auto openBlock = [&](const BlockFormat& bf) {
        if (started) insert(m_text.size(), "\n", {});
        m_blocks.back() = bf;
        started = true;
        closed = false;
        atStart = true;
    };
    auto addText = [&](std::string_view raw) {
        std::string t = decodeEntities(raw), c;
        for (char ch : t) {
            if (std::isspace((unsigned char)ch)) { if (c.empty() || c.back() != ' ') c += ' '; }
            else c += ch;
        }
        if (!started || closed) {
            if (trim(c).empty()) return;
            openBlock({});
        }
        if (atStart && !c.empty() && c[0] == ' ') c.erase(0, 1);
        if (c.empty()) return;
        if (c[0] == ' ' && !m_text.empty() && m_text.back() == ' ') c.erase(0, 1);
        insert(m_text.size(), c, stack.back().fmt);
        atStart = false;
    };
    std::size_t i = 0;
    while (i < h.size()) {
        if (h[i] != '<') {
            const auto lt = h.find('<', i);
            const std::string_view t = h.substr(i, lt == std::string_view::npos ? std::string_view::npos : lt - i);
            if (!skip) addText(t);
            i = lt == std::string_view::npos ? h.size() : lt;
            continue;
        }
        if (h.compare(i, 4, "<!--") == 0) {
            const auto e = h.find("-->", i);
            i = e == std::string_view::npos ? h.size() : e + 3;
            continue;
        }
        const auto gt = h.find('>', i);
        if (gt == std::string_view::npos) break;
        std::string_view tag = h.substr(i + 1, gt - i - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '!' || tag[0] == '?') continue;
        const bool closing = tag[0] == '/';
        if (closing) tag.remove_prefix(1);
        if (!tag.empty() && tag.back() == '/') tag.remove_suffix(1);
        std::size_t ne = 0;
        while (ne < tag.size() && !std::isspace((unsigned char)tag[ne])) ++ne;
        const std::string name = lower(tag.substr(0, ne));
        // attributes
        std::vector<std::pair<std::string, std::string>> attrs;
        for (std::size_t p = ne; p < tag.size();) {
            while (p < tag.size() && std::isspace((unsigned char)tag[p])) ++p;
            std::size_t q = p;
            while (q < tag.size() && tag[q] != '=' && !std::isspace((unsigned char)tag[q])) ++q;
            std::string an = lower(tag.substr(p, q - p)), av;
            if (q < tag.size() && tag[q] == '=') {
                ++q;
                if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                    const char qc = tag[q];
                    const auto e = tag.find(qc, q + 1);
                    av = std::string(tag.substr(q + 1, (e == std::string_view::npos ? tag.size() : e) - q - 1));
                    q = e == std::string_view::npos ? tag.size() : e + 1;
                } else {
                    const std::size_t s0 = q;
                    while (q < tag.size() && !std::isspace((unsigned char)tag[q])) ++q;
                    av = std::string(tag.substr(s0, q - s0));
                }
            }
            if (!an.empty()) attrs.emplace_back(an, decodeEntities(av));
            p = q == p ? p + 1 : q;
        }
        auto attr = [&](std::string_view n) -> std::string {
            for (auto& [k, v] : attrs) if (k == n) return v;
            return {};
        };
        if (name == "head" || name == "style" || name == "script" || name == "title") { skip += closing ? -1 : 1; skip = std::max(skip, 0); continue; }
        const bool heading = name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
        if (name == "p" || name == "div" || name == "li" || name == "blockquote" || heading) {
            if (closing) { closed = true; continue; }
            BlockFormat bf;
            if (heading) bf.heading = u8(name[1] - '0');
            if (name == "li") { bf.list = lists.empty() ? ListKind::Bullet : lists.back(); bf.indent = u8(lists.empty() ? 0 : lists.size() - 1); }
            const std::string al = lower(attr("align"));
            auto setAlign = [&](std::string_view v) {
                if (v == "center") bf.align = Align::Center;
                else if (v == "right") bf.align = Align::Right;
                else if (v == "justify") bf.align = Align::Justify;
            };
            setAlign(al);
            forEachDecl(attr("style"), [&](const std::string& k, const std::string& v) {
                if (k == "text-align") setAlign(v);
                if (k == "margin-left" && v.find("em") != std::string::npos) bf.indent = u8(std::max(int(bf.indent), std::atoi(v.c_str()) / 2));
            });
            openBlock(bf);
            continue;
        }
        if (name == "ul" || name == "ol") {
            if (closing) { if (!lists.empty()) lists.pop_back(); closed = true; }
            else lists.push_back(name == "ol" ? ListKind::Ordered : ListKind::Bullet);
            continue;
        }
        if (name == "br") {
            if (!started || closed) openBlock({});
            const BlockFormat bf = m_blocks.back();
            insert(m_text.size(), "\n", {});
            m_blocks.back() = bf;
            atStart = true;
            continue;
        }
        static const char* kInline[] = {"b", "strong", "i", "em", "u", "ins", "s", "strike", "del", "a", "span", "font", "code"};
        if (std::find_if(std::begin(kInline), std::end(kInline), [&](const char* t) { return name == t; }) == std::end(kInline)) continue;
        if (closing) {
            for (std::size_t k = stack.size(); k-- > 1;)
                if (stack[k].tag == name) { stack.resize(k); break; }
            continue;
        }
        CharFormat f = stack.back().fmt;
        if (name == "b" || name == "strong") f.bold = true;
        else if (name == "i" || name == "em") f.italic = true;
        else if (name == "u" || name == "ins") f.underline = true;
        else if (name == "s" || name == "strike" || name == "del") f.strike = true;
        else if (name == "a") f.link = attr("href");
        if (const std::string c = attr("color"); !c.empty() && fuse::ui::parseColor(c, f.color)) f.hasColor = true;
        forEachDecl(attr("style"), [&](const std::string& k, const std::string& v) {
            if (k == "color" && fuse::ui::parseColor(v, f.color)) f.hasColor = true;
            else if (k == "font-weight") f.bold = v == "bold" || v == "bolder" || std::atoi(v.c_str()) >= 600;
            else if (k == "font-style") f.italic = v == "italic" || v == "oblique";
            else if (k == "text-decoration" || k == "text-decoration-line") {
                if (v.find("underline") != std::string::npos) f.underline = true;
                if (v.find("line-through") != std::string::npos) f.strike = true;
            } else if (k == "font-size") {
                const f32 n = f32(std::atof(v.c_str()));
                if (n > 0.f) f.size = v.find("pt") != std::string::npos ? n * 4.f / 3.f : n;
            }
        });
        stack.push_back({name, f});
    }
}

// ---- Markdown -----------------------------------------------------------------------------------------------------------
std::string RichDocument::toMarkdown() const {
    std::string o;
    for (std::size_t k = 0; k < m_blocks.size(); ++k) {
        const BlockFormat& bf = m_blocks[k];
        if (k) o += (bf.list != ListKind::None && m_blocks[k - 1].list != ListKind::None) ? "\n" : "\n\n";
        if (bf.heading) o += std::string(bf.heading, '#') + " ";
        if (bf.list != ListKind::None) o += std::string(bf.indent * 2u, ' ') + (bf.list == ListKind::Ordered ? std::to_string(listNumber(k)) + ". " : "- ");
        // marker stack: L link, S strike, B bold, I italic, U underline
        std::vector<char> st;
        std::string href;
        auto closeOne = [&] {
            switch (st.back()) {
            case 'L': o += "](" + href + ")"; break;
            case 'S': o += "~~"; break;
            case 'B': o += "**"; break;
            case 'I': o += "_"; break;
            case 'U': o += "</u>"; break;
            }
            st.pop_back();
        };
        for (const Run& r : runs(k)) {
            const CharFormat& f = r.fmt;
            auto want = [&](char m) {
                switch (m) {
                case 'L': return !f.link.empty() && f.link == href;
                case 'S': return f.strike;
                case 'B': return f.bold;
                case 'I': return f.italic;
                default: return f.underline;
                }
            };
            std::size_t keep = 0;
            while (keep < st.size() && want(st[keep])) ++keep;
            while (st.size() > keep) closeOne();
            for (char m : {'L', 'S', 'B', 'I', 'U'}) {
                if (std::find(st.begin(), st.end(), m) != st.end()) continue;
                const bool on = m == 'L' ? !f.link.empty() : want(m);
                if (!on) continue;
                if (m == 'L') { href = f.link; o += "["; }
                else o += m == 'S' ? "~~" : m == 'B' ? "**" : m == 'I' ? "_" : "<u>";
                st.push_back(m);
            }
            for (char c : r.text) {
                if (std::strchr("\\*_~[]`<#", c)) o += '\\';
                o += c;
            }
        }
        while (!st.empty()) closeOne();
    }
    return o + "\n";
}

void RichDocument::fromMarkdown(std::string_view md) {
    clear();
    bool first = true, para = false;
    auto newBlock = [&](const BlockFormat& bf) {
        if (!first) insert(m_text.size(), "\n", {});
        m_blocks.back() = bf;
        first = false;
    };
    auto inlineText = [&](std::string_view s) {
        CharFormat f;
        std::size_t linkEnd = std::string_view::npos;
        std::string linkHref;
        for (std::size_t i = 0; i < s.size();) {
            if (i == linkEnd) {
                f.link.clear();
                i = s.find(')', i) + 1;
                linkEnd = std::string_view::npos;
                continue;
            }
            const char c = s[i];
            if (c == '\\' && i + 1 < s.size()) { insert(m_text.size(), s.substr(i + 1, 1), f); i += 2; continue; }
            if (s.compare(i, 2, "**") == 0 || s.compare(i, 2, "__") == 0) { f.bold = !f.bold; i += 2; continue; }
            if (s.compare(i, 2, "~~") == 0) { f.strike = !f.strike; i += 2; continue; }
            if (c == '*' || c == '_') { f.italic = !f.italic; ++i; continue; }
            if (s.compare(i, 3, "<u>") == 0) { f.underline = true; i += 3; continue; }
            if (s.compare(i, 4, "</u>") == 0) { f.underline = false; i += 4; continue; }
            if (c == '`') {
                const auto e = s.find('`', i + 1);
                if (e != std::string_view::npos) { insert(m_text.size(), s.substr(i + 1, e - i - 1), f); i = e + 1; continue; }
            }
            if (c == '[' && linkEnd == std::string_view::npos) {
                // find the matching unescaped ']' followed by '('
                std::size_t j = i + 1;
                while (j < s.size() && s[j] != ']') j += s[j] == '\\' ? 2 : 1;
                const auto close = j < s.size() && j + 1 < s.size() && s[j + 1] == '(' ? s.find(')', j) : std::string_view::npos;
                if (close != std::string_view::npos) {
                    f.link = std::string(s.substr(j + 2, close - j - 2));
                    linkEnd = j;
                    ++i;
                    continue;
                }
            }
            const std::size_t n = nextCp(std::string(s.substr(i, 4)), 0);
            insert(m_text.size(), s.substr(i, n), f);
            i += n;
        }
    };
    std::size_t p = 0;
    while (p <= md.size()) {
        const auto e = md.find('\n', p);
        std::string_view line = md.substr(p, e == std::string_view::npos ? std::string_view::npos : e - p);
        p = e == std::string_view::npos ? md.size() + 1 : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (trim(line).empty()) { para = false; continue; }
        std::size_t sp = 0;
        while (sp < line.size() && line[sp] == ' ') ++sp;
        const std::string_view body = line.substr(sp);
        std::size_t hn = 0;
        while (hn < body.size() && hn < 7 && body[hn] == '#') ++hn;
        if (hn >= 1 && hn <= 6 && hn < body.size() && body[hn] == ' ') {
            BlockFormat bf;
            bf.heading = u8(hn);
            newBlock(bf);
            inlineText(body.substr(hn + 1));
            para = false;
            continue;
        }
        BlockFormat bf;
        std::size_t skipN = 0;
        if (body.size() > 1 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') { bf.list = ListKind::Bullet; skipN = 2; }
        else {
            std::size_t d = 0;
            while (d < body.size() && std::isdigit((unsigned char)body[d])) ++d;
            if (d && d + 1 < body.size() && body[d] == '.' && body[d + 1] == ' ') { bf.list = ListKind::Ordered; skipN = d + 2; }
        }
        if (bf.list != ListKind::None) {
            bf.indent = u8(sp / 2);
            newBlock(bf);
            inlineText(body.substr(skipN));
            para = false;
            continue;
        }
        if (para) insert(m_text.size(), " ", {});
        else newBlock({});
        inlineText(body);
        para = true;
    }
}

// ---- editor -------------------------------------------------------------------------------------------------------------
void RichTextEditor::snapshot(bool coalescable) {
    if (coalescable && m_coalesce) return;
    m_undo.push_back({doc, cursor, anchor});
    if (m_undo.size() > 256) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_coalesce = coalescable;
}
void RichTextEditor::changed() { if (onChanged) onChanged(); }
void RichTextEditor::setCursor(std::size_t pos, bool select) {
    cursor = std::min(pos, doc.size());
    if (!select) anchor = cursor;
    typing = doc.formatAt(cursor);
    m_coalesce = false;
}
void RichTextEditor::eraseSelection() {
    if (!hasSelection()) return;
    const std::size_t a = selStart();
    doc.erase(a, selEnd() - a);
    cursor = anchor = a;
}
void RichTextEditor::insertText(std::string_view s) {
    if (s.empty()) return;
    snapshot(!hasSelection() && s.find('\n') == std::string_view::npos && s.find(' ') == std::string_view::npos);
    eraseSelection();
    doc.insert(cursor, s, typing);
    cursor = anchor = cursor + s.size();
    changed();
}
void RichTextEditor::newParagraph() {
    const std::size_t b = doc.blockAt(cursor);
    BlockFormat bf = doc.blockFormat(b);
    if (!hasSelection() && bf.list != ListKind::None && doc.blockStart(b) == doc.blockEnd(b)) {
        snapshot();
        bf.list = ListKind::None;
        bf.indent = 0;
        doc.setBlockFormat(b, bf);
        changed();
        return;
    }
    snapshot();
    eraseSelection();
    doc.insert(cursor, "\n", typing);
    ++cursor;
    anchor = cursor;
    const std::size_t nb = doc.blockAt(cursor);
    if (doc.blockFormat(nb).heading) { BlockFormat f = doc.blockFormat(nb); f.heading = 0; doc.setBlockFormat(nb, f); }
    m_coalesce = false;
    changed();
}
void RichTextEditor::backspace() {
    if (hasSelection()) { snapshot(); eraseSelection(); changed(); return; }
    if (cursor == 0 && doc.blockFormat(0).list == ListKind::None && doc.blockFormat(0).indent == 0) return;
    const std::size_t b = doc.blockAt(cursor);
    if (cursor == doc.blockStart(b)) {
        BlockFormat bf = doc.blockFormat(b);
        if (bf.list != ListKind::None || bf.indent) {
            snapshot();
            if (bf.indent) --bf.indent;
            else bf.list = ListKind::None;
            doc.setBlockFormat(b, bf);
            changed();
            return;
        }
    }
    if (cursor == 0) return;
    snapshot(true);
    const std::size_t p = prevCp(doc.plainText(), cursor);
    doc.erase(p, cursor - p);
    cursor = anchor = p;
    changed();
}
void RichTextEditor::del() {
    if (hasSelection()) { snapshot(); eraseSelection(); changed(); return; }
    if (cursor >= doc.size()) return;
    snapshot(true);
    doc.erase(cursor, nextCp(doc.plainText(), cursor) - cursor);
    changed();
}
void RichTextEditor::moveLeft(bool select) {
    if (!select && hasSelection()) { setCursor(selStart()); return; }
    setCursor(prevCp(doc.plainText(), cursor), select);
}
void RichTextEditor::moveRight(bool select) {
    if (!select && hasSelection()) { setCursor(selEnd()); return; }
    setCursor(nextCp(doc.plainText(), cursor), select);
}
void RichTextEditor::wordLeft(bool select) {
    const std::string& t = doc.plainText();
    std::size_t p = cursor;
    while (p > 0 && !wordChar((unsigned char)t[p - 1])) --p;
    while (p > 0 && wordChar((unsigned char)t[p - 1])) --p;
    setCursor(p, select);
}
void RichTextEditor::wordRight(bool select) {
    const std::string& t = doc.plainText();
    std::size_t p = cursor;
    while (p < t.size() && wordChar((unsigned char)t[p])) ++p;
    while (p < t.size() && !wordChar((unsigned char)t[p])) ++p;
    setCursor(p, select);
}
void RichTextEditor::home(bool select) { setCursor(doc.blockStart(doc.blockAt(cursor)), select); }
void RichTextEditor::end(bool select) { setCursor(doc.blockEnd(doc.blockAt(cursor)), select); }
void RichTextEditor::applyFormat(const std::function<void(CharFormat&)>& fn) {
    if (!hasSelection()) { fn(typing); return; }
    snapshot();
    doc.modifyFormat(selStart(), selEnd(), fn);
    changed();
}
namespace {
template <class Get>
bool allSet(const RichTextEditor& e, Get get) {
    if (!e.hasSelection()) return get(e.typing);
    bool any = false;
    for (std::size_t i = e.selStart(); i < e.selEnd(); ++i) {
        if (e.doc.plainText()[i] == '\n') continue;
        any = true;
        if (!get(e.doc.formatOfByte(i))) return false;
    }
    return any;
}
} // namespace
void RichTextEditor::toggleBold() { const bool v = !allSet(*this, [](const CharFormat& f) { return f.bold; }); applyFormat([v](CharFormat& f) { f.bold = v; }); }
void RichTextEditor::toggleItalic() { const bool v = !allSet(*this, [](const CharFormat& f) { return f.italic; }); applyFormat([v](CharFormat& f) { f.italic = v; }); }
void RichTextEditor::toggleUnderline() { const bool v = !allSet(*this, [](const CharFormat& f) { return f.underline; }); applyFormat([v](CharFormat& f) { f.underline = v; }); }
void RichTextEditor::toggleStrike() { const bool v = !allSet(*this, [](const CharFormat& f) { return f.strike; }); applyFormat([v](CharFormat& f) { f.strike = v; }); }
void RichTextEditor::setColor(Color c) { applyFormat([c](CharFormat& f) { f.hasColor = true; f.color = c; }); }
void RichTextEditor::setSize(f32 px) { applyFormat([px](CharFormat& f) { f.size = px; }); }
void RichTextEditor::setLink(std::string_view href) { applyFormat([h = std::string(href)](CharFormat& f) { f.link = h; }); }
void RichTextEditor::modifyBlocks(const std::function<void(BlockFormat&)>& fn) {
    snapshot();
    for (std::size_t b = doc.blockAt(selStart()), e = doc.blockAt(selEnd()); b <= e; ++b) {
        BlockFormat f = doc.blockFormat(b);
        fn(f);
        doc.setBlockFormat(b, f);
    }
    changed();
}
void RichTextEditor::setAlign(Align a) { modifyBlocks([a](BlockFormat& f) { f.align = a; }); }
void RichTextEditor::setList(ListKind k) {
    bool all = true;
    for (std::size_t b = doc.blockAt(selStart()), e = doc.blockAt(selEnd()); b <= e; ++b) all = all && doc.blockFormat(b).list == k;
    modifyBlocks([k, all](BlockFormat& f) { f.list = all ? ListKind::None : k; f.heading = 0; });
}
void RichTextEditor::setHeading(u8 level) { modifyBlocks([level](BlockFormat& f) { f.heading = std::min<u8>(level, 6); }); }
void RichTextEditor::indent(int delta) { modifyBlocks([delta](BlockFormat& f) { f.indent = u8(std::clamp(int(f.indent) + delta, 0, 8)); }); }
bool RichTextEditor::undo() {
    if (m_undo.empty()) return false;
    m_redo.push_back({doc, cursor, anchor});
    doc = std::move(m_undo.back().doc);
    cursor = m_undo.back().cursor;
    anchor = m_undo.back().anchor;
    m_undo.pop_back();
    m_coalesce = false;
    typing = doc.formatAt(cursor);
    changed();
    return true;
}
bool RichTextEditor::redo() {
    if (m_redo.empty()) return false;
    m_undo.push_back({doc, cursor, anchor});
    doc = std::move(m_redo.back().doc);
    cursor = m_redo.back().cursor;
    anchor = m_redo.back().anchor;
    m_redo.pop_back();
    m_coalesce = false;
    typing = doc.formatAt(cursor);
    changed();
    return true;
}

// ---- layout -------------------------------------------------------------------------------------------------------------
f32 headingScale(u8 level) {
    static const f32 s[] = {1.f, 2.f, 1.5f, 1.25f, 1.1f, 1.f, 0.9f};
    return s[std::min<u8>(level, 6)];
}
namespace {
constexpr f32 kIndent = 24.f;
f32 fragSize(const CharFormat& f, f32 blockPx) { return f.size > 0.f ? f.size : blockPx; }
} // namespace
RichLayout layout(const RichDocument& d, f32 width, f32 basePx, const std::function<f32(std::string_view, f32)>& measure,
                  const std::function<f32(f32)>& lineHeight) {
    RichLayout L;
    const std::string& t = d.plainText();
    f32 y = 0.f;
    for (std::size_t b = 0; b < d.blockCount(); ++b) {
        const BlockFormat& bf = d.blockFormat(b);
        const f32 px = basePx * headingScale(bf.heading);
        const f32 x0 = f32(bf.indent) * kIndent + (bf.list != ListKind::None ? kIndent : 0.f);
        const f32 avail = width > 0.f ? std::max(8.f, width - x0) : 1e9f;
        const std::size_t a = d.blockStart(b), e = d.blockEnd(b);
        auto widthOf = [&](std::size_t s0, std::size_t s1) {
            f32 w = 0.f;
            for (std::size_t i = s0; i < s1;) {
                std::size_t j = i;
                while (j < s1 && d.formatOfByte(j) == d.formatOfByte(i)) ++j;
                w += measure(std::string_view(t).substr(i, j - i), fragSize(d.formatOfByte(i), px));
                i = j;
            }
            return w;
        };
        // break into line ranges
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        std::size_t ls = a, i = a;
        f32 lw = 0.f;
        while (i < e) {
            std::size_t we = i;
            while (we < e && t[we] != ' ') ++we;
            std::size_t ws = we;
            while (ws < e && t[ws] == ' ') ++ws;
            const f32 ww = widthOf(i, we);
            if (lw > 0.f && lw + ww > avail) { ranges.push_back({ls, i}); ls = i; lw = 0.f; }
            if (ww > avail) { // the over-long word breaks at code points
                std::size_t k = i;
                while (widthOf(k, we) > avail) {
                    std::size_t m = nextCp(t, k);
                    while (m < we && widthOf(k, nextCp(t, m)) <= avail) m = nextCp(t, m);
                    ranges.push_back({k, m});
                    k = m;
                }
                ls = k;
                lw = widthOf(k, ws);
                i = ws;
                continue;
            }
            lw += widthOf(i, ws);
            i = ws;
        }
        if (ls < e || ranges.empty()) ranges.push_back({ls, e});
        bool firstLine = true;
        for (std::size_t ri = 0; ri < ranges.size(); ++ri) {
            const auto [ra, rb] = ranges[ri];
            RichLine ln{b, ra, rb, 0.f, y, 0.f, 0.f, 0.f, {}, {}};
            f32 x = 0.f, h = lineHeight(px);
            for (std::size_t k = ra; k < rb;) {
                std::size_t j = k;
                while (j < rb && d.formatOfByte(j) == d.formatOfByte(k)) ++j;
                CharFormat f = d.formatOfByte(k);
                if (bf.heading) f.bold = true;
                const f32 s = fragSize(f, px), w = measure(std::string_view(t).substr(k, j - k), s);
                ln.frags.push_back({k, j, x, w, s, f});
                h = std::max(h, lineHeight(s));
                x += w;
                k = j;
            }
            ln.width = x;
            ln.h = h;
            ln.baseline = h;
            f32 off = 0.f;
            const f32 trailing = rb > ra && t[rb - 1] == ' ' ? measure(" ", px) : 0.f;
            // Justify: every line but a paragraph's last spreads the slack over its inner spaces. Fragments are split
            // after each space run so each word starts at its stretched position (caret / hit testing follow).
            if (width > 0.f && bf.align == Align::Justify && ri + 1 < ranges.size()) {
                std::size_t end = rb;
                while (end > ra && t[end - 1] == ' ') --end;
                std::size_t spaces = 0;
                for (std::size_t k = ra; k < end; ++k) spaces += t[k] == ' ';
                f32 natural = 0.f;
                for (const auto& fr : ln.frags) natural += fr.w;
                f32 endW = natural;
                if (end < rb) endW -= measure(std::string_view(t).substr(end, rb - end), px);
                const f32 slack = avail - endW;
                if (spaces > 0 && slack > 0.f) {
                    const f32 gap = slack / f32(spaces);
                    std::vector<RichFragment> out;
                    f32 nx = 0.f;
                    std::size_t seen = 0;
                    for (const RichFragment& fr : ln.frags) {
                        std::size_t s0 = fr.a;
                        for (std::size_t k = fr.a; k <= fr.b; ++k) {
                            const bool cut = k == fr.b || (k > fr.a && t[k - 1] == ' ' && t[k] != ' ');
                            if (!cut) continue;
                            if (k > s0) {
                                const f32 w = measure(std::string_view(t).substr(s0, k - s0), fr.size);
                                out.push_back({s0, k, nx + gap * f32(seen), w, fr.size, fr.fmt});
                                nx += w;
                                for (std::size_t q = s0; q < k && q < end; ++q) seen += t[q] == ' ';
                            }
                            s0 = k;
                        }
                    }
                    ln.frags = std::move(out);
                    ln.width = x = avail + (x - endW);
                }
            }
            if (width > 0.f && bf.align == Align::Center) off = (avail - (x - trailing)) * 0.5f;
            else if (width > 0.f && bf.align == Align::Right) off = avail - (x - trailing);
            ln.x = x0 + std::max(0.f, off);
            for (auto& fr : ln.frags) fr.x += ln.x;
            if (firstLine && bf.list != ListKind::None) ln.marker = bf.list == ListKind::Bullet ? "\xE2\x80\xA2" : std::to_string(d.listNumber(b)) + ".";
            firstLine = false;
            y += h;
            L.lines.push_back(std::move(ln));
        }
        if (bf.heading) y += lineHeight(px) * 0.25f;
    }
    L.height = y;
    return L;
}
std::size_t RichLayout::lineOf(std::size_t pos) const {
    std::size_t r = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].a <= pos && pos <= lines[i].b) r = i;
        else if (lines[i].a > pos) break;
    return r;
}
namespace {
f32 caretX(const RichLine& ln, std::size_t pos, const RichDocument& d, const std::function<f32(std::string_view, f32)>& measure) {
    for (const RichFragment& f : ln.frags)
        if (pos >= f.a && pos <= f.b) return f.x + measure(std::string_view(d.plainText()).substr(f.a, pos - f.a), f.size);
    return ln.frags.empty() ? ln.x : ln.frags.back().x + ln.frags.back().w;
}
} // namespace
Rect RichLayout::caretRect(std::size_t pos, const RichDocument& d, const std::function<f32(std::string_view, f32)>& measure) const {
    if (lines.empty()) return {};
    const RichLine& ln = lines[lineOf(pos)];
    return {caretX(ln, pos, d, measure), ln.y, 1.5f, ln.h};
}
std::size_t RichLayout::hitTest(Vec2 p, const RichDocument& d, const std::function<f32(std::string_view, f32)>& measure) const {
    if (lines.empty()) return 0;
    std::size_t li = lines.size() - 1;
    for (std::size_t i = 0; i < lines.size(); ++i) if (p.y < lines[i].y + lines[i].h) { li = i; break; }
    const RichLine& ln = lines[li];
    std::size_t best = ln.a;
    f32 bd = 1e30f;
    const std::size_t end = (li + 1 < lines.size() && lines[li + 1].block == ln.block && ln.b > ln.a) ? prevCp(d.plainText(), ln.b) : ln.b;
    for (std::size_t q = ln.a;; q = nextCp(d.plainText(), q)) {
        const f32 dist = std::abs(caretX(ln, q, d, measure) - p.x);
        if (dist < bd) { bd = dist; best = q; }
        if (q >= end) break;
    }
    return best;
}

} // namespace fuse::ui::rich

namespace fuse::ui::rich {
// Synthetic oblique (what FreeType / CoreText do without an italic face): glyph quads are axis aligned atlas copies,
// so each one is cut into horizontal strips of its texel rows and every strip moves right by slope x its height above
// the baseline. One-row strips are an exact pixel-level shear (two rows for very large glyphs).
void obliqueGlyphs(DrawList& dl, std::size_t first, f32 baseline, f32 slope) {
    std::vector<DrawCmd>& cmds = dl.cmds();
    std::vector<DrawCmd> out;
    out.reserve(cmds.size() - first);
    for (std::size_t i = first; i < cmds.size(); ++i) {
        const DrawCmd& c = cmds[i];
        if (c.op != DrawOp::Glyph || c.th == 0 || c.rect.h <= 0.f) { out.push_back(c); continue; }
        const u16 step = c.th > 64 ? 2 : 1;
        const f32 sy = c.rect.h / f32(c.th);
        for (u16 r = 0; r < c.th; r += step) {
            const u16 rows = u16(std::min<int>(step, c.th - r));
            DrawCmd s = c;
            s.v = u16(c.v + r);
            s.th = rows;
            s.rect.y = c.rect.y + f32(r) * sy;
            s.rect.h = f32(rows) * sy;
            const f32 mid = s.rect.y + s.rect.h * 0.5f;
            s.rect.x = c.rect.x + std::round((baseline - mid) * slope);
            out.push_back(s);
        }
    }
    cmds.resize(first);
    cmds.insert(cmds.end(), out.begin(), out.end());
}
} // namespace fuse::ui::rich

// ---- widget ---------------------------------------------------------------------------------------------------------------
namespace fuse::ui::widgets {
namespace {
using namespace fuse::ui::rich;
struct RichState {
    RichTextEditor ed;
    RichLayout lay;
    f32 layWidth = -1.f, layPx = 0.f, scroll = 0.f;
    bool dirty = true, readOnly = false, dragging = false;
};
struct RichPool {
    struct Slot { NodeHandle node; std::unique_ptr<RichState> s; };
    std::vector<Slot> slots;
    RichState* get(const UiContext& ui, NodeHandle n) {
        const u64 k = ui.alive(n) ? ui.userData(n) : 0;
        if (k == 0 || k > slots.size() || slots[k - 1].node != n) return nullptr;
        return slots[k - 1].s.get();
    }
    RichState& create(UiContext& ui, NodeHandle n) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!ui.alive(slots[i].node) || ui.userData(slots[i].node) != i + 1) {
                slots[i] = {n, std::make_unique<RichState>()};
                ui.setUserData(n, i + 1);
                return *slots[i].s;
            }
        slots.push_back({n, std::make_unique<RichState>()});
        ui.setUserData(n, slots.size());
        return *slots.back().s;
    }
};
RichPool& richPool() { static RichPool p; return p; }
std::function<f32(std::string_view, f32)> measurer(UiContext& ui) { return [&ui](std::string_view s, f32 px) { return ui.measureString(s, px); }; }
std::function<f32(f32)> lineHeighter(UiContext& ui) {
    return [&ui](f32 px) { return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f); };
}
void ensureLayout(UiContext& ui, NodeHandle n, RichState& s) {
    const Rect c = ui.contentRect(n);
    const f32 px = ui.computed(n).fontSize;
    if (!s.dirty && s.layWidth == c.w && s.layPx == px) return;
    s.lay = rich::layout(s.ed.doc, c.w, px, measurer(ui), lineHeighter(ui));
    s.layWidth = c.w;
    s.layPx = px;
    s.dirty = false;
}
void scrollToCaret(UiContext& ui, NodeHandle n, RichState& s) {
    ensureLayout(ui, n, s);
    const Rect cr = s.lay.caretRect(s.ed.cursor, s.ed.doc, measurer(ui));
    const f32 h = ui.contentRect(n).h;
    if (cr.y < s.scroll) s.scroll = cr.y;
    if (cr.y + cr.h > s.scroll + h) s.scroll = cr.y + cr.h - h;
    s.scroll = std::max(0.f, s.scroll);
}
struct RichW {
    static constexpr std::string_view kName = "richtextedit";
    static constexpr A11yRole kRole = A11yRole::TextField;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {320.f, 160.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        RichState* s = richPool().get(pc.ui, pc.node);
        if (!s) return;
        ensureLayout(pc.ui, pc.node, *s);
        const Rect c = pc.content;
        const auto meas = measurer(pc.ui);
        const Color text = pc.tint(pc.style.color), linkCol = pc.tint(Color::rgb(0x3D8BFD)), selCol = pc.tint(Color::rgb(0x3D8BFD, 0.35f));
        const std::string& t = s->ed.doc.plainText();
        const std::size_t sa = s->ed.selStart(), sb = s->ed.selEnd();
        dl.pushClip(c);
        for (const RichLine& ln : s->lay.lines) {
            const f32 y = c.y + ln.y - s->scroll;
            if (y + ln.h < c.y || y > c.bottom()) continue;
            if (sa != sb && sb >= ln.a && sa <= ln.b) {
                const f32 x0 = s->lay.caretRect(std::max(sa, ln.a), s->ed.doc, meas).x;
                f32 x1 = s->lay.caretRect(std::min(sb, ln.b), s->ed.doc, meas).x;
                if (sb > ln.b) x1 += 4.f; // the paragraph break
                if (x1 > x0) dl.rect({c.x + x0, y, x1 - x0, ln.h}, selCol);
            }
            if (!ln.marker.empty()) {
                const f32 mw = pc.ui.measureString(ln.marker, pc.style.fontSize);
                pc.ui.paintString(dl, ln.marker, {c.x + ln.x - mw - 6.f, y, mw + 2.f, ln.h}, text, pc.style.fontSize * headingScale(s->ed.doc.blockFormat(ln.block).heading), TextAlignment::Start, pc.node.index);
            }
            for (const RichFragment& f : ln.frags) {
                const f32 lh = std::ceil(pc.ui.font() ? pc.ui.font()->metrics(f.size).lineHeight() : f.size * 1.25f);
                const Rect box{c.x + f.x, y + ln.h - lh, f.w + 2.f, lh};
                const Color col = f.fmt.hasColor ? pc.tint(f.fmt.color) : !f.fmt.link.empty() ? linkCol : text;
                const std::string_view str = std::string_view(t).substr(f.a, f.b - f.a);
                const std::size_t first = dl.size();
                pc.ui.paintString(dl, str, box, col, f.size, TextAlignment::Start, pc.node.index);
                if (f.fmt.bold) pc.ui.paintString(dl, str, {box.x + std::max(0.6f, f.size / 24.f), box.y, box.w, box.h}, col, f.size, TextAlignment::Start, pc.node.index);
                if (f.fmt.italic) rich::obliqueGlyphs(dl, first, box.y + lh * 0.78f);
                const f32 th = std::max(1.f, f.size / 14.f);
                if (f.fmt.underline || !f.fmt.link.empty()) dl.line({box.x, box.y + lh * 0.86f}, {box.x + f.w, box.y + lh * 0.86f}, col, th);
                if (f.fmt.strike) dl.line({box.x, box.y + lh * 0.55f}, {box.x + f.w, box.y + lh * 0.55f}, col, th);
            }
        }
        if (pc.ui.focused() == pc.node && !s->readOnly) {
            const Rect cr = s->lay.caretRect(s->ed.cursor, s->ed.doc, meas);
            dl.rect({c.x + cr.x, c.y + cr.y - s->scroll, cr.w, cr.h}, text);
        }
        dl.popClip();
    }
    static std::size_t hit(UiContext& ui, NodeHandle n, RichState& s, Vec2 p) {
        ensureLayout(ui, n, s);
        const Rect c = ui.contentRect(n);
        return s.lay.hitTest({p.x - c.x, p.y - c.y + s.scroll}, s.ed.doc, measurer(ui));
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        RichState* sp = richPool().get(ui, ec.node);
        if (!sp) return false;
        RichState& s = *sp;
        RichTextEditor& ed = s.ed;
        const bool shift = e.mods & kModShift, ctrl = e.mods & (kModCtrl | kModSuper);
        switch (e.type) {
        case EventType::PointerDown: {
            ui.setFocus(ec.node, FocusReason::Pointer);
            const std::size_t p = hit(ui, ec.node, s, e.pos);
            const std::size_t probe = p < ed.doc.size() ? p : (p ? p - 1 : 0);
            if ((ctrl || s.readOnly) && probe < ed.doc.size() && !ed.doc.formatOfByte(probe).link.empty()) {
                ui.emit(ec.node, SignalKind::Submitted, {.text = ed.doc.formatOfByte(probe).link});
                return true;
            }
            ed.setCursor(p, shift);
            s.dragging = true;
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::PointerMove:
            if (!s.dragging) return false;
            ed.setCursor(hit(ui, ec.node, s, e.pos), true);
            ui.invalidate(ec.node);
            return true;
        case EventType::PointerUp: s.dragging = false; return true;
        case EventType::Wheel:
            ensureLayout(ui, ec.node, s);
            s.scroll = std::clamp(s.scroll - e.wheel.y * 40.f, 0.f, std::max(0.f, s.lay.height - ui.contentRect(ec.node).h));
            ui.invalidate(ec.node);
            return true;
        case EventType::Text:
            if (s.readOnly || ctrl || e.text.empty()) return false;
            ed.insertText(e.text);
            break;
        case EventType::KeyDown: {
            const bool ro = s.readOnly;
            switch (e.key) {
            case Key::Left: ctrl ? ed.wordLeft(shift) : ed.moveLeft(shift); break;
            case Key::Right: ctrl ? ed.wordRight(shift) : ed.moveRight(shift); break;
            case Key::Home: ctrl ? ed.setCursor(0, shift) : ed.home(shift); break;
            case Key::End: ctrl ? ed.setCursor(ed.doc.size(), shift) : ed.end(shift); break;
            case Key::Up: case Key::Down: case Key::PageUp: case Key::PageDown: {
                ensureLayout(ui, ec.node, s);
                const auto m = measurer(ui);
                const Rect cr = s.lay.caretRect(ed.cursor, ed.doc, m);
                const f32 page = ui.contentRect(ec.node).h;
                const f32 dy = e.key == Key::Up ? -1.f : e.key == Key::Down ? cr.h + 1.f : e.key == Key::PageUp ? -page : page + cr.h;
                const f32 ty = cr.y + dy;
                ed.setCursor(ty < 0.f ? 0 : ty > s.lay.height ? ed.doc.size() : s.lay.hitTest({cr.x, ty}, ed.doc, m), shift);
                break;
            }
            case Key::Backspace: if (ro) return false; ctrl ? (ed.wordLeft(true), ed.backspace()) : ed.backspace(); break;
            case Key::Delete: if (ro) return false; ctrl ? (ed.wordRight(true), ed.del()) : ed.del(); break;
            case Key::Enter: if (ro) return false; ed.newParagraph(); break;
            case Key::Tab:
                if (ro || ctrl) return false;
                ed.indent(shift ? -1 : 1);
                break;
            case Key::A: if (!ctrl) return false; ed.selectAll(); break;
            case Key::C: if (!ctrl) return false; if (ed.hasSelection()) ui.setClipboardText(ed.selectedText()); return true;
            case Key::X:
                if (!ctrl || ro) return false;
                if (ed.hasSelection()) { ui.setClipboardText(ed.selectedText()); ed.backspace(); }
                break;
            case Key::V: {
                if (!ctrl || ro) return false;
                std::string clip;
                if (ui.clipboardText(clip)) ed.insertText(clip);
                break;
            }
            case Key::Z: if (!ctrl || ro) return false; shift ? ed.redo() : ed.undo(); break;
            case Key::Y: if (!ctrl || ro) return false; ed.redo(); break;
            case Key::B: if (!ctrl || ro) return false; ed.toggleBold(); break;
            case Key::I: if (!ctrl || ro) return false; ed.toggleItalic(); break;
            case Key::U: if (!ctrl || ro) return false; ed.toggleUnderline(); break;
            default: return false;
            }
            break;
        }
        default: return false;
        }
        scrollToCaret(ui, ec.node, s);
        ui.invalidate(ec.node);
        return true;
    }
};
void ensureRegistered(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("richtextedit") != 0xFFFFFFFFu) return;
    r.registerWidget<RichW>();
    ui.addStyleSheet("richtextedit { border-width: 1px; border-color: rgba(255,255,255,0.25); border-radius: 4px; padding: 6px; }\n"
                     "richtextedit:focus { border-color: #3D8BFD; }\n");
}
} // namespace

NodeHandle richTextEdit(UiContext& ui, NodeHandle parent, std::string_view id) {
    ensureRegistered(ui);
    auto h = ui.create("richtextedit", parent, id);
    if (!h) return {};
    const NodeHandle n = *h;
    RichState& s = richPool().create(ui, n);
    UiContext* up = &ui;
    s.ed.onChanged = [up, n] {
        RichState* st = richPool().get(*up, n);
        if (!st) return;
        st->dirty = true;
        up->setText(n, st->ed.doc.plainText());
        up->invalidate(n);
        up->emit(n, SignalKind::TextChanged, {.text = st->ed.doc.plainText()});
    };
    return n;
}
rich::RichTextEditor* richEditor(UiContext& ui, NodeHandle n) {
    RichState* s = richPool().get(ui, n);
    return s ? &s->ed : nullptr;
}
void richTextChanged(UiContext& ui, NodeHandle n) {
    if (RichState* s = richPool().get(ui, n)) {
        s->ed.cursor = std::min(s->ed.cursor, s->ed.doc.size());
        s->ed.anchor = std::min(s->ed.anchor, s->ed.doc.size());
        if (s->ed.onChanged) s->ed.onChanged();
    }
}
void setRichHtml(UiContext& ui, NodeHandle n, std::string_view html) {
    if (RichState* s = richPool().get(ui, n)) { s->ed.doc.fromHtml(html); s->ed.resetHistory(); s->ed.setCursor(0); richTextChanged(ui, n); }
}
std::string richHtml(UiContext& ui, NodeHandle n) {
    RichState* s = richPool().get(ui, n);
    return s ? s->ed.doc.toHtml() : std::string{};
}
void setRichMarkdown(UiContext& ui, NodeHandle n, std::string_view md) {
    if (RichState* s = richPool().get(ui, n)) { s->ed.doc.fromMarkdown(md); s->ed.resetHistory(); s->ed.setCursor(0); richTextChanged(ui, n); }
}
std::string richMarkdown(UiContext& ui, NodeHandle n) {
    RichState* s = richPool().get(ui, n);
    return s ? s->ed.doc.toMarkdown() : std::string{};
}
void setRichReadOnly(UiContext& ui, NodeHandle n, bool ro) {
    if (RichState* s = richPool().get(ui, n)) { s->readOnly = ro; ui.invalidate(n); }
}
} // namespace fuse::ui::widgets
