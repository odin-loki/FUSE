// SVG files -> .fuseicons (fuse/ui/icons/icon_cook.hpp): parse with SvgImage, encode with encodeIconPack.
#include "fuse/ui/icons/icon_cook.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fuse::ui::icons {

namespace fs = std::filesystem;

std::expected<std::vector<u8>, std::string> cookIcons(std::vector<SvgSource> sources, CookStats* stats) {
    if (sources.empty()) return std::unexpected("no icons");
    std::vector<IconPackSource> parsed;
    parsed.reserve(sources.size());
    for (SvgSource& s : sources) {
        auto img = SvgImage::parse(s.svg);
        if (!img) return std::unexpected("icon '" + s.name + "': " + img.error().message + " (offset " + std::to_string(img.error().offset) + ")");
        if (img->unsupportedElements() > 0)
            return std::unexpected("icon '" + s.name + "': " + std::to_string(img->unsupportedElements()) +
                                   " unsupported SVG element(s) (text, use, image, clipPath, mask, filter, ...)");
        parsed.push_back({std::move(s.name), std::move(*img)});
    }
    IconPackStats st;
    auto pack = encodeIconPack(std::move(parsed), &st);
    if (pack && stats) *stats = {st.icons, st.multiColor, st.shapes, st.bytes};
    return pack;
}

std::expected<SvgSource, std::string> loadSvgFile(const std::string& path) {
    const fs::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::unexpected(path + ": cannot open");
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::u8string stem = p.stem().u8string();
    return SvgSource{std::string(reinterpret_cast<const char*>(stem.data()), stem.size()), ss.str()};
}

std::expected<std::vector<SvgSource>, std::string> loadSvgDirectory(const std::string& dir) {
    std::error_code ec;
    fs::directory_iterator it(fs::path(std::u8string(reinterpret_cast<const char8_t*>(dir.data()), dir.size())), ec);
    if (ec) return std::unexpected(dir + ": cannot list");
    std::vector<std::string> files;
    for (const fs::directory_entry& e : it) {
        std::error_code fe;
        if (!e.is_regular_file(fe) || fe || e.path().extension() != ".svg") continue;
        const std::u8string u = e.path().u8string();
        files.emplace_back(reinterpret_cast<const char*>(u.data()), u.size());
    }
    std::sort(files.begin(), files.end()); // directory order is not stable across file systems
    std::vector<SvgSource> out;
    for (const std::string& f : files) {
        auto s = loadSvgFile(f);
        if (!s) return std::unexpected(s.error());
        out.push_back(std::move(*s));
    }
    std::sort(out.begin(), out.end(), [](const SvgSource& a, const SvgSource& b) { return a.name < b.name; });
    return out;
}

} // namespace fuse::ui::icons
