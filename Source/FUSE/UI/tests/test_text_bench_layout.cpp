// Gate fuse_text_bench_layout: shaping / layout throughput over a ~1 MB mixed-script document (Latin code and prose,
// Cyrillic, Greek, CJK, Hebrew / Arabic, emoji) with the bundled font and its fallback chain, the way the widgets lay
// text out:
//   paragraphs   markdownView: one wrapped layoutText per paragraph (cold: fresh fonts; warm: same widths again;
//                resize: a new width, shaping reused)
//   code frames  codeEdit: per visible line, an elided layoutText and a measureText per token up to the right edge,
//                one caret measure; a frame is 50 lines, scrolled over the whole document (cold, then warm)
//   one block    the whole document as a single wrapped layoutText (a huge label)
//   ui           markdownView in a UiContext: first update, then a viewport resize
// Budgets (Release only) are about 4x what this machine (64-core Windows desktop, MSVC Release) measured, so a slower
// CI box or a loaded machine still passes while a complexity regression (quadratic fallback lookup, cache thrash)
// fails. Steady state: re-laying out a visible window (one screen of paragraphs, one code frame) allocates nothing.
#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<long> g_allocs{0};
bool g_counting = false;
} // namespace

void* operator new(std::size_t n) {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::text;
using fuse::ui::test::finish;
namespace ui = fuse::ui;

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct ChainFont {
    Font font;
    std::vector<Font> fallbacks;
    ChainFont() {
        auto f = Font::bundled();
        if (!f) {
            std::fprintf(stderr, "bundled font missing\n");
            std::exit(2);
        }
        font = std::move(*f);
        fallbacks = Font::bundledFallbacks();
        std::vector<const Font*> chain;
        for (const Font& x : fallbacks) chain.push_back(&x);
        font.setFallbacks(chain);
    }
};

// ---- deterministic document ----------------------------------------------------------------------------------------
struct Rng {
    std::uint32_t s = 0x12345678u;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    std::uint32_t operator()(std::uint32_t n) { return next() % n; }
};

std::string u8(std::u32string_view cps) {
    std::string s;
    for (char32_t c : cps) appendUtf8(s, c);
    return s;
}

const char* const kLatin[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "layout", "engine", "shaping",
                              "glyph", "paragraph", "window", "render", "frame", "budget", "cache", "texture", "a", "of",
                              "and", "to", "in", "is", "with", "for", "measure", "wrap", "line"};
const char* const kCode[] = {"local", "function", "end", "return", "if", "then", "else", "for", "do", "while", "nil",
                             "true", "self", "value", "count", "node", "ui", "layout", "x", "y", "i", "=", "==", "+", "..",
                             "(", ")", "{", "}", ",", "\"text\"", "42", "0.5", "--", "string.format", "table.insert"};
const std::u32string_view kCyrillic[] = {U"текст", U"окно", U"и",
                                         U"шрифт", U"строка",
                                         U"кадр"};
const std::u32string_view kGreek[] = {U"κείμενο", U"γραμμή",
                                      U"και", U"σελίδα"};
const std::u32string_view kHebrew[] = {U"שלום", U"עולם", U"טקסט"};
const std::u32string_view kArabic[] = {U"سلام", U"النص", U"كتاب"};
const char32_t kHan[] = {U'的', U'一', U'是', U'不', U'了', U'人', U'我', U'在', U'有',
                         U'他', U'这', U'中', U'大', U'来', U'上', U'国', U'字', U'文'};
const std::u32string_view kEmoji[] = {U"\U0001F600", U"\U0001F44D\U0001F3FD", U"\U0001F468‍\U0001F469‍\U0001F467",
                                      U"❤️", U"\U0001F1E9\U0001F1EA", U"\U0001F680"};

void prose(Rng& r, std::string& out, std::size_t words) {
    for (std::size_t w = 0; w < words; ++w) {
        if (w) out += ' ';
        const std::uint32_t k = r(100);
        if (k < 70) out += kLatin[r(std::size(kLatin))];
        else if (k < 78) out += u8(kCyrillic[r(std::size(kCyrillic))]);
        else if (k < 83) out += u8(kGreek[r(std::size(kGreek))]);
        else if (k < 90) for (std::uint32_t c = 0, n = 2 + r(5); c < n; ++c) appendUtf8(out, kHan[r(std::size(kHan))]);
        else if (k < 93) out += u8(kHebrew[r(std::size(kHebrew))]);
        else if (k < 96) out += u8(kArabic[r(std::size(kArabic))]);
        else out += u8(kEmoji[r(std::size(kEmoji))]);
        if (r(12) == 0) out += r(2) ? "," : ".";
    }
}

/// Markdown-ish text: prose paragraphs (short and long), code blocks, a few RTL paragraphs, blank lines between.
std::string makeDocument(std::size_t bytes) {
    Rng r;
    std::string d;
    d.reserve(bytes + 4096);
    while (d.size() < bytes) {
        const std::uint32_t k = r(10);
        if (k < 5) {
            prose(r, d, 5 + r(r(4) == 0 ? 200 : 60));
        } else if (k < 8) { // code block: short and long lines, indentation
            for (std::uint32_t l = 0, n = 3 + r(12); l < n; ++l) {
                if (l) d += '\n';
                d.append(std::size_t(r(4)) * 4, ' ');
                for (std::uint32_t t = 0, m = 1 + r(r(6) == 0 ? 40 : 10); t < m; ++t) {
                    if (t) d += ' ';
                    d += kCode[r(std::size(kCode))];
                }
                if (r(5) == 0) {
                    d += " -- ";
                    prose(r, d, 3 + r(8));
                }
            }
        } else if (k < 9) { // right-to-left paragraph with Latin and digits
            for (std::uint32_t w = 0, n = 6 + r(30); w < n; ++w) {
                if (w) d += ' ';
                const std::uint32_t c = r(10);
                d += c < 4 ? u8(kHebrew[r(std::size(kHebrew))]) : c < 8 ? u8(kArabic[r(std::size(kArabic))])
                                                                          : std::string(kLatin[r(std::size(kLatin))]);
                if (r(9) == 0) d += std::to_string(r(10000));
            }
        } else { // heading / list item
            d += r(2) ? "## " : "- ";
            prose(r, d, 2 + r(8));
        }
        d += "\n\n";
    }
    return d;
}

void splitBy(std::string_view s, std::string_view sep, std::vector<std::string_view>& out) {
    for (std::size_t at = 0; at < s.size();) {
        std::size_t e = s.find(sep, at);
        if (e == std::string_view::npos) e = s.size();
        if (e > at) out.push_back(s.substr(at, e - at));
        at = e + sep.size();
    }
}

// codeEdit's token runs (identifier / space / other), as its tokenizer splits a line for colouring
void tokens(std::string_view l, std::vector<std::uint32_t>& cuts) {
    cuts.clear();
    auto cls = [](char c) { return c == ' ' ? 0 : (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (c & 0x80)) ? 1 : 2; };
    for (std::size_t i = 0; i < l.size();) {
        cuts.push_back(std::uint32_t(i));
        const int c = cls(l[i]);
        while (i < l.size() && cls(l[i]) == c) ++i;
    }
    cuts.push_back(std::uint32_t(l.size()));
}

struct Bench {
    const Font& font;
    std::vector<std::string_view> paras, lines;
    TextLayout tl;
    std::vector<ShapedGlyph> scratch;
    std::vector<std::uint32_t> cuts;
    std::size_t sink = 0;

    explicit Bench(const Font& f) : font(f) {} // not an aggregate: Bench b{font} (-Wmissing-field-initializers)

    void layoutParas(std::size_t first, std::size_t count, float width) {
        TextLayoutDesc d;
        d.pixelSize = 15.f;
        d.maxWidth = width;
        for (std::size_t k = first; k < first + count && k < paras.size(); ++k) {
            layoutText(font, defaultShaper(), paras[k], d, tl);
            sink += tl.lines.size();
        }
    }
    // one codeEdit paint of 50 lines starting at `first`
    void codeFrame(std::size_t first) {
        TextLayoutDesc d;
        d.pixelSize = 14.f;
        d.maxWidth = 2000.f;
        d.elide = true;
        for (std::size_t li = first; li < first + 50 && li < lines.size(); ++li) {
            const std::string_view L = lines[li];
            tokens(L, cuts);
            float x = 0.f;
            for (std::size_t t = 0; t + 1 < cuts.size() && x < 1200.f; ++t) { // tokens up to the right edge
                const std::string_view tok = L.substr(cuts[t], cuts[t + 1] - cuts[t]);
                layoutText(font, defaultShaper(), tok, d, tl);
                x += measureText(font, tok, 14.f, scratch);
                sink += tl.glyphs.size();
            }
            sink += std::size_t(measureText(font, L.substr(0, std::min<std::size_t>(L.size(), 40)), 14.f, scratch)); // caret
        }
    }
};

struct Budgets {
    double parasCold, parasWarm, parasResize, codeFrameMean, block, uiFirst, uiResize; // ms
};
} // namespace

int main() {
    const std::string doc = makeDocument(1u << 20);
    const double mb = double(doc.size()) / (1024.0 * 1024.0);
    const ChainFont cf;
    Bench b{cf.font};
    splitBy(doc, "\n\n", b.paras);
    splitBy(doc, "\n", b.lines);
    std::printf("profile %s: document %.2f MB, %zu paragraphs, %zu lines\n", textProfileName(textProfile()), mb, b.paras.size(),
                b.lines.size());

    // where layout time goes: segmentation and shaping on their own (fresh fonts: shaping misses its cache)
    {
        std::vector<LineBreak> br;
        std::vector<std::uint8_t> lv;
        std::uint8_t para = 0;
        auto t = Clock::now();
        for (std::string_view p : b.paras) lineBreaks(p, br);
        const double lb = msSince(t);
        t = Clock::now();
        for (std::string_view p : b.paras) bidiLevels(p, BaseDirection::Auto, lv, para);
        const double bidi = msSince(t);
        t = Clock::now();
        std::size_t gc = 0;
        for (std::string_view p : b.paras) gc += graphemeCount(p);
        const double gr = msSince(t);
        const ChainFont sf;
        std::vector<ShapedGlyph> out;
        t = Clock::now();
        for (std::string_view p : b.paras) shapeText(sf.font, p, 15.f, out);
        const double sh = msSince(t);
        std::printf("parts: line breaks %.1f ms, bidi levels %.1f ms, graphemes %.1f ms (%zu), shapeText with fallback %.1f ms\n",
                    lb, bidi, gr, gc, sh);
    }

    // paragraphs (markdownView)
    auto t0 = Clock::now();
    b.layoutParas(0, b.paras.size(), 720.f);
    const double parasCold = msSince(t0);
    t0 = Clock::now();
    b.layoutParas(0, b.paras.size(), 720.f);
    const double parasWarm = msSince(t0);
    t0 = Clock::now();
    b.layoutParas(0, b.paras.size(), 560.f);
    const double parasResize = msSince(t0);
    std::printf("paragraphs: cold %.1f ms (%.1f MB/s), warm %.1f ms (%.1f MB/s), resize %.1f ms (%.1f MB/s)\n", parasCold,
                mb / parasCold * 1e3, parasWarm, mb / parasWarm * 1e3, parasResize, mb / parasResize * 1e3);

    // code frames (codeEdit), scrolled one screen at a time
    const std::size_t frames = (b.lines.size() + 49) / 50;
    double codeMax = 0.0;
    t0 = Clock::now();
    for (std::size_t f = 0; f < frames; ++f) {
        const auto f0 = Clock::now();
        b.codeFrame(f * 50);
        codeMax = std::max(codeMax, msSince(f0));
    }
    const double codeCold = msSince(t0);
    t0 = Clock::now();
    for (std::size_t f = 0; f < frames; ++f) b.codeFrame(f * 50);
    const double codeWarm = msSince(t0);
    std::printf("code frames (%zu x 50 lines): cold %.1f ms (%.3f ms/frame, max %.3f), warm %.1f ms (%.3f ms/frame), %.1f MB/s\n",
                frames, codeCold, codeCold / double(frames), codeMax, codeWarm, codeWarm / double(frames), mb / codeWarm * 1e3);

    // the whole document as one block
    TextLayoutDesc bd;
    bd.pixelSize = 15.f;
    bd.maxWidth = 720.f;
    TextLayout big;
    t0 = Clock::now();
    layoutText(cf.font, defaultShaper(), doc, bd, big);
    const double block = msSince(t0);
    t0 = Clock::now();
    layoutText(cf.font, defaultShaper(), doc, bd, big);
    const double block2 = msSince(t0);
    std::printf("one block: %.1f ms then %.1f ms (%.1f MB/s), %zu lines, %zu glyphs\n", block, block2, mb / block2 * 1e3,
                big.lines.size(), big.glyphs.size());

    // markdownView through the UI (its own fonts, so the text caches start cold)
    double uiFirst = 0.0, uiResize = 0.0;
    {
        const ChainFont uf;
        ui::UiContext ctx({.viewport = {900, 700}, .font = &uf.font, .reserveNodes = 16384});
        ui::widgets::install(ctx);
        const ui::NodeHandle v = ui::widgets::markdownView(ctx, ctx.root(), doc);
        (void)v;
        t0 = Clock::now();
        ctx.update();
        uiFirst = msSince(t0);
        ctx.setViewport({700, 700});
        t0 = Clock::now();
        ctx.update();
        uiResize = msSince(t0);
        std::printf("ui markdownView: %u nodes, first update %.1f ms, resize %.1f ms\n", ctx.nodeCount(), uiFirst, uiResize);
    }

    // steady state: one screen of paragraphs and one code frame, laid out again, allocate nothing
    for (int i = 0; i < 3; ++i) {
        b.layoutParas(100, 40, 720.f);
        b.codeFrame(1000);
    }
    const ShapeCacheStats s0 = shapeCacheStats(cf.font);
    g_allocs = 0;
    g_counting = true;
    for (int i = 0; i < 10; ++i) {
        b.layoutParas(100, 40, 720.f);
        b.codeFrame(1000);
    }
    g_counting = false;
    const ShapeCacheStats s1 = shapeCacheStats(cf.font);
    std::printf("steady window: %ld allocations, run cache hits +%llu misses +%llu (sink %zu)\n", g_allocs.load(),
                (unsigned long long)(s1.hits - s0.hits), (unsigned long long)(s1.misses - s0.misses), b.sink);
    UI_CHECK_EQ(g_allocs.load(), 0L);
    if (textProfile() == TextProfile::Full) UI_CHECK(s1.misses == s0.misses);

#ifdef NDEBUG
    // ~4x the measured times on the reference machine (see the header); FULL shapes through HarfBuzz + SheenBidi.
    // Measured (MINIMAL / FULL, ms): paragraphs cold 60 / 340-500, warm 51 / 390-560, resize 52 / 370-400, code frame
    // 0.45 / 1.4-2.2, one block 57 / 360-500, ui first 125 / 500-640, ui resize 61 / 380-570. A code frame stays within
    // half a 60 Hz frame even at the budget.
    const Budgets bud = textProfile() == TextProfile::Full ? Budgets{2000, 2000, 2000, 8.0, 2000, 2500, 2000}
                                                           : Budgets{250, 250, 250, 2.0, 250, 500, 250};
    UI_CHECK(parasCold <= bud.parasCold);
    UI_CHECK(parasWarm <= bud.parasWarm);
    UI_CHECK(parasResize <= bud.parasResize);
    UI_CHECK(codeWarm / double(frames) <= bud.codeFrameMean);
    UI_CHECK(block2 <= bud.block);
    UI_CHECK(uiFirst <= bud.uiFirst);
    UI_CHECK(uiResize <= bud.uiResize);
#else
    std::printf("(debug build: budgets not enforced)\n");
#endif
    return finish("fuse_text_bench_layout");
}
