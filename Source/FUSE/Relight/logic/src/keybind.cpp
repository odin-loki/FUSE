/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/util/util_keybind.h@0867d3c (key name table)
#include <fuse/relight/logic/keybind.hpp>

#include <cctype>
#include <map>
#include <utility>

namespace fuse::relight::logic {

namespace {

struct KeyName {
    const char* name;
    std::uint32_t vk;
};

// Windows VK codes (winuser.h values); the first name of a code is its canonical name.
constexpr KeyName kKeys[] = {
    {"LBUTTON", 0x01},  {"RBUTTON", 0x02},    {"CANCEL", 0x03},     {"MBUTTON", 0x04},    {"XBUTTON1", 0x05},
    {"XBUTTON2", 0x06}, {"BACK", 0x08},       {"TAB", 0x09},        {"CLEAR", 0x0C},      {"ENTER", 0x0D},
    {"RETURN", 0x0D},   {"SHFT", 0x10},       {"SHIFT", 0x10},      {"CTRL", 0x11},       {"CONTROL", 0x11},
    {"ALT", 0x12},      {"MENU", 0x12},       {"PAUSE", 0x13},      {"CAPITAL", 0x14},    {"KANA", 0x15},
    {"IME_ON", 0x16},   {"JUNJA", 0x17},      {"FINAL", 0x18},      {"HANJA", 0x19},      {"IME_OFF", 0x1A},
    {"ESCAPE", 0x1B},   {"CONVERT", 0x1C},    {"NONCONVERT", 0x1D}, {"ACCEPT", 0x1E},     {"MODECHANGE", 0x1F},
    {"SPACE", 0x20},    {"PRIOR", 0x21},      {"NEXT", 0x22},       {"END", 0x23},        {"HOME", 0x24},
    {"LEFT", 0x25},     {"UP", 0x26},         {"RIGHT", 0x27},      {"DOWN", 0x28},       {"SELECT", 0x29},
    {"PRINT", 0x2A},    {"EXECUTE", 0x2B},    {"SNAPSHOT", 0x2C},   {"INSERT", 0x2D},     {"DELETE", 0x2E},
    {"HELP", 0x2F},     {"LWIN", 0x5B},       {"RWIN", 0x5C},       {"APPS", 0x5D},       {"SLEEP", 0x5F},
    {"NUMPAD0", 0x60},  {"NUMPAD1", 0x61},    {"NUMPAD2", 0x62},    {"NUMPAD3", 0x63},    {"NUMPAD4", 0x64},
    {"NUMPAD5", 0x65},  {"NUMPAD6", 0x66},    {"NUMPAD7", 0x67},    {"NUMPAD8", 0x68},    {"NUMPAD9", 0x69},
    {"MULTIPLY", 0x6A}, {"ADD", 0x6B},        {"SEPARATOR", 0x6C},  {"SUBTRACT", 0x6D},   {"DECIMAL", 0x6E},
    {"DIVIDE", 0x6F},   {"F1", 0x70},         {"F2", 0x71},         {"F3", 0x72},         {"F4", 0x73},
    {"F5", 0x74},       {"F6", 0x75},         {"F7", 0x76},         {"F8", 0x77},         {"F9", 0x78},
    {"F10", 0x79},      {"F11", 0x7A},        {"F12", 0x7B},        {"F13", 0x7C},        {"F14", 0x7D},
    {"F15", 0x7E},      {"F16", 0x7F},        {"F17", 0x80},        {"F18", 0x81},        {"F19", 0x82},
    {"F20", 0x83},      {"F21", 0x84},        {"F22", 0x85},        {"F23", 0x86},        {"F24", 0x87},
    {"NUMLOCK", 0x90},  {"SCROLL", 0x91},     {"LSHIFT", 0xA0},     {"RSHIFT", 0xA1},     {"LCONTROL", 0xA2},
    {"RCONTROL", 0xA3}, {"LMENU", 0xA4},      {"RMENU", 0xA5},      {"BROWSER_BACK", 0xA6}, {"BROWSER_FORWARD", 0xA7},
    {"BROWSER_REFRESH", 0xA8}, {"BROWSER_STOP", 0xA9}, {"BROWSER_SEARCH", 0xAA}, {"BROWSER_FAVORITES", 0xAB},
    {"BROWSER_HOME", 0xAC}, {"VOLUME_MUTE", 0xAD}, {"VOLUME_DOWN", 0xAE}, {"VOLUME_UP", 0xAF},
    {"MEDIA_NEXT_TRACK", 0xB0}, {"MEDIA_PREV_TRACK", 0xB1}, {"MEDIA_STOP", 0xB2}, {"MEDIA_PLAY_PAUSE", 0xB3},
    {"LAUNCH_MAIL", 0xB4}, {"LAUNCH_MEDIA_SELECT", 0xB5}, {"LAUNCH_APP1", 0xB6}, {"LAUNCH_APP2", 0xB7},
    {"OEM_1", 0xBA},    {"OEM_PLUS", 0xBB},   {"OEM_COMMA", 0xBC},  {"OEM_MINUS", 0xBD},  {"OEM_PERIOD", 0xBE},
    {"OEM_2", 0xBF},    {"OEM_3", 0xC0},      {"OEM_4", 0xDB},      {"OEM_5", 0xDC},      {"OEM_6", 0xDD},
    {"OEM_7", 0xDE},    {"OEM_8", 0xDF},      {"OEM_102", 0xE2},    {"PROCESSKEY", 0xE5}, {"PACKET", 0xE7},
    {"ATTN", 0xF6},     {"CRSEL", 0xF7},      {"EXSEL", 0xF8},      {"EREOF", 0xF9},      {"PLAY", 0xFA},
    {"ZOOM", 0xFB},     {"NONAME", 0xFC},     {"PA1", 0xFD},        {"OEM_CLEAR", 0xFE},
};

const std::map<std::string, std::uint32_t>& nameTable() {
    static const std::map<std::string, std::uint32_t> table = [] {
        std::map<std::string, std::uint32_t> t;
        for (const KeyName& k : kKeys) {
            t.emplace(k.name, k.vk);
        }
        for (char c = '0'; c <= '9'; ++c) {
            t.emplace(std::string(1, c), static_cast<std::uint32_t>(c));
        }
        for (char c = 'A'; c <= 'Z'; ++c) {
            t.emplace(std::string(1, c), static_cast<std::uint32_t>(c));
        }
        return t;
    }();
    return table;
}

std::string trimUpper(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    std::string out(s.substr(b, e - b));
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

bool parseVirtualKeys(std::string_view text, std::vector<std::uint32_t>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t comma = text.find(',', pos);
        const std::string name = trimUpper(text.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        if (name.empty()) {
            out.clear();
            return false;
        }
        const auto it = nameTable().find(name);
        if (it == nameTable().end()) {
            out.clear();
            return false;
        }
        out.push_back(it->second);
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return !out.empty();
}

std::string virtualKeyName(std::uint32_t vk) {
    for (const KeyName& k : kKeys) {
        if (k.vk == vk) {
            return k.name;
        }
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return std::string(1, static_cast<char>(vk));
    }
    return {};
}

} // namespace fuse::relight::logic
