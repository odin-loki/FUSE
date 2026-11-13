#include "fuse/ui/icons.hpp"

#include <array>
#include <string>

namespace fuse::ui {

namespace {
struct IconDef {
    std::string_view name;
    std::string_view body; ///< SVG elements inside the shared <svg> wrapper
};
// Original artwork for FUSE (simple geometric line icons on a 24 x 24 grid).
constexpr std::array<IconDef, kIconCount> kIcons{{
    {"chevron-left", R"(<path d="M15 6l-6 6 6 6"/>)"},
    {"chevron-right", R"(<path d="M9 6l6 6-6 6"/>)"},
    {"chevron-up", R"(<path d="M6 15l6-6 6 6"/>)"},
    {"chevron-down", R"(<path d="M6 9l6 6 6-6"/>)"},
    {"close", R"(<path d="M6 6l12 12M18 6L6 18"/>)"},
    {"check", R"(<path d="M5 12.5l4.5 4.5L19 7.5"/>)"},
    {"search", R"(<circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.5 15.5L20.5 20.5"/>)"},
    {"folder", R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>)"},
    {"file", R"(<path d="M7 3h7l5 5v11a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M14 3v5h5"/>)"},
    {"save", R"(<path d="M5 3h11l5 5v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M8 3v5h7V3"/><path d="M7 21v-7h10v7"/>)"},
    {"undo", R"(<path d="M9 14L4 9l5-5"/><path d="M4 9h10a6 6 0 0 1 0 12h-3"/>)"},
    {"redo", R"(<path d="M15 14l5-5-5-5"/><path d="M20 9H10a6 6 0 0 0 0 12h3"/>)"},
    {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
    {"minus", R"(<path d="M5 12h14"/>)"},
    {"settings", R"(<circle cx="12" cy="12" r="2.8"/><circle cx="12" cy="12" r="6.6"/>)"
                 R"(<path d="M12 2.2v3.2M12 18.6v3.2M2.2 12h3.2M18.6 12h3.2M5.07 5.07l2.26 2.26M16.67 16.67l2.26 2.26M5.07 18.93l2.26-2.26M16.67 7.33l2.26-2.26"/>)"},
    {"warning", R"(<path d="M12 3.5L21.5 20h-19z"/><path d="M12 10v4.5"/><path d="M12 17.4h.01"/>)"},
    {"info", R"(<circle cx="12" cy="12" r="9"/><path d="M12 11v5.5"/><path d="M12 7.6h.01"/>)"},
    {"error", R"(<circle cx="12" cy="12" r="9"/><path d="M9 9l6 6M15 9l-6 6"/>)"},
    {"menu", R"(<path d="M4 7h16M4 12h16M4 17h16"/>)"},
}};

struct IconData {
    std::array<std::string, kIconCount> svg;
    std::array<SvgImage, kIconCount> image;
    IconData() {
        for (u32 i = 0; i < kIconCount; ++i) {
            svg[i] = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24" fill="none" )"
                     R"(stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">)";
            svg[i] += kIcons[i].body;
            svg[i] += "</svg>";
            if (auto r = SvgImage::parse(svg[i])) image[i] = std::move(*r); // (gate fuse_ui_svg checks every icon parses)
        }
    }
};
const IconData& data() {
    static const IconData d;
    return d;
}
u32 index(Icon icon) { return std::min(u32(icon), kIconCount - 1); }
} // namespace

std::string_view iconName(Icon icon) { return kIcons[index(icon)].name; }

bool findIcon(std::string_view name, Icon& out) {
    for (u32 i = 0; i < kIconCount; ++i)
        if (kIcons[i].name == name) {
            out = Icon(i);
            return true;
        }
    return false;
}

bool findIconHash(u32 nameHash, Icon& out) {
    for (u32 i = 0; i < kIconCount; ++i)
        if (hashId(kIcons[i].name) == nameHash) {
            out = Icon(i);
            return true;
        }
    return false;
}

std::string_view iconSvg(Icon icon) { return data().svg[index(icon)]; }
const SvgImage& iconImage(Icon icon) { return data().image[index(icon)]; }

void drawIcon(Painter& p, Icon icon, const Rect& box, Color color) { iconImage(icon).render(p, box, color); }

} // namespace fuse::ui
