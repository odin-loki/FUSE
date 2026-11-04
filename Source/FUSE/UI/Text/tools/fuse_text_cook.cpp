// fuse_text_cook: TrueType / OpenType -> .fusefont (COOKED text profile; see fuse/text/font_cook.hpp).
//   fuse_text_cook --out=<pack.fusefont> [--sizes=8-40,48] [--color-sizes=...] [--msdf] [--chars=20-7E,A0-17F]
//                  [--no-kerning] [--face=N] [--reference=<file>] <font.ttf>
// Sizes are pixel sizes (lists and inclusive integer ranges, decimals allowed: 17.5); chars are hex code point ranges.
// --reference writes the MINIMAL layout / render dump of sample strings at the coverage sizes (fuse_text_cooked).
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

int main(int argc, char** argv) {
    std::string out, input, reference;
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
                             "[--no-kerning] [--face=N] [--reference=<file>] <font>\n");
        return 2;
    }
    fuse::text::enableColorGlyphs(); // before loading: bitmap strikes are indexed at load
    fuse::text::enableMsdf();
    const auto t0 = std::chrono::steady_clock::now();
    auto font = fuse::text::Font::fromFile(input, face);
    if (!font) {
        std::fprintf(stderr, "fuse_text_cook: cannot load %s\n", input.c_str());
        return 1;
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
