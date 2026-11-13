// fuse_text_cooked (UI3 gate, COOKED text profile):
//   every profile  fuse_text_cook is deterministic: the bundled font cooked twice gives the same bytes, with a valid
//                  .fusefont header; garbage is rejected. (Cross builds cannot run the tool: there cookFont() is
//                  called in-process instead, which MINIMAL / FULL builds have.)
//   COOKED         the build-time pack equals a fresh cook with the same options; the runtime loads packs only (a .ttf
//                  is rejected); the bundled pack's layout of sample strings (glyph ids, pen positions, widths: advances
//                  and kerning) matches the MINIMAL reference the cooker wrote, within 1e-3 px, and the strings rendered
//                  from atlas coverage are byte-identical at every cooked size; other sizes come from the MSDF.
#include "test_util.hpp"

#include "cook_reference.hpp"
#include "fuse/text/font_cook.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace fuse::text;
using fuse::ui::test::finish;

namespace {
std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
#ifdef FUSE_TEXT_COOK_EXE
int run(const std::string& cmd) {
#if defined(_WIN32)
    return std::system(("\"" + cmd + "\"").c_str()); // cmd /c strips one level of quotes
#else
    return std::system(cmd.c_str());
#endif
}
std::string cookCmd(const std::string& out, const std::string& args) {
    return "\"" FUSE_TEXT_COOK_EXE "\" \"--out=" + out + "\" " + args + " \"" FUSE_TEXT_TEST_FONT "\"";
}
#endif

template <class T> T take(const std::vector<std::uint8_t>& b, std::size_t& at) {
    T v{};
    if (at + sizeof v <= b.size()) std::memcpy(&v, b.data() + at, sizeof v);
    at += sizeof v;
    return v;
}
} // namespace

int main() {
    // determinism (any profile: the cooker is always built over MINIMAL)
#ifdef FUSE_TEXT_COOK_EXE
    const std::string dir = FUSE_TEXT_TEST_OUT_DIR;
    const std::string a = dir + "/cooked_a.fusefont", b = dir + "/cooked_b.fusefont";
    const std::string args = FUSE_TEXT_TEST_COOK_ARGS;
    UI_CHECK(run(cookCmd(a, args)) == 0);
    UI_CHECK(run(cookCmd(b, args)) == 0);
    const auto pa = readAll(a), pb = readAll(b);
#elif !FUSE_UI_TEST_TEXT_COOKED
    // no runnable tool (cross build): the library entry point, on the bundled UI font
    std::vector<std::uint8_t> pa, pb;
    {
        auto src = Font::bundled();
        UI_CHECK(src.has_value());
        CookOptions opt;
        opt.sizes = {14.f, 16.f, 17.5f};
        opt.ranges = {{0x20, 0x17F}};
        if (src) {
            auto c1 = cookFont(*src, opt), c2 = cookFont(*src, opt);
            UI_CHECK(c1.has_value() && c2.has_value());
            if (c1 && c2) {
                pa = std::move(*c1);
                pb = std::move(*c2);
            }
        }
    }
#else
    std::printf("fuse_text_cooked: no runnable fuse_text_cook in this build, skipped\n");
    return 77;
    const std::vector<std::uint8_t> pa, pb;
#endif
    std::printf("cooked twice: %zu / %zu bytes, %s\n", pa.size(), pb.size(), pa == pb ? "identical" : "DIFFERENT");
    UI_CHECK(!pa.empty() && pa == pb);
    UI_CHECK(pa.size() > 64 && std::memcmp(pa.data(), "FUSEFONT", 8) == 0);
    std::vector<std::uint8_t> junk(pa.begin(), pa.begin() + 64);
    junk[8] = 99; // unknown version

    if (textProfile() != TextProfile::Cooked) {
        UI_CHECK(!Font::fromBytes(junk)); // not a font either way
        return finish("fuse_text_cooked");
    }
#if defined(FUSE_TEXT_COOKED_PACK)
    UI_CHECK(!Font::fromBytes(junk));
    UI_CHECK(!Font::fromFile(FUSE_TEXT_TEST_FONT)); // no font parsing in COOKED
    const auto built = readAll(FUSE_TEXT_COOKED_PACK);
    std::printf("build-time pack: %zu bytes, %s\n", built.size(), built == pa ? "equal to the fresh cook" : "DIFFERENT");
    UI_CHECK(built == pa);
    auto font = Font::fromFile(FUSE_TEXT_COOKED_PACK);
    UI_CHECK(font.has_value());
    if (!font) return finish("fuse_text_cooked");
    auto copy = Font::fromBytes(pa); // from memory too
    UI_CHECK(copy.has_value() && copy->glyphIndex(U'A') == font->glyphIndex(U'A'));

    // the MINIMAL reference (fuse_text_cook --reference) against the cooked font
    const auto ref = readAll(FUSE_TEXT_COOKED_REFERENCE);
    UI_CHECK(!ref.empty());
    std::vector<float> sizes;
    {
        std::size_t at = 0;
        while (at < ref.size()) {
            const float px = float(take<std::uint32_t>(ref, at)) / 4.f;
            take<float>(ref, at);
            const std::uint32_t n = take<std::uint32_t>(ref, at);
            at += std::size_t(n) * 8;
            const std::uint32_t w = take<std::uint32_t>(ref, at), h = take<std::uint32_t>(ref, at);
            at += std::size_t(w) * h;
            if (sizes.empty() || sizes.back() != px) sizes.push_back(px);
        }
    }
    std::vector<std::uint8_t> mine;
    cookref::dump(*font, sizes, mine);
    std::size_t ra = 0, ma = 0;
    int records = 0, idMismatch = 0, imgMismatch = 0;
    double maxDx = 0.0;
    while (ra < ref.size() && ma < mine.size()) {
        const std::uint32_t q1 = take<std::uint32_t>(ref, ra), q2 = take<std::uint32_t>(mine, ma);
        const float w1 = take<float>(ref, ra), w2 = take<float>(mine, ma);
        const std::uint32_t n1 = take<std::uint32_t>(ref, ra), n2 = take<std::uint32_t>(mine, ma);
        UI_CHECK(q1 == q2 && n1 == n2);
        if (q1 != q2 || n1 != n2) break;
        maxDx = std::max(maxDx, double(std::abs(w1 - w2)));
        for (std::uint32_t k = 0; k < n1; ++k) {
            idMismatch += take<std::uint32_t>(ref, ra) != take<std::uint32_t>(mine, ma);
            maxDx = std::max(maxDx, double(std::abs(take<float>(ref, ra) - take<float>(mine, ma))));
        }
        const std::uint32_t iw1 = take<std::uint32_t>(ref, ra), ih1 = take<std::uint32_t>(ref, ra);
        const std::uint32_t iw2 = take<std::uint32_t>(mine, ma), ih2 = take<std::uint32_t>(mine, ma);
        const std::size_t bytes = std::size_t(iw1) * ih1;
        const bool same = iw1 == iw2 && ih1 == ih2 && ra + bytes <= ref.size() && ma + bytes <= mine.size() &&
                          std::memcmp(ref.data() + ra, mine.data() + ma, bytes) == 0;
        imgMismatch += !same;
        ra += bytes;
        ma += std::size_t(iw2) * ih2;
        ++records;
    }
    std::printf("reference: %d strings at %zu sizes, glyph id mismatches %d, max pen / width difference %.6f px, "
                "rendered strings differing %d\n", records, sizes.size(), idMismatch, maxDx, imgMismatch);
    UI_CHECK(records == int(sizes.size() * cookref::samples().size()));
    UI_CHECK(idMismatch == 0 && maxDx <= 1e-3 && imgMismatch == 0);

    // a size that is not cooked: drawn from the MSDF, with ink
    GlyphAtlas atlas(1024);
    const AtlasGlyph big = atlas.get(*font, font->glyphIndex(U'g'), 100.f);
    long ink = 0;
    for (int y = 0; y < big.h; ++y)
        for (int x = 0; x < big.w; ++x) ink += atlas.pixels()[std::size_t(big.v + y) * std::size_t(atlas.size()) + big.u + std::size_t(x)];
    std::printf("uncooked 100 px 'g': %dx%d, mean coverage %.3f\n", big.w, big.h, double(ink) / 255.0 / std::max(1, big.w * big.h));
    UI_CHECK(big.w > 30 && big.h > 50 && ink > 0);
#endif
    return finish("fuse_text_cooked");
}
