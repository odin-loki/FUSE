// Pure-C++ tables for the Android backend (android.cpp), tested on every host (tests/test_mobile_map.cpp).
#pragma once
#include "fuse/ui/ui.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace fuse::app::android {

/// AKEYCODE_* (android/keycodes.h values) -> fuse Key.
inline fuse::ui::Key keyFromKeycode(int32_t kc) {
    using K = fuse::ui::Key;
    if (kc >= 29 && kc <= 54) { // AKEYCODE_A..Z
        static constexpr K letters[26] = {K::A, K::B, K::C, K::D, K::E, K::F, K::G, K::H, K::I, K::J, K::K, K::L, K::M,
                                          K::N, K::O, K::P, K::Q, K::R, K::S, K::T, K::U, K::V, K::W, K::X, K::Y, K::Z};
        return letters[kc - 29];
    }
    if (kc >= 7 && kc <= 16) { // AKEYCODE_0..9
        static constexpr K digits[10] = {K::Num0, K::Num1, K::Num2, K::Num3, K::Num4, K::Num5, K::Num6, K::Num7, K::Num8, K::Num9};
        return digits[kc - 7];
    }
    if (kc >= 131 && kc <= 142) { // AKEYCODE_F1..F12
        static constexpr K fk[12] = {K::F1, K::F2, K::F3, K::F4, K::F5, K::F6, K::F7, K::F8, K::F9, K::F10, K::F11, K::F12};
        return fk[kc - 131];
    }
    switch (kc) {
    case 61: return K::Tab;                  // AKEYCODE_TAB
    case 66: case 160: return K::Enter;      // ENTER, NUMPAD_ENTER
    case 111: case 4: return K::Escape;      // ESCAPE, BACK
    case 62: return K::Space;
    case 67: return K::Backspace;            // AKEYCODE_DEL
    case 112: return K::Delete;              // FORWARD_DEL
    case 21: return K::Left; case 22: return K::Right; case 19: return K::Up; case 20: return K::Down; // DPAD
    case 122: return K::Home; case 123: return K::End; // MOVE_HOME / MOVE_END
    case 92: return K::PageUp; case 93: return K::PageDown;
    case 71: return K::BracketLeft; case 72: return K::BracketRight;
    default: return K::Other;
    }
}

/// AMETA_* state -> fuse Mods.
inline std::uint8_t modsFromMeta(int32_t meta) {
    std::uint8_t m = 0;
    if (meta & 0x1) m |= fuse::ui::kModShift;      // AMETA_SHIFT_ON
    if (meta & 0x1000) m |= fuse::ui::kModCtrl;    // AMETA_CTRL_ON
    if (meta & 0x2) m |= fuse::ui::kModAlt;        // AMETA_ALT_ON
    if (meta & 0x10000) m |= fuse::ui::kModSuper;  // AMETA_META_ON
    return m;
}

/// A11yRole -> AccessibilityNodeInfo.setClassName (TalkBack derives the spoken role from it).
inline const char* className(fuse::ui::A11yRole r) {
    using R = fuse::ui::A11yRole;
    switch (r) {
    case R::Button: return "android.widget.Button";
    case R::CheckBox: return "android.widget.CheckBox";
    case R::RadioButton: case R::Tab: return "android.widget.RadioButton";
    case R::Switch: return "android.widget.Switch";
    case R::Slider: case R::Dial: return "android.widget.SeekBar";
    case R::ProgressBar: return "android.widget.ProgressBar";
    case R::TextField: return "android.widget.EditText";
    case R::ScrollArea: case R::ScrollBar: return "android.widget.ScrollView";
    case R::List: return "android.widget.ListView";
    case R::Image: return "android.widget.ImageView";
    case R::ComboBox: return "android.widget.Spinner";
    case R::SpinBox: return "android.widget.NumberPicker";
    case R::Text: case R::Link: return "android.widget.TextView";
    default: return "android.view.View";
    }
}

/// UTF-16 index -> UTF-8 byte offset (InputConnection carets are UTF-16).
inline int32_t utf8OffsetOfUtf16(const char* s, int32_t len, int32_t u16) {
    int32_t i = 0, units = 0;
    while (i < len && units < u16) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += n == 4 ? 2 : 1;
        i += n;
    }
    return i > len ? len : i;
}

} // namespace fuse::app::android

namespace fuse::app::uikit {
/// A11yRole -> UIAccessibilityTraits bits (UIKit's documented constant values).
inline std::uint64_t traits(fuse::ui::A11yRole r, bool disabled, bool checked) {
    using R = fuse::ui::A11yRole;
    constexpr std::uint64_t kButton = 1ull << 0, kLink = 1ull << 1, kImage = 1ull << 2, kSelected = 1ull << 3,
                            kStatic = 1ull << 6, kNotEnabled = 1ull << 8, kAdjustable = 1ull << 12, kHeader = 1ull << 16;
    (void)kHeader;
    std::uint64_t t = 0;
    switch (r) {
    case R::Button: case R::CheckBox: case R::RadioButton: case R::Switch: case R::Tab: case R::ComboBox: t = kButton; break;
    case R::Link: t = kLink; break;
    case R::Image: t = kImage; break;
    case R::Text: t = kStatic; break;
    case R::Slider: case R::Dial: case R::SpinBox: t = kAdjustable; break;
    case R::ProgressBar: t = 1ull << 10; break; // UpdatesFrequently
    default: break;
    }
    if (checked) t |= kSelected;
    if (disabled) t |= kNotEnabled;
    return t;
}

/// UTF-16 code units in a UTF-8 string (UITextInput positions / NSRange are UTF-16).
inline std::int32_t utf16Length(std::string_view s) {
    std::int32_t units = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += n == 4 ? 2 : 1;
        i += std::size_t(n);
    }
    return units;
}

/// iOS IME marked text (UITextInput setMarkedText / unmarkText / insertText) mapped onto UiContext: the marked
/// string is the composition (UiContext::textComposition, caret in UTF-8 bytes) and finished text goes to
/// UiContext::textInput. FUSE keeps no document mirror, so the "document" UIKit sees is exactly the marked string
/// (positions 0..utf16Length(marked)); kana -> kanji, pinyin, Hangul jamo and dictation all fit that model.
struct MarkedText {
    std::string marked; ///< UTF-8, empty when nothing is being composed
    struct Step {
        bool setComposition = false; ///< call textComposition(composition, caret)
        std::string composition;
        std::int32_t caret = 0;      ///< UTF-8 byte offset into composition
        std::string commit;          ///< non-empty: call textInput(commit) (after the composition update)
    };
    bool active() const { return !marked.empty(); }
    std::int32_t length16() const { return utf16Length(marked); }
    /// setMarkedText:selectedRange: (`selUtf16` = selectedRange.location, relative to the new marked text).
    Step setMarked(std::string_view utf8, std::int32_t selUtf16) {
        marked.assign(utf8);
        Step st;
        st.setComposition = true;
        st.composition = marked;
        st.caret = fuse::app::android::utf8OffsetOfUtf16(marked.data(), std::int32_t(marked.size()), selUtf16 < 0 ? 0 : selUtf16);
        return st;
    }
    /// unmarkText: the marked text is accepted as-is.
    Step unmark() {
        Step st;
        st.setComposition = !marked.empty();
        st.commit = std::move(marked);
        marked.clear();
        return st;
    }
    /// insertText: replaces the marked text (if any) with `utf8`.
    Step insert(std::string_view utf8) {
        Step st;
        st.setComposition = !marked.empty();
        marked.clear();
        st.commit.assign(utf8);
        return st;
    }
};
} // namespace fuse::app::uikit
