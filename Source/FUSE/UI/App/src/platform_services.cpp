// fuse_app platform services that need no OS (services.hpp): the title-bar commands, saved UI / window state, the
// dialog-helper command lines and parsers, uri-list <-> paths, and the PNG codec of the image clipboard format.
#include "fuse/app/app.hpp"
#include "fuse/text/from_chars.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace fuse::app {
using namespace fuse::ui;

namespace {
bool parseNumber(std::string_view s, double& out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    if (s.empty()) return false;
    const auto r = fuse::text::fromChars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
std::string fixed2(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}
bool vec2Of(std::string_view s, Vec2& out) {
    const std::size_t c = s.find(',');
    double x = 0.0, y = 0.0;
    if (c == std::string_view::npos || !parseNumber(s.substr(0, c), x) || !parseNumber(s.substr(c + 1), y)) return false;
    out = {f32(x), f32(y)};
    return true;
}
constexpr std::string_view kModeNames[] = {"normal", "minimized", "maximized", "fullscreen"};
} // namespace

// ---- title-bar commands ----------------------------------------------------------------------------------------------
void AppWindow::command(WindowCommand c) {
    switch (c) {
    case WindowCommand::Minimize: minimize(); break;
    case WindowCommand::ToggleMaximize: setMode(m_mode == WindowMode::Maximized ? WindowMode::Normal : WindowMode::Maximized); break;
    case WindowCommand::Close: tryClose(); break;
    }
}

// ---- saved state -----------------------------------------------------------------------------------------------------
void AppWindow::saveUiState(Settings& out, std::string_view prefix) const {
    const std::string p(prefix);
    if (const std::string_view f = m_ui.idOf(m_ui.focused()); !f.empty()) out.set(p + "focus", f);
    std::vector<NodeHandle> stack{m_ui.root()};
    while (!stack.empty()) {
        const NodeHandle n = stack.back();
        stack.pop_back();
        for (NodeHandle c = m_ui.firstChild(n); m_ui.alive(c); c = m_ui.nextSibling(c)) stack.push_back(c);
        const std::string id(m_ui.idOf(n));
        if (id.empty()) continue;
        switch (m_ui.role(n)) {
        case A11yRole::TextField: out.set(p + "text." + id, m_ui.text(n)); break;
        case A11yRole::Tab:
        case A11yRole::Slider:
        case A11yRole::ComboBox:
        case A11yRole::SpinBox:
        case A11yRole::Dial: out.set(p + "value." + id, double(m_ui.value(n))); break;
        case A11yRole::CheckBox:
        case A11yRole::RadioButton:
        case A11yRole::Switch: out.set(p + "checked." + id, m_ui.checked(n) ? 1.0 : 0.0); break;
        default: break;
        }
        if (const Vec2 o = m_ui.scroll(n); o.x != 0.f || o.y != 0.f) out.set(p + "scroll." + id, fixed2(o.x) + "," + fixed2(o.y));
    }
}
void AppWindow::restoreUiState(const Settings& in, std::string_view prefix) {
    std::string focus;
    for (const auto& [key, v] : in.items()) {
        const std::string_view k(key);
        if (!k.starts_with(prefix)) continue;
        const std::string_view rest = k.substr(prefix.size());
        if (rest == "focus") { focus = v; continue; }
        const std::size_t dot = rest.find('.');
        if (dot == std::string_view::npos) continue;
        const std::string_view kind = rest.substr(0, dot);
        const NodeHandle n = m_ui.find(rest.substr(dot + 1));
        if (!m_ui.alive(n)) continue;
        double d = 0.0;
        Vec2 o;
        if (kind == "text" && m_ui.text(n) != v) {
            m_ui.setText(n, v);
            m_ui.editBuffer(n).setText(v);
            m_ui.editBuffer(n).end(false);
            m_ui.emit(n, SignalKind::TextChanged, {.text = v});
        } else if (kind == "value" && parseNumber(v, d)) {
            m_ui.setValue(n, f32(d), true);
        } else if (kind == "checked" && parseNumber(v, d) && m_ui.checked(n) != (d != 0.0)) {
            m_ui.setChecked(n, d != 0.0);
            m_ui.emit(n, SignalKind::Toggled, {.flag = d != 0.0});
        } else if (kind == "scroll" && vec2Of(v, o)) {
            m_pendingScroll.emplace_back(std::string(rest.substr(dot + 1)), o);
        }
    }
    if (const NodeHandle f = m_ui.find(focus); !focus.empty() && m_ui.alive(f)) m_ui.setFocus(f);
    m_forceRedraw = true;
}
void AppWindow::saveWindowState(Settings& out, std::string_view prefix) const {
    const std::string p(prefix);
    if (m_mode == WindowMode::Normal) { // else: keep the normal geometry saved before
        const Size s = size();
        out.set(p + "x", double(m_position.x));
        out.set(p + "y", double(m_position.y));
        out.set(p + "w", double(s.w));
        out.set(p + "h", double(s.h));
    }
    out.set(p + "mode", kModeNames[int(m_mode)]);
}
bool AppWindow::restoreWindowState(const Settings& in, std::string_view prefix) {
    const std::string p(prefix);
    const double w = in.number(p + "w", 0.0), h = in.number(p + "h", 0.0);
    if (!(w >= 1.0) || !(h >= 1.0)) return false;
    const Size want = clampSize({f32(w), f32(h)});
    if (const Size now = size(); now.w != want.w || now.h != want.h) resize(u32(std::lround(want.w)), u32(std::lround(want.h)));
    if (!in.get(p + "x").empty() && !in.get(p + "y").empty()) setPosition({f32(in.number(p + "x")), f32(in.number(p + "y"))});
    const std::string_view mode = in.get(p + "mode", "normal");
    if (mode == "maximized") setMode(WindowMode::Maximized);
    else if (mode == "fullscreen") setMode(WindowMode::Fullscreen);
    else if (m_wantMode != WindowMode::Normal) setMode(WindowMode::Normal);
    return true;
}

std::string Application::saveState() {
    Settings st;
    for (std::size_t i = 0; i < m_windows.size(); ++i) {
        const std::string p = "w" + std::to_string(i) + ".";
        m_windows[i]->saveUiState(st, p);
        m_windows[i]->saveWindowState(st, p + "window.");
    }
    if (m_stateHooks.save) m_stateHooks.save(m_stateHooks.user, *this, st);
    return st.toText();
}
void Application::restoreState(std::string_view bytes) {
    m_restored.fromText(bytes);
    m_restoredSeen = true;
    for (std::size_t i = 0; i < m_windows.size(); ++i) {
        const std::string p = "w" + std::to_string(i) + ".";
        m_windows[i]->restoreWindowState(m_restored, p + "window.");
        m_windows[i]->restoreUiState(m_restored, p);
    }
    if (m_stateHooks.restore) m_stateHooks.restore(m_stateHooks.user, *this, m_restored);
}
bool Application::saveStateTo(const std::string& path) {
    const std::string text = saveState();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f.good()) return false;
    }
    std::remove(path.c_str()); // rename does not replace on Windows
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
bool Application::restoreStateFrom(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    restoreState(ss.str());
    return true;
}

// ---- dialog helpers ----------------------------------------------------------------------------------------------------
const char* platformErrorName(PlatformError e) {
    switch (e) {
    case PlatformError::Cancelled: return "cancelled";
    case PlatformError::Unavailable: return "unavailable";
    case PlatformError::InvalidArgument: return "invalid argument";
    case PlatformError::Failed: return "failed";
    }
    return "?";
}
namespace {
std::string startOf(const FileDialogDesc& d, DialogKind kind) {
    std::string p(d.folder);
    if (!p.empty() && p.back() != '/') p += '/';
    if (kind == DialogKind::Save) p += std::string(d.fileName);
    return p;
}
std::vector<std::string> patternsOf(std::string_view list) { // "*.png;*.jpg" -> {"*.png", "*.jpg"}
    std::vector<std::string> out;
    while (!list.empty()) {
        const std::size_t c = list.find_first_of(";, ");
        if (c != 0) out.emplace_back(list.substr(0, c));
        list = c == std::string_view::npos ? std::string_view{} : list.substr(c + 1);
    }
    return out;
}
int byteOf(f32 v) { return int(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }
std::string hexColor(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byteOf(c.r), byteOf(c.g), byteOf(c.b));
    return buf;
}
int hexDigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
} // namespace

std::vector<std::string> zenityFileArgs(const FileDialogDesc& d, DialogKind kind) {
    std::vector<std::string> a{"zenity", "--file-selection"};
    if (!d.title.empty()) a.push_back("--title=" + std::string(d.title));
    if (kind == DialogKind::Save) { a.emplace_back("--save"); a.emplace_back("--confirm-overwrite"); }
    if (kind == DialogKind::Folder) a.emplace_back("--directory");
    if (kind == DialogKind::Open && d.multiple) { a.emplace_back("--multiple"); a.emplace_back("--separator=|"); }
    if (const std::string p = startOf(d, kind); !p.empty()) a.push_back("--filename=" + p);
    if (kind != DialogKind::Folder)
        for (const FileFilter& f : d.filters) {
            std::string s = "--file-filter=" + std::string(f.name.empty() ? f.patterns : f.name) + " |";
            for (const std::string& p : patternsOf(f.patterns)) s += " " + p;
            a.push_back(std::move(s));
        }
    return a;
}
std::vector<std::string> kdialogFileArgs(const FileDialogDesc& d, DialogKind kind) {
    std::vector<std::string> a{"kdialog"};
    if (!d.title.empty()) { a.emplace_back("--title"); a.emplace_back(d.title); }
    if (kind == DialogKind::Open && d.multiple) { a.emplace_back("--multiple"); a.emplace_back("--separate-output"); }
    a.emplace_back(kind == DialogKind::Folder ? "--getexistingdirectory" : kind == DialogKind::Save ? "--getsavefilename" : "--getopenfilename");
    const std::string p = startOf(d, kind);
    a.push_back(p.empty() ? std::string(".") : p);
    if (kind != DialogKind::Folder && !d.filters.empty()) {
        std::string all; // "Images (*.png *.jpg)\nText (*.txt)"
        for (const FileFilter& f : d.filters) {
            std::string pats;
            for (const std::string& q : patternsOf(f.patterns)) pats += (pats.empty() ? "" : " ") + q;
            all += (all.empty() ? "" : "\n") + (f.name.empty() ? pats : std::string(f.name) + " (" + pats + ")");
        }
        a.push_back(std::move(all));
    }
    return a;
}
std::vector<std::string> zenityColorArgs(const ColorDialogDesc& d) {
    std::vector<std::string> a{"zenity", "--color-selection", "--color=" + hexColor(d.initial)};
    if (!d.title.empty()) a.push_back("--title=" + std::string(d.title));
    return a;
}
std::vector<std::string> kdialogColorArgs(const ColorDialogDesc& d) {
    std::vector<std::string> a{"kdialog"};
    if (!d.title.empty()) { a.emplace_back("--title"); a.emplace_back(d.title); }
    a.emplace_back("--getcolor");
    a.emplace_back("--default");
    a.push_back(hexColor(d.initial));
    return a;
}
std::vector<std::string> zenityMessageArgs(std::string_view title, std::string_view text, MessageIcon icon, MessageButtons buttons) {
    std::vector<std::string> a{"zenity"};
    // --question has two buttons; three (Yes / No / Cancel) take an extra one, reported on stdout
    if (buttons == MessageButtons::Ok) a.emplace_back(icon == MessageIcon::Error ? "--error" : icon == MessageIcon::Warning ? "--warning" : "--info");
    else a.emplace_back("--question");
    a.push_back("--title=" + std::string(title));
    a.push_back("--text=" + std::string(text));
    a.emplace_back("--no-markup");
    if (buttons == MessageButtons::OkCancel) { a.emplace_back("--ok-label=OK"); a.emplace_back("--cancel-label=Cancel"); }
    if (buttons == MessageButtons::YesNoCancel) a.emplace_back("--extra-button=Cancel");
    return a;
}
bool parseDialogColor(std::string_view text, f32 fallbackAlpha, Color& out) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    if (text.starts_with("rgb")) { // rgb(r,g,b) / rgba(r,g,b,a)
        const std::size_t open = text.find('('), close = text.find(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open) return false;
        std::string_view body = text.substr(open + 1, close - open - 1);
        double v[4] = {0.0, 0.0, 0.0, double(fallbackAlpha)};
        int n = 0;
        while (!body.empty() && n < 4) {
            const std::size_t c = body.find(',');
            if (!parseNumber(body.substr(0, c), v[n])) return false;
            ++n;
            body = c == std::string_view::npos ? std::string_view{} : body.substr(c + 1);
        }
        if (n < 3) return false;
        out = {f32(v[0] / 255.0), f32(v[1] / 255.0), f32(v[2] / 255.0), f32(v[3])};
        return true;
    }
    if (text.starts_with("#") && (text.size() == 7 || text.size() == 13)) { // #rrggbb / #rrrrggggbbbb
        const std::size_t per = (text.size() - 1) / 3;
        f32 ch[3];
        for (int i = 0; i < 3; ++i) {
            const int hi = hexDigit(text[1 + std::size_t(i) * per]), lo = hexDigit(text[2 + std::size_t(i) * per]);
            if (hi < 0 || lo < 0) return false;
            ch[i] = f32(hi * 16 + lo) / 255.f;
        }
        out = {ch[0], ch[1], ch[2], fallbackAlpha};
        return true;
    }
    return false;
}
std::vector<std::string> splitDialogPaths(std::string_view output) {
    std::vector<std::string> out;
    while (!output.empty()) {
        const std::size_t c = output.find_first_of("|\n");
        std::string_view one = output.substr(0, c);
        while (!one.empty() && one.back() == '\r') one.remove_suffix(1);
        if (!one.empty()) out.emplace_back(one);
        output = c == std::string_view::npos ? std::string_view{} : output.substr(c + 1);
    }
    return out;
}
bool isOpenableUrl(std::string_view url) {
    for (std::string_view scheme : {"http://", "https://", "mailto:", "file:"})
        if (url.size() > scheme.size() && url.starts_with(scheme)) return true;
    return false;
}
std::string pathsFromUriList(std::string_view uriList) {
    std::string out;
    while (!uriList.empty()) {
        const std::size_t nl = uriList.find('\n');
        std::string_view line = uriList.substr(0, nl);
        uriList = nl == std::string_view::npos ? std::string_view{} : uriList.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\0')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#' || !line.starts_with("file:")) continue;
        line.remove_prefix(5);
        if (line.starts_with("//")) { // file://host/path: drop the authority ("" or "localhost")
            const std::size_t slash = line.find('/', 2);
            line = slash == std::string_view::npos ? std::string_view{} : line.substr(slash);
        }
        std::string path;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '%' && i + 2 < line.size() && hexDigit(line[i + 1]) >= 0 && hexDigit(line[i + 2]) >= 0) {
                path += char(hexDigit(line[i + 1]) * 16 + hexDigit(line[i + 2]));
                i += 2;
            } else {
                path += line[i];
            }
        }
#if defined(_WIN32)
        if (path.size() > 2 && path[0] == '/' && path[2] == ':') path.erase(0, 1); // "/C:/dir" -> "C:/dir"
#endif
        if (path.empty()) continue;
        if (!out.empty()) out += '\n';
        out += path;
    }
    return out;
}
std::string uriListFromPaths(std::string_view paths) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    while (!paths.empty()) {
        const std::size_t nl = paths.find('\n');
        const std::string_view path = paths.substr(0, nl);
        paths = nl == std::string_view::npos ? std::string_view{} : paths.substr(nl + 1);
        if (path.empty()) continue;
        out += path.front() == '/' ? "file://" : "file:///";
        for (const char ch : path) {
            const unsigned char c = static_cast<unsigned char>(ch);
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                               c == '_' || c == '.' || c == '~' || c == ':';
            if (plain) out += ch;
            else if (ch == '\\') out += '/';
            else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 15]; }
        }
        out += "\r\n";
    }
    return out;
}

// ---- XSETTINGS -----------------------------------------------------------------------------------------------------------
std::vector<XSetting> parseXSettings(std::span<const u8> bytes) {
    std::vector<XSetting> out;
    const u8* p = bytes.data();
    const std::size_t n = bytes.size();
    if (n < 12) return out;
    const bool msb = p[0] == 1;
    auto rd16 = [&](std::size_t at) { return msb ? u32(p[at]) << 8 | p[at + 1] : u32(p[at]) | u32(p[at + 1]) << 8; };
    auto rd32 = [&](std::size_t at) {
        return msb ? u32(p[at]) << 24 | u32(p[at + 1]) << 16 | u32(p[at + 2]) << 8 | p[at + 3]
                   : u32(p[at]) | u32(p[at + 1]) << 8 | u32(p[at + 2]) << 16 | u32(p[at + 3]) << 24;
    };
    auto pad4 = [](std::size_t v) { return (v + 3) & ~std::size_t(3); };
    const u32 count = rd32(8);
    std::size_t at = 12;
    for (u32 i = 0; i < count && at + 4 <= n; ++i) {
        XSetting s;
        s.type = p[at];
        const std::size_t nameLen = rd16(at + 2);
        at += 4;
        if (at + pad4(nameLen) + 4 > n) break;
        s.name.assign(reinterpret_cast<const char*>(p + at), nameLen);
        at += pad4(nameLen) + 4; // the name, then the last-change serial
        if (s.type == 0) {
            if (at + 4 > n) break;
            s.integer = int(rd32(at));
            at += 4;
        } else if (s.type == 1) {
            if (at + 4 > n) break;
            const std::size_t len = rd32(at);
            at += 4;
            if (len > n || at + pad4(len) > n) break;
            s.string.assign(reinterpret_cast<const char*>(p + at), len);
            at += pad4(len);
        } else if (s.type == 2) {
            if (at + 8 > n) break;
            at += 8;
        } else {
            break;
        }
        out.push_back(std::move(s));
    }
    return out;
}
std::vector<u8> buildXSettings(const std::vector<XSetting>& settings, u32 serial) {
    std::vector<u8> b{0, 0, 0, 0}; // byte order LSB first + padding
    auto put32 = [&](u32 v) {
        for (int k = 0; k < 4; ++k) b.push_back(u8(v >> (8 * k)));
    };
    auto pad = [&] {
        while (b.size() % 4) b.push_back(0);
    };
    put32(serial);
    put32(u32(settings.size()));
    for (const XSetting& s : settings) {
        b.push_back(u8(s.type));
        b.push_back(0);
        b.push_back(u8(s.name.size() & 0xFF));
        b.push_back(u8(s.name.size() >> 8));
        b.insert(b.end(), s.name.begin(), s.name.end());
        pad();
        put32(serial);
        if (s.type == 0) {
            put32(u32(s.integer));
        } else if (s.type == 1) {
            put32(u32(s.string.size()));
            b.insert(b.end(), s.string.begin(), s.string.end());
            pad();
        } else {
            for (int k = 0; k < 8; ++k) b.push_back(0); // a colour: four CARD16
        }
    }
    return b;
}

// ---- PNG -----------------------------------------------------------------------------------------------------------------
bool encodePng(const Image& image, std::vector<u8>& out) {
    out.clear();
    if (image.width <= 0 || image.height <= 0 || image.pixels.size() != std::size_t(image.width) * std::size_t(image.height)) return false;
    const auto sink = [](void* ctx, void* data, int n) {
        auto& v = *static_cast<std::vector<u8>*>(ctx);
        v.insert(v.end(), static_cast<const u8*>(data), static_cast<const u8*>(data) + n);
    };
    return stbi_write_png_to_func(sink, &out, image.width, image.height, 4, image.pixels.data(), image.width * 4) != 0 && !out.empty();
}
bool decodePng(std::span<const u8> png, Image& out) {
    int w = 0, h = 0, n = 0;
    if (png.empty() || png.size() > 0x7FFFFFFFu) return false;
    stbi_uc* p = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
    if (!p) return false;
    out.resize(w, h);
    std::memcpy(out.pixels.data(), p, std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(p);
    return true;
}

} // namespace fuse::app
