// Style sheets by handle (replace / remove), the UiContext theme slot and theme token colours. Kept apart from
// style.cpp / ui.cpp: everything here goes through StyleEngine::addSheet / clearSheets, so the cascade order rules
// live in one place.
#include "fuse/ui/ui.hpp"

#include <utility>
#include <vector>

namespace fuse::ui {

namespace {
std::string_view trimSpaces(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
} // namespace

u32 StyleEngine::addSheetTracked(StyleSheet sheet) {
    if (++m_nextSheetHandle == 0) ++m_nextSheetHandle;
    sheet.m_handle = m_nextSheetHandle;
    addSheet(std::move(sheet));
    return m_nextSheetHandle;
}

bool StyleEngine::hasSheet(u32 handle) const {
    if (handle == 0) return false;
    for (const StyleSheet& s : m_sheets)
        if (s.m_handle == handle) return true;
    return false;
}

bool StyleEngine::replaceSheet(u32 handle, StyleSheet sheet) {
    if (!hasSheet(handle)) return false;
    // re-add every sheet in order through addSheet, so rule ordering has one implementation
    std::vector<StyleSheet> all = std::move(m_sheets);
    clearSheets();
    for (StyleSheet& s : all) {
        if (s.m_handle == handle) {
            sheet.m_handle = handle;
            addSheet(std::move(sheet));
        } else {
            addSheet(std::move(s));
        }
    }
    return true;
}

bool StyleEngine::removeSheet(u32 handle) {
    if (!hasSheet(handle)) return false;
    std::vector<StyleSheet> all = std::move(m_sheets);
    clearSheets();
    for (StyleSheet& s : all)
        if (s.m_handle != handle) addSheet(std::move(s));
    return true;
}

bool StyleEngine::tokenColorCached(u32 nameHash) const {
    if (m_tokenColorVersion != m_version) return false;
    const std::size_t mask = m_tokenColors.size() - 1;
    std::size_t slot = std::size_t((nameHash * 2654435761u) >> 16) & mask;
    for (std::size_t probe = 0; probe < 8; ++probe, slot = (slot + 1) & mask) {
        if (m_tokenColors[slot].state == 0) return false;
        if (m_tokenColors[slot].hash == nameHash) return true;
    }
    return false;
}
bool StyleEngine::tokenColor(u32 nameHash, Color& out) const {
    if (m_tokenColorVersion != m_version) {
        m_tokenColors.fill({});
        m_tokenColorVersion = m_version;
    }
    const std::size_t mask = m_tokenColors.size() - 1;
    std::size_t slot = std::size_t((nameHash * 2654435761u) >> 16) & mask;
    TokenColorSlot* free = nullptr;
    for (std::size_t probe = 0; probe < 8; ++probe, slot = (slot + 1) & mask) {
        TokenColorSlot& s = m_tokenColors[slot];
        if (s.state == 0) { free = &s; break; }
        if (s.hash == nameHash) {
            if (s.state == 1) out = s.color;
            return s.state == 1;
        }
    }
    // resolve: the last sheet that defines the token wins; whole-value var(--other[, fallback]) chains are followed
    Color c;
    bool ok = false;
    u32 h = nameHash;
    std::string_view text;
    for (int depth = 0; depth < 8; ++depth) {
        bool found = false;
        for (auto it = m_sheets.rbegin(); it != m_sheets.rend() && !found; ++it) found = it->token(h, text);
        if (!found) break;
        text = trimSpaces(text);
        if (!text.starts_with("var(")) {
            ok = parseColor(text, c);
            break;
        }
        const auto close = text.rfind(')');
        if (close == std::string_view::npos) break;
        const std::string_view inner = text.substr(4, close - 4);
        const auto comma = inner.find(',');
        h = hashId(trimSpaces(inner.substr(0, comma)));
        std::string_view probe;
        bool defined = false;
        for (auto it = m_sheets.rbegin(); it != m_sheets.rend() && !defined; ++it) defined = it->token(h, probe);
        if (!defined) {
            if (comma != std::string_view::npos) ok = parseColor(trimSpaces(inner.substr(comma + 1)), c);
            break;
        }
    }
    if (free) *free = {nameHash, u8(ok ? 1 : 2), c};
    if (ok) out = c;
    return ok;
}

// ---- UiContext -------------------------------------------------------------------------------------------------------
u32 UiContext::addStyleSheetTracked(StyleSheet sheet) {
    m_anyStyleDirty = true;
    return m_styles.addSheetTracked(std::move(sheet));
}
bool UiContext::replaceStyleSheet(u32 handle, StyleSheet sheet) {
    if (!m_styles.replaceSheet(handle, std::move(sheet))) return false;
    m_anyStyleDirty = true;
    return true;
}
bool UiContext::removeStyleSheet(u32 handle) {
    if (!m_styles.removeSheet(handle)) return false;
    if (handle == m_themeSheet) m_themeSheet = 0;
    m_anyStyleDirty = true;
    return true;
}
std::expected<void, UiError> UiContext::setThemeStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    auto sheet = StyleSheet::parse(css, diags);
    if (!sheet) {
        if (diags) diags->push_back(sheet.error());
        return std::unexpected(UiError::StyleParse);
    }
    if (m_styles.hasSheet(m_themeSheet)) (void)replaceStyleSheet(m_themeSheet, std::move(*sheet));
    else m_themeSheet = addStyleSheetTracked(std::move(*sheet));
    return {};
}

} // namespace fuse::ui
