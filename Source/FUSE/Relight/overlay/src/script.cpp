// FUSE Relight RL-6.1: scripted overlay input (see script.hpp).
#include <fuse/relight/overlay/script.hpp>

#include <fuse/relight/overlay/input.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace fuse::relight::overlay {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view nextToken(std::string_view& s) {
    s = trim(s);
    std::size_t n = 0;
    while (n < s.size() && !std::isspace(static_cast<unsigned char>(s[n]))) {
        ++n;
    }
    const std::string_view t = s.substr(0, n);
    s.remove_prefix(n);
    return t;
}

template <typename T>
bool number(std::string_view s, T& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool virtualKey(std::string_view s, std::int32_t& out) {
    struct Named {
        const char* name;
        std::int32_t vk;
    };
    static constexpr Named kNames[] = {{"back", kVkBack}, {"enter", kVkReturn}, {"esc", kVkEscape},
                                       {"up", kVkUp},     {"down", kVkDown},    {"pgup", kVkPrior},
                                       {"pgdn", kVkNext}, {"home", kVkHome}};
    for (const Named& n : kNames) {
        if (s == n.name) {
            out = n.vk;
            return true;
        }
    }
    return number(s, out);
}

} // namespace

bool Script::parse(std::string_view text) {
    m_commands.clear();
    m_error.clear();
    std::uint32_t lineNo = 0;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        ++lineNo;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        auto fail = [&](const char* why) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "line %u: ", lineNo);
            m_error = std::string(buf) + why;
            return false;
        };
        ScriptCommand c;
        c.line = lineNo;
        if (!number(nextToken(line), c.frame)) {
            return fail("expected a frame number");
        }
        const std::string_view cmd = nextToken(line);
        if (cmd == "toggle") {
            c.kind = ScriptCommand::Kind::Toggle;
        } else if (cmd == "click" || cmd == "move") {
            c.kind = cmd == "click" ? ScriptCommand::Kind::Click : ScriptCommand::Kind::Move;
            if (!number(nextToken(line), c.x) || !number(nextToken(line), c.y)) {
                return fail("expected X Y");
            }
        } else if (cmd == "clickw") {
            c.kind = ScriptCommand::Kind::ClickWidget;
            c.text = std::string(nextToken(line));
            if (c.text.empty()) {
                return fail("expected a widget id");
            }
        } else if (cmd == "type") {
            c.kind = ScriptCommand::Kind::Type;
            line = trim(line);
            c.text = std::string(line);
            line = {};
        } else if (cmd == "key") {
            c.kind = ScriptCommand::Kind::Key;
            if (!virtualKey(nextToken(line), c.value)) {
                return fail("expected a virtual key");
            }
        } else if (cmd == "wheel" || cmd == "game") {
            c.kind = cmd == "wheel" ? ScriptCommand::Kind::Wheel : ScriptCommand::Kind::Game;
            if (!number(nextToken(line), c.value)) {
                return fail("expected a count");
            }
        } else {
            return fail("unknown command");
        }
        if (!trim(line).empty()) {
            return fail("trailing text");
        }
        m_commands.push_back(std::move(c));
    }
    std::stable_sort(m_commands.begin(), m_commands.end(),
                     [](const ScriptCommand& a, const ScriptCommand& b) { return a.frame < b.frame; });
    return true;
}

bool Script::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        m_error = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);
    return parse(text);
}

void Script::range(std::uint64_t frame, std::size_t& begin, std::size_t& end) const {
    begin = static_cast<std::size_t>(
        std::lower_bound(m_commands.begin(), m_commands.end(), frame,
                         [](const ScriptCommand& c, std::uint64_t f) { return c.frame < f; }) -
        m_commands.begin());
    end = begin;
    while (end < m_commands.size() && m_commands[end].frame == frame) {
        ++end;
    }
}

void Script::messages(const ScriptCommand& c, std::int32_t cx, std::int32_t cy, std::vector<WindowMessage>& out) {
    using namespace win32;
    const std::int64_t pos = makeLParam(cx, cy);
    switch (c.kind) {
    case ScriptCommand::Kind::Toggle:
        out.push_back({kWmSysKeyDown, kVkX, kAltContextBit | 1});
        out.push_back({kWmSysChar, 'x', kAltContextBit | 1});
        out.push_back({kWmSysKeyUp, kVkX, kAltContextBit | kRepeatBit | (1ll << 31) | 1});
        break;
    case ScriptCommand::Kind::Click:
    case ScriptCommand::Kind::ClickWidget:
        out.push_back({kWmMouseMove, 0, pos});
        out.push_back({kWmLButtonDown, 1, pos});
        out.push_back({kWmLButtonUp, 0, pos});
        break;
    case ScriptCommand::Kind::Move:
        out.push_back({kWmMouseMove, 0, pos});
        break;
    case ScriptCommand::Kind::Type:
        for (char ch : c.text) {
            out.push_back({kWmChar, static_cast<std::uint64_t>(static_cast<unsigned char>(ch)), 1});
        }
        break;
    case ScriptCommand::Kind::Key:
        out.push_back({kWmKeyDown, static_cast<std::uint64_t>(c.value), 1});
        out.push_back({kWmKeyUp, static_cast<std::uint64_t>(c.value), kRepeatBit | (1ll << 31) | 1});
        break;
    case ScriptCommand::Kind::Wheel:
        out.push_back({kWmMouseWheel,
                       static_cast<std::uint64_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(c.value * 120)))
                           << 16,
                       pos});
        break;
    case ScriptCommand::Kind::Game:
        for (std::int32_t i = 0; i < c.value; ++i) {
            switch (i % 4) {
            case 0:
                out.push_back({kWmKeyDown, 'W', 1});
                break;
            case 1:
                out.push_back({kWmKeyUp, 'W', kRepeatBit | (1ll << 31) | 1});
                break;
            case 2:
                out.push_back({kWmMouseMove, 0, makeLParam(i % 64, i % 48)});
                break;
            default:
                out.push_back({kWmChar, 'w', 1});
                break;
            }
        }
        break;
    }
}

} // namespace fuse::relight::overlay
