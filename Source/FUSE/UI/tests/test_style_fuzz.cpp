// Gate fuse_ui_style_fuzz: the style sheet parser and the value parsers never crash, hang or read out of bounds on
// malformed input, and whatever they accept can be cooked, loaded, matched and painted.
//   * truncation: every prefix of a sheet that uses every feature (selectors, at-rules, nesting, keyframes, the
//     effect properties)
//   * mutation: bytes of that sheet replaced by structural characters, deleted or duplicated (seeded, deterministic)
//   * generated soup: random sequences of CSS tokens, deep nesting, long selectors, huge numbers
//   * value parsers and parseKeyframes fed the same way; UiContext::setCss / animateCss with garbage
// A parsed sheet (or one with diagnostics) is then applied to a small tree and updated for a few frames; a run under
// ASan / UBSan turns any bad access here into a failure. FUSE_UI_FUZZ_ITERATIONS scales the mutation count.
#include "fuse/ui/doc/cook.hpp"
#include "test_util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
struct Rng {
    u32 s;
    u32 next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    u32 below(u32 n) { return n ? next() % n : 0; }
};

const char* kSeedSheet = R"css(
/* every feature once */
:root { --accent: #3af; --gap: 4px; }
@layer base, theme.dark;
@layer base { panel { gap: var(--gap); color: var(--missing, red); } }
panel.a > label:first-child + label ~ .c:not(.d, #e):is(.f, .g):where(.h):has(> .i, + .j) { padding: 1px 2px; }
[data-x], [data-y=v], [data-z~="a b" i], a[b|=c][d^=e][f$=g][h*=i] { margin: 1px; }
label:nth-child(2n+1):nth-last-child(-n+3):only-child:empty { border: 1px solid red; }
.n { opacity: 0.5; .o { opacity: 0.25; } &.p { opacity: 0.75; } > .q, & + .r { opacity: 0.1; } @media (min-width: 100px) { gap: 2px; } }
@media screen and (min-width: 600px), not print and (orientation: landscape) { .m { opacity: 0.5; } }
@media (400px <= width < 800px) and ((prefers-color-scheme: dark) or (hover: none)) { .m2 { gap: 1px; } }
@container card (min-width: 300px) and (aspect-ratio > 1 / 2) { .t { opacity: 0.5; } }
@supports (transform: rotate(1deg)) and (not (bogus: 1)) or selector(a > b) { .s { gap: 1px; } }
@scope (.card) to (.content, .x > .y) { :scope > .img { opacity: 0.5; } }
@keyframes spin { from { transform: rotate(0deg); } 33.3%, 66% { opacity: 0.5; animation-timing-function: ease-in; } to { transform: rotate(1turn) scale(2, 3); } }
.fx { transform: translate(-50%, 10px) rotate(45deg) skewX(10deg) matrix(1, 0, 0, 1, 2, 3); transform-origin: left 25%;
      filter: blur(2px) brightness(120%) drop-shadow(1px 2px 3px rgba(0, 0, 0, 0.5)) hue-rotate(0.5turn);
      clip-path: polygon(evenodd, 0 0, 100% 0, 50% 100%); mask: linear-gradient(to top left, black, transparent 60%) luminance;
      background: radial-gradient(circle, #fff, #000 50%, transparent); animation: spin 2s ease-in-out 100ms infinite alternate both paused, none;
      position: sticky; inset: 1px 2px 3px 4px; z-index: -2; aspect-ratio: 16 / 9; container: card / inline-size;
      transition: transform 0.2s ease, filter 1s linear 50ms; grid-template-columns: subgrid; box-shadow: 1px 2px 3px #000; }
.w { clip-path: circle(40% at right 10px); mask-image: radial-gradient(black 30%, transparent); background: linear-gradient(45deg, red, blue); }
)css";

const char* kTokens[] = {"{", "}", "{", "}", ";", ":", ",", "(", ")", "[", "]", "@media", "@container", "@layer", "@scope", "@supports", "@keyframes",
                         "@import", "@", "&", ">", "+", "~", "*", "#", ".", ":not(", ":is(", ":has(", ":where(", ":nth-child(", ":root", ":scope",
                         "::", "panel", "label", ".a", "#b", "[x=y]", "[x", "from", "to", "50%", "-", "1e9999", "999999999999999999999px", "-0",
                         "0.0.0", "var(", "var(--a)", "--a", "calc(", "rgb(", "#12345", "linear-gradient(", "radial-gradient(", "polygon(",
                         "circle(", "inset(", "rotate(", "translate(", "matrix(", "blur(", "drop-shadow(", "infinite", "none", "auto", "and", "or",
                         "not", "(min-width: 1px)", "(width <", "<=", "width", "opacity", "transform", "filter", "clip-path", "mask", "animation",
                         "background", "position", "z-index", "aspect-ratio", "container", "inset", "transition", "!important", "\"", "'", "\\",
                         "/*", "*/", "\n", " ", " ", "\t", "\x01", "\xff", "an+b", "2n+", "odd", "deg", "turn", "%", "px"};

int g_parsed = 0, g_failed = 0;

/// Parses `css`; whatever comes back is cooked, loaded and used.
void exercise(const std::string& css, bool use) {
    std::vector<StyleDiagnostic> diags;
    auto sheet = StyleSheet::parse(css, &diags);
    if (!sheet) {
        ++g_failed;
        return;
    }
    ++g_parsed;
    if (!use) return;
    auto cooked = doc::cookStyle(css);
    UI_CHECK(cooked.has_value()); // what parses, cooks
    std::expected<StyleSheet, std::string> loaded = cooked ? doc::loadCookedStyle(*cooked) : std::unexpected(std::string("-"));
    UI_CHECK(loaded.has_value()); // and loads again
    UiContext ui({.viewport = {320, 240}});
    ui.styles().addSheet(loaded ? std::move(*loaded) : std::move(*sheet));
    const u32 panel = ui.registry().find("panel"), label = ui.registry().find("label");
    const NodeHandle a = ui.create(panel, ui.root(), "b");
    for (const char* c : {"a", "n", "p", "fx", "card", "m", "w"}) ui.addClass(a, c);
    ui.setAttribute(a, "data-x", "a b");
    ui.setAttribute(a, "x", "y");
    NodeHandle kid;
    for (int k = 0; k < 4; ++k) {
        kid = ui.create(k % 2 ? panel : label, a);
        for (const char* c : {"c", "f", "h", "o", "q", "r", "img", "t", "s", "i", "j"}) ui.addClass(kid, c);
        if (k == 1) ui.setText(kid, "x");
    }
    ui.create(label, kid);
    ui.update(0.f);
    ui.setViewport({700, 500});
    ui.setState(a, kStateHover, true);
    ui.update(0.05f);
    ui.pointerMove({20, 20});
    (void)ui.hitTest({100, 100});
    ui.scrollTo(a, {0, 10});
    ui.update(0.5f);
    Image img;
    img.resize(64, 64, 0);
    CpuRasterizer().render(ui.drawList(), img, {0, 0, 64, 64});
    ui.destroy(kid);
    ui.update(1.f);
}
} // namespace

int main() {
    const std::string seedSheet = kSeedSheet;
    {
        std::vector<StyleDiagnostic> diags;
        auto ok = StyleSheet::parse(seedSheet, &diags);
        for (const auto& d : diags) std::fprintf(stderr, "  seed diagnostic line %u: %s\n", d.line, d.message.c_str());
        UI_CHECK(ok.has_value() && diags.empty()); // the seed itself is clean: the mutations start from valid input
        if (ok) std::printf("seed sheet: %zu rules, %zu conditions, %zu layers, %zu keyframes\n", ok->rules().size(), ok->conditions().size(), ok->layers().size(), ok->keyframes().size());
        UI_CHECK(ok && ok->rules().size() >= 20 && ok->keyframes().size() == 1 && ok->conditions().size() >= 4 && ok->layers().size() == 3);
        exercise(seedSheet, true);
    }
    int iterations = 3000;
    if (const char* env = std::getenv("FUSE_UI_FUZZ_ITERATIONS")) iterations = std::max(1, std::atoi(env));

    // ---- truncation: every prefix ----
    for (std::size_t n = 0; n < seedSheet.size(); ++n) exercise(seedSheet.substr(0, n), n % 9 == 0);

    // ---- mutation ----
    Rng rng{0x9E3779B9u};
    static const char kStructural[] = "{}()[];:,@&>+~*#.\"'\\/!-%0n \n";
    for (int it = 0; it < iterations; ++it) {
        std::string s = seedSheet;
        const u32 edits = 1 + rng.below(6);
        for (u32 e = 0; e < edits && !s.empty(); ++e) {
            const std::size_t at = rng.below(u32(s.size()));
            switch (rng.below(5)) {
            case 0: s[at] = kStructural[rng.below(sizeof kStructural - 1)]; break;
            case 1: s.erase(at, 1 + rng.below(12)); break;
            case 2: s.insert(at, s.substr(rng.below(u32(s.size())), 1 + rng.below(40))); break;
            case 3: s.insert(at, kTokens[rng.below(u32(std::size(kTokens)))]); break;
            default: s[at] = char(rng.below(256)); break;
            }
        }
        exercise(s, it % 6 == 0);
    }

    // ---- token soup, deep nesting, long selectors ----
    for (int it = 0; it < iterations; ++it) {
        std::string s;
        const u32 count = 1 + rng.below(60);
        for (u32 k = 0; k < count; ++k) {
            s += kTokens[rng.below(u32(std::size(kTokens)))];
            if (rng.below(3) == 0) s += ' ';
        }
        exercise(s, it % 8 == 0);
    }
    {
        std::string deep, wide = ".a", nest, parens = "x:not(";
        for (int k = 0; k < 400; ++k) deep += ".a { ";
        deep += "gap: 1px; ";
        exercise(deep, false); // unbalanced
        for (int k = 0; k < 400; ++k) deep += "} ";
        exercise(deep, true);
        for (int k = 0; k < 3000; ++k) wide += k % 3 ? " > .b" : " .c";
        exercise(wide + " { gap: 1px; }", true);
        for (int k = 0; k < 200; ++k) nest += "@media (min-width: 1px) { ";
        nest += ".x { gap: 1px; } ";
        for (int k = 0; k < 200; ++k) nest += "} ";
        exercise(nest, true);
        for (int k = 0; k < 300; ++k) parens += ":not(";
        exercise(parens + " { gap: 1px; }", false);
        for (int k = 0; k < 301; ++k) parens += ")";
        exercise(parens + " { gap: 1px; }", true);
        exercise(std::string(100000, '{'), false);
        exercise(std::string(100000, '}'), false);
        exercise(std::string(50000, '(') + "{}", false);
        exercise("a{" + std::string(200000, ';') + "}", true);
        exercise("@keyframes k {" + std::string(5000, '%') + "}", true);
        exercise(".a{animation:" + std::string(3000, ',') + "}", true);
        exercise(std::string("\0\0{\0}", 5), true);
    }

    // ---- value parsers ----
    {
        const Prop props[] = {Prop::Transform, Prop::Filter, Prop::ClipPath, Prop::MaskImage, Prop::BackgroundImage, Prop::AnimationName,
                              Prop::AnimationDuration, Prop::AnimationIterationCount, Prop::ZIndex, Prop::AspectRatio, Prop::TransformOriginX,
                              Prop::ContainerName, Prop::Position, Prop::Transition, Prop::GridColumns, Prop::Width, Prop::Background, Prop::FontFamily};
        const char* values[] = {"translate(-50%, 10px) rotate(45deg) skewX(10deg) matrix(1, 0, 0, 1, 2, 3)", "blur(2px) drop-shadow(1px 2px 3px rgba(0, 0, 0, 0.5))",
                                "polygon(evenodd, 0 0, 100% 0, 50% 100%)", "circle(40% at right 10px)", "linear-gradient(to top left, black, transparent 60%)",
                                "radial-gradient(circle, #fff, #000 50%, transparent)", "spin, none, a", "1s, 200ms", "infinite, 2.5", "16 / 9"};
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle n = ui.create(ui.registry().find("panel"), ui.root());
        for (int it = 0; it < iterations * 4; ++it) {
            std::string s = values[rng.below(u32(std::size(values)))];
            const u32 edits = rng.below(5);
            for (u32 e = 0; e < edits && !s.empty(); ++e) {
                const std::size_t at = rng.below(u32(s.size()));
                switch (rng.below(4)) {
                case 0: s[at] = kStructural[rng.below(sizeof kStructural - 1)]; break;
                case 1: s.erase(at, 1 + rng.below(8)); break;
                case 2: s.insert(at, kTokens[rng.below(u32(std::size(kTokens)))]); break;
                default: s[at] = char(rng.below(256)); break;
                }
            }
            const Prop p = props[rng.below(u32(std::size(props)))];
            PropValue v;
            if (parsePropValue(p, s, v)) { // an accepted value applies, reads back and applies again unchanged
                ComputedStyle a, b;
                applyProp(a, p, v);
                applyProp(b, p, readProp(a, p));
                if (p != Prop::Transition && p != Prop::AnimationDuration && p != Prop::AnimationIterationCount) UI_CHECK(sameStyleProp(a, b, p));
                ComputedStyle mid;
                lerpStyleProp(mid, ComputedStyle{}, a, p, 0.37f);
            }
            if (it % 16 == 0) {
                ui.setCss(n, propInfo(p).cssName, s);
                KeyframesRule rule;
                parseKeyframes(s, rule);
                parseKeyframes("from { " + std::string(propInfo(p).cssName) + ": " + s + " } to { opacity: 0 }", rule);
                AnimationTiming t;
                t.duration = f32(rng.below(3));
                t.iterations = f32(rng.below(4)) * 0.75f;
                t.delay = f32(rng.below(3)) - 1.f;
                const AnimationHandle h = ui.animateCss(n, "from { " + std::string(propInfo(p).cssName) + ": " + s + " } 50% { opacity: 0.5 } to { filter: blur(3px) }", t);
                ui.update(0.4f);
                if (rng.below(3) == 0) ui.cancelAnimation(h);
                else if (rng.below(2)) ui.reverseAnimation(h);
            }
        }
        ui.update(10.f);
    }
    std::printf("style fuzz: %d inputs parsed, %d refused\n", g_parsed, g_failed);
    UI_CHECK(g_parsed > 100 && g_failed > 100); // both paths were taken
    return finish("fuse_ui_style_fuzz");
}
