// fuse_ui_extras_pdf_paths: PdfDocument::renderDrawList writes main's vector path commands as PDF path operators:
// a filled path (m / l / c / h + f, f* for even-odd), a stroked one (width, cap, join, dashes + S), a quadratic as the
// equivalent cubic, a path clip (W n) that brackets what it contains, and a gradient fill as the mean of its stops.
// The page content is checked as text (the content stream is not compressed) and for balanced q / Q.
#include "test_util.hpp"

#include "fuse/ui/extras/pdf.hpp"
#include "fuse/ui/path.hpp"

#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
int count(const std::string& s, std::string_view what) {
    int n = 0;
    for (std::size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}
} // namespace

int main() {
    DrawList dl;
    Path tri;
    tri.moveTo({10.f, 10.f}).lineTo({110.f, 10.f}).lineTo({60.f, 90.f}).close();
    dl.fillPath(tri, Color::rgb(0xFF0000));
    Path ring; // two contours: even-odd leaves the hole
    ring.addCircle({200.f, 50.f}, 40.f).addCircle({200.f, 50.f}, 20.f);
    dl.fillPath(ring, Color::rgb(0x00FF00, 0.5f), FillRule::EvenOdd);
    Path curve;
    curve.moveTo({10.f, 150.f}).quadTo({60.f, 100.f}, {110.f, 150.f}).cubicTo({130.f, 170.f}, {150.f, 130.f}, {170.f, 150.f});
    StrokeStyle st;
    st.width = 3.f;
    st.cap = LineCap::Round;
    st.join = LineJoin::Bevel;
    const f32 dashes[] = {6.f, 2.f};
    st.setDashes(dashes, 1.f);
    dl.strokePath(curve, st, Color::rgb(0x0000FF));
    Path clip;
    clip.addCircle({60.f, 250.f}, 30.f);
    dl.pushClipPath(clip);
    dl.rect({20.f, 210.f, 80.f, 80.f}, Color::rgb(0x808080));
    dl.popClip();
    const GradientStop stops[] = {{0.f, Color::rgb(0x000000)}, {1.f, Color::rgb(0xFFFFFF)}};
    Path box;
    box.moveTo({150.f, 210.f}).lineTo({250.f, 210.f}).lineTo({250.f, 290.f}).lineTo({150.f, 290.f}).close();
    dl.fillPath(box, Gradient::linear({150.f, 0.f}, {250.f, 0.f}, stops));

    fuse::ui::pdf::PdfDocument pdf;
    pdf.newPage({300.f, 300.f});
    pdf.renderDrawList(dl, {0.f, 0.f}, 1.f, nullptr, 0);
    const std::string out = pdf.finish();
    UI_CHECK(out.starts_with("%PDF-"));
    // the triangle: three points and a close, filled non-zero in red
    UI_CHECK(out.find("1 0 0 rg\n10 10 m\n110 10 l\n60 90 l\nh\nf\n") != std::string::npos);
    // the ring: even-odd fill, half transparent (an alpha graphics state), curves
    UI_CHECK(out.find("f*\n") != std::string::npos && out.find("/Ga10 gs\n0 1 0 rg\n") != std::string::npos);
    // the stroke: width 3, round cap (1), bevel join (2), the dash array with its offset, stroked not filled
    UI_CHECK(out.find("3 w 1 J 2 j 4 M\n[6 2] 1 d\n0 0 1 RG\n10 150 m\n") != std::string::npos);
    // quadTo (60,100) -> (110,150) from (10,150) as a cubic: control points at 2/3 towards the quad's control point
    UI_CHECK(out.find("43.33 116.67 76.67 116.67 110 150 c\n") != std::string::npos ||
             out.find("43.333 116.667 76.667 116.667 110 150 c\n") != std::string::npos);
    UI_CHECK(out.find("130 170 150 130 170 150 c\nS\n") != std::string::npos);
    // the clip: q, the circle, W n, then the rectangle inside it, then Q
    const std::size_t w = out.find("W n\n"), rect = out.find("0.502 0.502 0.502 rg");
    UI_CHECK(w != std::string::npos && rect != std::string::npos && rect > w);
    if (w != std::string::npos && rect != std::string::npos) UI_CHECK(out.find("Q\n", rect) != std::string::npos);
    // the gradient: the mean of black and white
    UI_CHECK(out.find("0.5 0.5 0.5 rg\n150 210 m\n") != std::string::npos);
    // balanced graphics states in the page content
    const std::size_t stream = out.find("stream\n"), end = out.find("endstream");
    UI_CHECK(stream != std::string::npos && end != std::string::npos);
    if (stream != std::string::npos && end != std::string::npos) {
        const std::string page = out.substr(stream, end - stream);
        UI_CHECK_EQ(count(page, "q\n") + count(page, "q "), count(page, "Q\n"));
        UI_CHECK(count(page, "\nf\n") >= 3 && count(page, "S\n") >= 1);
    }
    return finish("fuse_ui_extras_pdf_paths");
}
