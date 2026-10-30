// fuse_app_mobile_map: the platform-neutral tables of the Android (android.cpp) and UIKit (uikit.mm) backends.
#include "../App/src/android_map.hpp"
#include <cstdio>
#include <cstring>
using namespace fuse::ui;
namespace an = fuse::app::android;
namespace uk = fuse::app::uikit;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
    CHECK(an::keyFromKeycode(29) == Key::A && an::keyFromKeycode(54) == Key::Z && an::keyFromKeycode(7) == Key::Num0);
    CHECK(an::keyFromKeycode(16) == Key::Num9 && an::keyFromKeycode(131) == Key::F1 && an::keyFromKeycode(142) == Key::F12);
    CHECK(an::keyFromKeycode(66) == Key::Enter && an::keyFromKeycode(67) == Key::Backspace && an::keyFromKeycode(112) == Key::Delete);
    CHECK(an::keyFromKeycode(21) == Key::Left && an::keyFromKeycode(4) == Key::Escape && an::keyFromKeycode(0) == Key::Other);
    CHECK(an::modsFromMeta(0x1 | 0x1000) == (kModShift | kModCtrl) && an::modsFromMeta(0x2 | 0x10000) == (kModAlt | kModSuper));
    CHECK(!std::strcmp(an::className(A11yRole::Button), "android.widget.Button"));
    CHECK(!std::strcmp(an::className(A11yRole::TextField), "android.widget.EditText"));
    CHECK(!std::strcmp(an::className(A11yRole::Group), "android.view.View"));
    const char* s = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80z"; // a é 中 😀 z
    const int n = int(std::strlen(s));
    CHECK(an::utf8OffsetOfUtf16(s, n, 0) == 0 && an::utf8OffsetOfUtf16(s, n, 1) == 1 && an::utf8OffsetOfUtf16(s, n, 2) == 3);
    CHECK(an::utf8OffsetOfUtf16(s, n, 3) == 6 && an::utf8OffsetOfUtf16(s, n, 5) == 10 && an::utf8OffsetOfUtf16(s, n, 99) == n);
    CHECK(uk::traits(A11yRole::Button, false, false) == 1);
    CHECK(uk::traits(A11yRole::Slider, true, false) == ((1ull << 12) | (1ull << 8)));
    CHECK(uk::traits(A11yRole::CheckBox, false, true) == (1ull | (1ull << 3)));
    // iOS marked text: pinyin "ni" -> "nǐ" (2 units, caret at the end) -> commit 你; unmark commits as-is
    CHECK(uk::utf16Length(s) == 6 && uk::utf16Length("") == 0);
    uk::MarkedText mt;
    auto st = mt.setMarked("ni", 2);
    CHECK(mt.active() && st.setComposition && st.composition == "ni" && st.caret == 2 && st.commit.empty());
    st = mt.setMarked("n\xC7\x90", 1); // nǐ, caret after n
    CHECK(st.caret == 1 && mt.length16() == 2);
    st = mt.insert("\xE4\xBD\xA0"); // 你
    CHECK(!mt.active() && st.setComposition && st.composition.empty() && st.commit == "\xE4\xBD\xA0");
    st = mt.insert("x"); // plain typing: no composition reset
    CHECK(!st.setComposition && st.commit == "x");
    mt.setMarked("\xF0\x9F\x98\x80" "a", 3); // 😀a, caret after a (UTF-16 3 -> byte 5)
    CHECK(mt.setMarked("\xF0\x9F\x98\x80" "a", 3).caret == 5);
    st = mt.unmark();
    CHECK(!mt.active() && st.setComposition && st.commit == "\xF0\x9F\x98\x80" "a");
    CHECK(!mt.unmark().setComposition);
    { // A11yState: expanded / selected / suggestions (completer, editable combo)
        A11yState st;
        CHECK(an::stateDescription(st).empty());
        st.expanded = 0;
        CHECK(an::stateDescription(st) == "collapsed");
        st.expanded = 1;
        st.autoComplete = A11yAutoComplete::List;
        CHECK(an::stateDescription(st) == "expanded, suggestions");
        st.autoComplete = A11yAutoComplete::Inline;
        CHECK(an::stateDescription(st) == "expanded");
        st.selected = true;
        float f[3] = {};
        an::a11yStateFloats(st, f, 7.f);
        CHECK(f[0] == 1.f && f[1] == 1.f && f[2] == 7.f);
        CHECK(uk::traits(A11yRole::ListItem, false, false, true) == (1ull << 3));
        CHECK(uk::traits(A11yRole::ListItem, false, false, false) == 0);
    }
    std::printf("fuse_app_mobile_map: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
