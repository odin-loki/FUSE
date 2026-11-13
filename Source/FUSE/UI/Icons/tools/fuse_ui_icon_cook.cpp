// fuse_ui_icon_cook: cooks SVG icons into a .fuseicons pack (format in fuse/ui/icon_set.hpp).
//   fuse_ui_icon_cook --out=<pack.fuseicons> [--list] <file.svg | directory>...
// Directories contribute every *.svg in them (name = file stem). --list prints the index of the written pack.
// The pack is written only when every icon cooked: on an error nothing is written and the exit code is 1.
#include "fuse/ui/icons/icon_cook.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fuse::ui;

int main(int argc, char** argv) {
    std::string out;
    bool list = false;
    std::vector<icons::SvgSource> sources;
    int errors = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--out=", 0) == 0) out = a.substr(6);
        else if (a == "--list") list = true;
        else if (a.rfind("--sizes=", 0) == 0) {
            // accepted and ignored: packs hold vector shapes, not per-size rasters (the option of the coverage cooker)
        } else if (std::filesystem::is_directory(a)) {
            auto d = icons::loadSvgDirectory(a);
            if (!d) { std::fprintf(stderr, "%s\n", d.error().c_str()); ++errors; continue; }
            for (auto& s : *d) sources.push_back(std::move(s));
        } else {
            auto s = icons::loadSvgFile(a);
            if (!s) { std::fprintf(stderr, "%s\n", s.error().c_str()); ++errors; continue; }
            sources.push_back(std::move(*s));
        }
    }
    if (out.empty() || (sources.empty() && !errors)) {
        std::fprintf(stderr, "usage: fuse_ui_icon_cook --out=<pack.fuseicons> [--list] <file.svg | dir>...\n");
        return 1;
    }
    if (errors) return 1;
    icons::CookStats st;
    auto pack = icons::cookIcons(std::move(sources), &st);
    if (!pack) { std::fprintf(stderr, "fuse_ui_icon_cook: %s\n", pack.error().c_str()); return 1; }
    std::ofstream f(out, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(pack->data()), std::streamsize(pack->size()))) {
        std::fprintf(stderr, "%s: cannot write\n", out.c_str());
        return 1;
    }
    std::printf("fuse_ui_icon_cook: %u icons (%u multi-colour, %u shapes) -> %s (%zu bytes)\n", st.icons, st.multiColor,
                st.shapes, out.c_str(), st.bytes);
    if (list) {
        auto set = IconSet::fromBytes(*pack);
        if (!set) { std::fprintf(stderr, "reload failed: %s\n", set.error().c_str()); return 1; }
        for (u32 i = 0; i < set->count(); ++i)
            std::printf("  %.*s%s\n", int(set->name(i).size()), set->name(i).data(), set->multiColor(i) ? " (multi-colour)" : "");
    }
    return 0;
}
