// Internationalisation (Qt QTranslator / QLocale parity for the GUI): gettext .po catalogs with contexts and plural
// forms, %1..%9 argument substitution, CLDR plural rules for common languages, layout direction per language and
// locale-aware number formatting. Bidi shaping itself lives in fuse_text.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fuse::ui::i18n {

enum class LayoutDirection : std::uint8_t { LeftToRight, RightToLeft };

/// "ar", "he_IL", "fa-IR", ... -> RightToLeft.
LayoutDirection directionOf(std::string_view language);
/// Plural form index for `n` in `language` (CLDR integer rules; matches gettext Plural-Forms for these languages).
int pluralIndex(std::string_view language, long long n);
/// Number of plural forms for `language`.
int pluralCount(std::string_view language);

struct Locale {
    std::string name = "en_US";
    char32_t decimal = U'.', group = U',';
    int groupSize = 3;
    LayoutDirection direction = LayoutDirection::LeftToRight;
    static Locale fromName(std::string_view name);
    /// Locale from LC_ALL / LC_MESSAGES / LANG (via the OS seam).
    static Locale system();
    std::string formatInteger(long long v) const;
    std::string formatNumber(double v, int decimals) const;
    std::string language() const { return name.substr(0, name.find_first_of("_-.@")); }
};

/// Replaces %1..%9 with args (Qt QString::arg style; "%%" is a literal '%').
std::string format(std::string_view pattern, std::initializer_list<std::string_view> args);

class Translator {
public:
    /// Parses a gettext .po file (msgctxt, msgid, msgid_plural, msgstr, msgstr[n], "Language:" header, fuzzy entries
    /// skipped). Returns false on malformed input. Can be called repeatedly to merge catalogs.
    bool loadPo(std::string_view po);
    bool loadPoFile(std::string_view path);
    void insert(std::string_view context, std::string_view source, std::string_view translation);
    void insertPlural(std::string_view context, std::string_view source, std::vector<std::string> forms);
    /// Translation of `source` in `context`, or `source` itself.
    std::string_view tr(std::string_view context, std::string_view source) const;
    /// Plural translation; `%n` in the result is replaced by n.
    std::string trn(std::string_view context, std::string_view singular, std::string_view plural, long long n) const;
    const std::string& language() const { return m_lang; }
    void setLanguage(std::string lang) { m_lang = std::move(lang); }
    std::size_t size() const { return m_map.size(); }

private:
    static std::string key(std::string_view ctx, std::string_view src);
    std::unordered_map<std::string, std::vector<std::string>> m_map;
    std::string m_lang = "en";
};

/// Process-wide installed translators (searched newest first), like QCoreApplication::installTranslator.
void installTranslator(const Translator* t);
void removeTranslator(const Translator* t);
std::string_view tr(std::string_view context, std::string_view source);
std::string trn(std::string_view context, std::string_view singular, std::string_view plural, long long n);
/// Direction of the newest installed translator's language (LeftToRight when none).
LayoutDirection applicationDirection();

} // namespace fuse::ui::i18n
