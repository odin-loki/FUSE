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
    std::printf("fuse_app_mobile_map: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
