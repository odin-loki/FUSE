#pragma once

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace fuse::locale {

/// Locale string table.
/// Lookup order: the active locale, then "en", then the key itself.
class StringTable {
public:
    void set(std::string locale, std::string key, std::string value) {
        m_entries[std::move(locale)][std::move(key)] = std::move(value);
    }

    void setLocale(std::string locale) { m_locale = std::move(locale); }

    /// String for the active locale. A missing key returns `key`.
    /// A locale with no entry falls back to "en", then to `key`.
    [[nodiscard]] std::string get(const std::string& key) const {
        if (const std::string* value = find(m_locale, key)) {
            return *value;
        }
        if (m_locale != "en") {
            if (const std::string* value = find("en", key)) {
                return *value;
            }
        }
        return key;
    }

    /// Replaces `{0}`, `{1}`, ... while scanning left to right.
    /// Placeholders past the end of `args` are left unchanged.
    /// A null or empty argument is replaced with an empty string.
    /// Missing keys and missing translations follow `get`. Does not throw.
    [[nodiscard]] std::string format(const std::string& key,
                                     std::initializer_list<const char*> args = {}) const {
        const std::string text = get(key);
        std::string out;
        out.reserve(text.size());

        std::size_t i = 0;
        while (i < text.size()) {
            if (text[i] != '{') {
                out.push_back(text[i]);
                ++i;
                continue;
            }

            const std::size_t start = i;
            std::size_t j = i + 1;
            if (j >= text.size() || text[j] < '0' || text[j] > '9') {
                out.push_back(text[i]);
                ++i;
                continue;
            }

            std::size_t index = 0;
            bool indexOk = true;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
                const std::size_t digit = static_cast<std::size_t>(text[j] - '0');
                if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10u) {
                    indexOk = false;
                    break;
                }
                index = index * 10u + digit;
                ++j;
            }

            if (!indexOk || j >= text.size() || text[j] != '}') {
                out.push_back(text[i]);
                ++i;
                continue;
            }

            if (index < args.size()) {
                const char* arg = args.begin()[index];
                if (arg != nullptr && arg[0] != '\0') {
                    out.append(arg);
                }
            } else {
                out.append(text, start, (j - start) + 1u);
            }
            i = j + 1;
        }

        return out;
    }

private:
    [[nodiscard]] const std::string* find(const std::string& locale, const std::string& key) const {
        const auto localeIt = m_entries.find(locale);
        if (localeIt == m_entries.end()) {
            return nullptr;
        }
        const auto keyIt = localeIt->second.find(key);
        if (keyIt == localeIt->second.end()) {
            return nullptr;
        }
        return &keyIt->second;
    }

    std::string m_locale = "en";
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_entries;
};

} // namespace fuse::locale
