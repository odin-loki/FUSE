// fuse_app_cocoa_map: the platform-neutral tables used by the (Apple-only) Cocoa backend.
#include "../App/src/cocoa_map.hpp"
#include <cstdio>
#include <cstring>
using namespace fuse::ui;
using namespace fuse::app::cocoa;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
    CHECK(keyFromVk(0x00) == Key::A && keyFromVk(0x06) == Key::Z && keyFromVk(0x24) == Key::Enter);
    CHECK(keyFromVk(0x7B) == Key::Left && keyFromVk(0x35) == Key::Escape && keyFromVk(0x6F) == Key::F12);
    CHECK(keyFromVk(0x1D) == Key::Num0 && keyFromVk(0x3F) == Key::Other);
    // every letter A..Z distinct
    unsigned short vks[] = {0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26,0x28,0x25,0x2E,0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11,0x20,0x09,0x0D,0x07,0x10,0x06};
    for (int i = 0; i < 26; ++i) for (int j = i + 1; j < 26; ++j) CHECK(keyFromVk(vks[i]) != keyFromVk(vks[j]));
    CHECK(modsFromFlags((1ul << 17) | (1ul << 20)) == (kModShift | kModSuper));
    CHECK(modsFromFlags(1ul << 18) == kModCtrl && modsFromFlags(1ul << 19) == kModAlt && modsFromFlags(0) == 0);
    CHECK(flipY(10.f, 100.f) == 90.f);
    CHECK(!std::strcmp(nsRole(A11yRole::Button), "AXButton") && !std::strcmp(nsRole(A11yRole::None), "AXUnknown"));
    CHECK(!std::strcmp(nsSubrole(A11yRole::Switch), "AXSwitch") && nsSubrole(A11yRole::Button) == nullptr);
    std::printf("fuse_app_cocoa_map: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
