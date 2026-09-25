/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/util/config/config.cpp@0867d3c (DXVK origin, zlib; altered)

#include <fuse/relight/options/option_config.hpp>

#include "options_log.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fuse::relight::options {

namespace {

bool isWhitespace(char ch) { return ch == ' ' || ch == '\x9' || ch == '\r'; }

bool isValidKeyChar(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '.' ||
           ch == '_';
}

std::size_t skipWhitespace(std::string_view line, std::size_t n) {
    while (n < line.size() && isWhitespace(line[n])) {
        ++n;
    }
    return n;
}

bool keyPassesFilters(const std::string& key, const std::vector<std::string>& filters) {
    if (filters.empty()) {
        return true;
    }
    for (const std::string& filter : filters) {
        if (key.find(filter) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct ParseState {
    bool active = true;
    std::map<std::string, std::uint32_t, std::less<>> keyLines;
};

void addDiagnostic(std::vector<ConfigDiagnostic>* diagnostics, std::uint32_t line, std::string message) {
    if (diagnostics) {
        diagnostics->push_back({line, std::move(message)});
    }
}

void parseLine(OptionConfig& config, ParseState& state, std::string_view line, std::uint32_t lineNumber,
               const std::string& exeName, std::vector<ConfigDiagnostic>* diagnostics) {
    std::size_t n = skipWhitespace(line, 0);
    if (n >= line.size() || line[n] == '#') {
        return; // blank line or comment
    }

    if (line[n] == '[') {
        n += 1;
        std::size_t e = line.size() - 1;
        while (e > n && line[e] != ']') {
            e -= 1;
        }
        if (line[e] != ']') {
            addDiagnostic(diagnostics, lineNumber, "section header without ']' matches no executable");
        }
        const std::string_view section = e > n ? line.substr(n, e - n) : std::string_view();
        state.active = section == exeName;
        return;
    }

    const std::size_t keyStart = n;
    while (n < line.size() && isValidKeyChar(line[n])) {
        ++n;
    }
    const std::string_view key = line.substr(keyStart, n - keyStart);
    const std::size_t keyEnd = n;

    n = skipWhitespace(line, n);
    if (n >= line.size() || line[n] != '=') {
        if (keyStart == 0 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
            addDiagnostic(diagnostics, lineNumber, "line ignored: UTF-8 byte order mark before the key");
        } else if (n == keyEnd && n < line.size() && !key.empty()) {
            addDiagnostic(diagnostics, lineNumber,
                          "line ignored: invalid character '" + std::string(1, line[n]) + "' after key '" +
                              std::string(key) + "' (keys use A-Z a-z 0-9 . _)");
        } else if (key.empty()) {
            addDiagnostic(diagnostics, lineNumber, "line ignored: expected 'key = value'");
        } else {
            addDiagnostic(diagnostics, lineNumber, "line ignored: missing '=' after key '" + std::string(key) + "'");
        }
        return;
    }

    std::string value;
    bool insideString = false;
    n = skipWhitespace(line, n + 1);
    while (n < line.size()) {
        if (line[n] == '"') {
            insideString = !insideString;
            ++n;
        } else {
            value.push_back(line[n++]);
        }
    }
    if (insideString) {
        addDiagnostic(diagnostics, lineNumber, "unterminated quote in value of '" + std::string(key) + "'");
    }
    if (key.empty()) {
        addDiagnostic(diagnostics, lineNumber, "line ignored: empty key");
        return;
    }
    if (!state.active) {
        return;
    }
    auto previous = state.keyLines.find(key);
    if (previous != state.keyLines.end()) {
        addDiagnostic(diagnostics, lineNumber,
                      "'" + std::string(key) + "' set again; overrides line " + std::to_string(previous->second));
        previous->second = lineNumber;
    } else {
        state.keyLines.emplace(std::string(key), lineNumber);
    }
    config.set(std::string(key), std::move(value));
}

} // namespace

const std::vector<std::string>& defaultSaveKeyFilters() {
    static const std::vector<std::string> kFilters = {"rtx.", "relight."};
    return kFilters;
}

void OptionConfig::set(std::string key, std::string value) { m_entries.insert_or_assign(std::move(key), std::move(value)); }

void OptionConfig::erase(std::string_view key) {
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        m_entries.erase(it);
    }
}

const std::string* OptionConfig::find(std::string_view key) const {
    auto it = m_entries.find(key);
    if (it == m_entries.end() || it->second.empty()) {
        return nullptr;
    }
    return &it->second;
}

void OptionConfig::merge(const OptionConfig& other) {
    for (const auto& [key, value] : other.m_entries) {
        m_entries.insert_or_assign(key, value);
    }
}

OptionConfig OptionConfig::parse(std::string_view text, const ConfigParseOptions& options,
                                 std::vector<ConfigDiagnostic>* diagnostics) {
    OptionConfig config;
    ParseState state;
    const std::string exeName = options.exeName.empty() ? currentExecutableName() : options.exeName;

    std::uint32_t lineNumber = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        const bool last = end == std::string_view::npos;
        if (last) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1); // Windows text-mode streams drop the '\r' of "\r\n"
        }
        ++lineNumber;
        parseLine(config, state, line, lineNumber, exeName, diagnostics);
        if (last) {
            break;
        }
        start = end + 1;
    }
    return config;
}

OptionConfig OptionConfig::loadFile(const std::string& path, const ConfigParseOptions& options,
                                    std::vector<ConfigDiagnostic>* diagnostics, bool* found) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        detail::logInfo("No config file found at: %s", path.c_str());
        if (found) {
            *found = false;
        }
        return OptionConfig();
    }
    if (found) {
        *found = true;
    }
    detail::logInfo("Found config file: %s", path.c_str());

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();

    std::vector<ConfigDiagnostic> local;
    OptionConfig config = parse(text, options, &local);
    for (const ConfigDiagnostic& d : local) {
        detail::logWarn("%s:%u: %s", path.c_str(), static_cast<unsigned>(d.line), d.message.c_str());
    }
    if (diagnostics) {
        diagnostics->insert(diagnostics->end(), local.begin(), local.end());
    }
    return config;
}

std::string OptionConfig::serialize(const std::vector<std::string>& keyFilters) const {
    std::string out;
    for (const auto& [key, value] : m_entries) {
        if (value.empty() || !keyPassesFilters(key, keyFilters)) {
            continue;
        }
        out += key;
        out += " = ";
        if (isWhitespace(value.front())) {
            out += '"';
            out += value;
            out += '"';
        } else {
            out += value;
        }
        out += '\n';
    }
    return out;
}

bool OptionConfig::saveFile(const std::string& path, const std::vector<std::string>& keyFilters) const {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        detail::logWarn("Cannot write config file: %s", path.c_str());
        return false;
    }
    detail::logInfo("Serializing config file: %s", path.c_str());
    const std::string text = serialize(keyFilters);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

void OptionConfig::logEntries(const char* title) const {
    if (m_entries.empty()) {
        return;
    }
    detail::logInfo("%s configuration:", title);
    for (const auto& [key, value] : m_entries) {
        detail::logInfo("  %s = %s", key.c_str(), value.c_str());
    }
}

// ----------------------------------------------------------------------------
// Platform helpers
// ----------------------------------------------------------------------------

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}
std::string narrow(const wchar_t* text, std::size_t count) {
    if (count == 0) {
        return std::string();
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), out.data(), length, nullptr, nullptr);
    return out;
}
} // namespace

std::string getEnvironmentVariable(const char* name) {
    const std::wstring wname = widen(name);
    const DWORD length = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (length == 0) {
        return std::string();
    }
    std::wstring buffer(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(wname.c_str(), buffer.data(), length);
    return narrow(buffer.data(), written);
}

bool setEnvironmentVariable(const char* name, const std::string& value) {
    const std::wstring wname = widen(name);
    const std::wstring wvalue = widen(value);
    _wputenv_s(wname.c_str(), wvalue.c_str()); // keep the CRT copy in sync
    return SetEnvironmentVariableW(wname.c_str(), value.empty() ? nullptr : wvalue.c_str()) != 0 || value.empty();
}

std::string currentExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return narrow(buffer.data(), length);
}
#else
std::string getEnvironmentVariable(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

bool setEnvironmentVariable(const char* name, const std::string& value) {
    if (value.empty()) {
        return unsetenv(name) == 0;
    }
    return setenv(name, value.c_str(), 1) == 0;
}

std::string currentExecutablePath() {
#if defined(__linux__)
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) {
        return std::string(buffer, static_cast<std::size_t>(length));
    }
#endif
    return std::string();
}
#endif

std::string currentExecutableName() {
    const std::string path = currentExecutablePath();
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace fuse::relight::options
