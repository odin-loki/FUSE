// fuse_text_cook: TrueType / OpenType -> .fusefont (COOKED text profile; see fuse/text/font_cook.hpp).
//   fuse_text_cook --out=<pack.fusefont> [--sizes=8-40,48] [--color-sizes=...] [--msdf] [--chars=20-7E,A0-17F]
//                  [--no-kerning] [--face=N] [--variation=wght:700,wdth:80 | --instance=<name>] [--reference=<file>]
//                  <font.ttf>
// Sizes are pixel sizes (lists and inclusive integer ranges, decimals allowed: 17.5); chars are hex code point ranges.
// --reference writes the MINIMAL layout / render dump of sample strings at the coverage sizes (fuse_text_cooked).
// --variation / --instance cook a variable font at those axis values / that named instance (advances, metrics,
// outlines and COLRv1 Var* paints of the instance; a pack holds one instance).
#include "fuse/text/font_cook.hpp"

#include "cook_reference.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {
bool parseSizes(std::string_view s, std::vector<float>& out) {
    while (!s.empty()) {
        const std::size_t comma = s.find(',');
        const std::string item(s.substr(0, comma));
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
        if (item.empty()) continue;
        const std::size_t dash = item.find('-');
        if (dash != std::string::npos) {
            const int a = std::atoi(item.substr(0, dash).c_str()), b = std::atoi(item.substr(dash + 1).c_str());
            if (a <= 0 || b < a) return false;
            for (int v = a; v <= b; ++v) out.push_back(float(v));
        } else {
            const float v = float(std::atof(item.c_str()));
            if (!(v > 0.f)) return false;
            out.push_back(v);
        }
    }
    return true;
}
bool parseRanges(std::string_view s, std::vector<std::pair<char32_t, char32_t>>& out) {
    while (!s.empty()) {
        const std::size_t comma = s.find(',');
        const std::string item(s.substr(0, comma));
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
        if (item.empty()) continue;
        const std::size_t dash = item.find('-');
        const unsigned long a = std::strtoul(item.substr(0, dash).c_str(), nullptr, 16);
        const unsigned long b = dash == std::string::npos ? a : std::strtoul(item.substr(dash + 1).c_str(), nullptr, 16);
        if (b < a || b > 0x10FFFF) return false;
        out.push_back({char32_t(a), char32_t(b)});
    }
    return true;
}
bool writeFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}
} // namespace

// "wght:700,wdth:80" -> axis settings.
bool parseVariations(std::string_view s, std::vector<fuse::text::FontVariation>& out) {
    while (!s.empty()) {
        const std::size_t comma = s.find(',');
        const std::string_view item = s.substr(0, comma);
        if (item.size() < 6 || item[4] != ':') return false;
        std::uint32_t tag = 0;
        for (int k = 0; k < 4; ++k) tag = (tag << 8) | std::uint8_t(item[std::size_t(k)]);
        char* end = nullptr;
        const std::string num(item.substr(5));
        const float v = std::strtof(num.c_str(), &end);
        if (end == num.c_str() || *end) return false;
        out.push_back({tag, v});
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return true;
}

int main(int argc, char** argv) {
    std::string out, input, reference, instanceName;
    std::vector<fuse::text::FontVariation> variation;
    fuse::text::CookOptions opt;
    std::uint32_t face = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto val = [&](std::string_view key) { return a.substr(key.size()); };
        bool ok = true;
        if (a.starts_with("--out=")) out = val("--out=");
        else if (a.starts_with("--sizes=")) ok = parseSizes(val("--sizes="), opt.sizes);
        else if (a.starts_with("--color-sizes=")) ok = parseSizes(val("--color-sizes="), opt.colorSizes);
        else if (a.starts_with("--chars=")) ok = parseRanges(val("--chars="), opt.ranges);
        else if (a.starts_with("--face=")) face = std::uint32_t(std::atoi(std::string(val("--face=")).c_str()));
        else if (a.starts_with("--reference=")) reference = val("--reference=");
        else if (a.starts_with("--variation=")) ok = parseVariations(val("--variation="), variation);
        else if (a.starts_with("--instance=")) instanceName = val("--instance=");
        else if (a == "--msdf") opt.msdf = true;
        else if (a == "--no-kerning") opt.kerning = false;
        else if (!a.starts_with("--")) input = a;
        else ok = false;
        if (!ok) {
            std::fprintf(stderr, "fuse_text_cook: bad argument %s\n", argv[i]);
            return 2;
        }
    }
    if (out.empty() || input.empty()) {
        std::fprintf(stderr, "usage: fuse_text_cook --out=<pack.fusefont> [--sizes=..] [--color-sizes=..] [--msdf] [--chars=..] "
                             "[--no-kerning] [--face=N] [--variation=tag:value,..] [--instance=name] [--reference=<file>] "
                             "<font>\n");
        return 2;
    }
    fuse::text::enableColorGlyphs(); // before loading: bitmap strikes are indexed at load
    fuse::text::enableMsdf();
    fuse::text::enableVariableFonts();
    const auto t0 = std::chrono::steady_clock::now();
    auto font = fuse::text::Font::fromFile(input, face);
    if (!font) {
        std::fprintf(stderr, "fuse_text_cook: cannot load %s\n", input.c_str());
        return 1;
    }
    if (!instanceName.empty()) { // a named instance: its coordinates as the variation
        const auto named = font->namedInstances();
        const auto axes = font->axes();
        bool found = false;
        for (const fuse::text::NamedInstance& ni : named)
            if (ni.name == instanceName) {
                for (std::size_t k = 0; k < axes.size() && k < ni.coords.size(); ++k) variation.push_back({axes[k].tag, ni.coords[k]});
                found = true;
                break;
            }
        if (!found) {
            std::fprintf(stderr, "fuse_text_cook: %s has no named instance \"%s\"\n", input.c_str(), instanceName.c_str());
            return 1;
        }
    }
    if (!variation.empty()) {
        if (!font->isVariable()) {
            std::fprintf(stderr, "fuse_text_cook: %s is not a variable font\n", input.c_str());
            return 1;
        }
        font->setVariation(variation);
    }
    auto pack = fuse::text::cookFont(*font, opt);
    if (!pack) {
        std::fprintf(stderr, "fuse_text_cook: %s: %s\n", input.c_str(), fuse::text::textErrorName(pack.error()));
        return 1;
    }
    if (!writeFile(out, *pack)) {
        std::fprintf(stderr, "fuse_text_cook: cannot write %s\n", out.c_str());
        return 1;
    }
    if (!reference.empty()) {
        std::vector<std::uint8_t> ref;
        fuse::text::cookref::dump(*font, opt.sizes, ref);
        if (!writeFile(reference, ref)) {
            std::fprintf(stderr, "fuse_text_cook: cannot write %s\n", reference.c_str());
            return 1;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fuse_text_cook: %s -> %s (%zu bytes, %zu sizes%s, %.0f ms)\n", input.c_str(), out.c_str(), pack->size(),
                opt.sizes.size(), opt.msdf ? ", msdf" : "", ms);
    return 0;
}
