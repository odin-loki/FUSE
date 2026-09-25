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
// Ported from dxvk-remix src/util/config/config.h@0867d3c and config.cpp@0867d3c (the Config
// key/value set and the .conf reader/writer). That code derives from DXVK, zlib licence,
// Copyright (c) 2017-2021 Philip Rebohle, Copyright (c) 2019-2021 Joshua Ashton. This is an
// altered version.
//
// .conf syntax (rtx.conf, user.conf, dxvk.conf), as read by Remix:
//
//   # comment                     any line that does not have the form below is ignored
//   rtx.someOption = value        key: [A-Za-z0-9._]+, whitespace around '=' is skipped
//   rtx.name = "two words"        '"' characters toggle a quoted run and are dropped
//   [Game.exe]                    following lines apply only when the process is Game.exe
//
// The value runs to the end of the line, including trailing spaces (Remix does not trim them, so
// "True " is not a bool). A trailing '\r' is dropped, as Windows text-mode streams do. A key that
// appears twice keeps its last value. An empty value counts as "not set".
//
// Writing produces one `key = value` line per entry, sorted by key, '\n' line ends, so a canonical
// file reads and writes back byte for byte. Upstream writes in hash-map order; sorting is the only
// difference in the bytes. Values that would not read back unchanged are written in a form that
// does: an empty value is skipped (it reads back as "not set"), and a value with leading
// whitespace is written inside quotes.
#pragma once

#include <fuse/relight/options/option_value.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::options {

/// A problem found while reading a .conf file. Upstream ignores these lines silently; Relight
/// reports them (and logs them as warnings when loading files).
struct ConfigDiagnostic {
    std::uint32_t line = 0; ///< 1-based
    std::string message;
};

struct ConfigParseOptions {
    /// Executable file name matched against `[section]` headers. Empty: the current process's name
    /// (see currentExecutableName()).
    std::string exeName;
};

/// Keys a save or export writes: every key that contains one of these substrings. Remix filters on
/// "rtx."; Relight adds "relight." for FUSE-only options.
const std::vector<std::string>& defaultSaveKeyFilters();

/// Configuration key/value set (Remix `Config`). Ordered by key.
class OptionConfig {
public:
    using Map = std::map<std::string, std::string, std::less<>>;

    OptionConfig() = default;
    explicit OptionConfig(Map entries) : m_entries(std::move(entries)) {}

    /// Set a raw string value (replaces an existing one).
    void set(std::string key, std::string value);
    /// Set a typed value, formatted as Remix formats it.
    template <typename T>
    void setValue(std::string key, const T& value) {
        set(std::move(key), formatOptionValue(value));
    }
    void erase(std::string_view key);

    /// True when `key` has a non-empty value (Remix Config::findOption).
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    /// The value of `key`, or nullptr when it is missing or empty.
    const std::string* find(std::string_view key) const;

    /// Remix Config::getOption: parse the value into a copy of `fallback` (what a failed parse leaves
    /// is kept, see option_value.hpp). A non-empty environment variable `envVarName` is parsed on top.
    template <typename T>
    T get(std::string_view key, T fallback = T{}, const char* envVarName = nullptr) const;

    /// Copy every entry of `other` over this set (`other` wins).
    void merge(const OptionConfig& other);

    bool empty() const { return m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }
    const Map& entries() const { return m_entries; }

    /// Parse .conf text. `diagnostics` receives malformed lines.
    static OptionConfig parse(std::string_view text, const ConfigParseOptions& options = {},
                              std::vector<ConfigDiagnostic>* diagnostics = nullptr);
    /// Read a .conf file. A missing file gives an empty set (and `*found = false`). Diagnostics are
    /// logged as warnings and also returned when `diagnostics` is given.
    static OptionConfig loadFile(const std::string& path, const ConfigParseOptions& options = {},
                                 std::vector<ConfigDiagnostic>* diagnostics = nullptr, bool* found = nullptr);

    /// Text of the entries whose key contains one of `keyFilters` (all entries when empty).
    std::string serialize(const std::vector<std::string>& keyFilters = {}) const;
    /// Write serialize(keyFilters) to `path`. Returns false when the file cannot be written.
    bool saveFile(const std::string& path, const std::vector<std::string>& keyFilters = {}) const;

    /// Log every entry at info level under `title`.
    void logEntries(const char* title) const;

private:
    Map m_entries;
};

/// Read the environment variable `name` ("" when unset).
std::string getEnvironmentVariable(const char* name);
/// Set (or with an empty value, clear) an environment variable of this process.
bool setEnvironmentVariable(const char* name, const std::string& value);
/// File name of the running executable (e.g. "Game.exe"), "" when unknown.
std::string currentExecutableName();
/// Full path of the running executable, "" when unknown.
std::string currentExecutablePath();

template <typename T>
T OptionConfig::get(std::string_view key, T fallback, const char* envVarName) const {
    T result = fallback;
    if (const std::string* value = find(key)) {
        parseOptionValue(*value, result);
    }
    if (envVarName) {
        const std::string envValue = getEnvironmentVariable(envVarName);
        if (!envValue.empty()) {
            parseOptionValue(envValue, result);
        }
    }
    return result;
}

} // namespace fuse::relight::options
