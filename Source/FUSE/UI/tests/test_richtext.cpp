// fuse_ui_extras_richtext*: the rich text editor (Extras/richtext.hpp) on main's text stack. `--gate=<name>`:
//   model    headless editor: character / block formatting, lists, HTML and Markdown round trips, layout (wrapping,
//            caret, hit testing, justify) with a fixed-advance measure
//   undo     undo / redo through the Extras UndoStack: command names, typing runs merge, a stack shared with the
//            application, an editor destroyed while its commands are still on the shared stack
//   widget   the `richtextedit` widget: typing, shortcuts, clipboard, links, read-only; bold / italic fragments are
//            measured and painted in the family's bold / italic face (advance and pixels differ from regular)
//   view     one model: a document written as `richText` view markup shows the same text, links and faces in the
//            read-only view, and view markup read back gives the same character formats
//   golden   the editor rendered against tests/golden/extras_richtext.png (MINIMAL text profile)
#include "test_util.hpp"

#include "fuse/ui/extras/richtext.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::rich;
namespace w = fuse::ui::widgets;

namespace {
void step(UiContext& ui) { ui.update(1.f / 60.f); }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

void gateModel() {
    RichTextEditor ed;
    ed.insertText("Hello world");
    ed.setCursor(6); ed.setCursor(11, true); // select "world"
    ed.toggleBold();
    UI_CHECK(ed.doc.formatOfByte(6).bold && !ed.doc.formatOfByte(5).bold);
    ed.toggleBold();
    UI_CHECK(!ed.doc.formatOfByte(6).bold);
    ed.toggleBold();
    ed.setColor(Color::rgb(0xFF0000));
    ed.setCursor(11);
    ed.newParagraph();
    ed.setList(ListKind::Ordered);
    ed.typing = {};
    ed.insertText("one");
    ed.newParagraph();
    ed.insertText("two");
    UI_CHECK(ed.doc.blockCount() == 3 && ed.doc.blockFormat(2).list == ListKind::Ordered && ed.doc.listNumber(2) == 2);
    ed.newParagraph();
    ed.newParagraph(); // empty item leaves the list
    UI_CHECK(ed.doc.blockCount() == 4 && ed.doc.blockFormat(3).list == ListKind::None);
    ed.setCursor(0);
    ed.setHeading(1);
    ed.setAlign(rich::Align::Center);
    UI_CHECK(ed.doc.plainText() == "Hello world\none\ntwo\n");
    ed.setCursor(ed.doc.size());
    ed.insertText("abcdef");
    // size, italic, underline, strike on a selection
    ed.setCursor(ed.doc.size() - 3, false);
    ed.setCursor(ed.doc.size(), true);
    ed.setSize(22.f);
    ed.toggleItalic();
    ed.toggleUnderline();
    ed.toggleStrike();
    const CharFormat tail = ed.doc.formatOfByte(ed.doc.size() - 1);
    UI_CHECK(tail.size == 22.f && tail.italic && tail.underline && tail.strike && !ed.doc.formatOfByte(ed.doc.size() - 4).italic);
    ed.toggleItalic(); ed.toggleUnderline(); ed.toggleStrike();
    ed.setSize(0.f);
    // HTML round trip
    const std::string html = ed.doc.toHtml();
    UI_CHECK(html.find("<h1 style=\"text-align:center;\">Hello <b><span style=\"color:#ff0000;\">world</span></b></h1>") != std::string::npos);
    UI_CHECK(html.find("<ol>\n<li>one</li>\n<li>two</li>\n</ol>") != std::string::npos);
    RichDocument h2;
    h2.fromHtml(html);
    UI_CHECK(h2 == ed.doc);
    // foreign HTML
    RichDocument fh;
    fh.fromHtml("<html><head><style>p{}</style></head><body><p>A &amp; <a href=\"https://x.org\">link</a><br>next <span style=\"font-weight:700; font-size:12pt; text-decoration: underline line-through\">styled</span></p>\n<ul><li>x<ul><li>y</li></ul></li></ul></body></html>");
    UI_CHECK(fh.plainText() == "A & link\nnext styled\nx\ny");
    UI_CHECK(fh.formatOfByte(4).link == "https://x.org");
    const CharFormat st = fh.formatOfByte(fh.plainText().find("styled"));
    UI_CHECK(st.bold && st.underline && st.strike && std::abs(st.size - 16.f) < 0.01f);
    UI_CHECK(fh.blockFormat(3).list == ListKind::Bullet && fh.blockFormat(3).indent == 1);
    // Markdown round trip
    const std::string md = ed.doc.toMarkdown();
    UI_CHECK(md.starts_with("# Hello **world**\n\n1. one\n2. two\n\n"));
    RichDocument m2;
    m2.fromMarkdown("## Title\n\nSome **bold _both_** and ~~gone~~ [site](https://a.b) <u>u</u> \\*lit\\*\n\n- a\n  - b\n3. c\n");
    UI_CHECK(m2.plainText() == "Title\nSome bold both and gone site u *lit*\na\nb\nc");
    UI_CHECK(m2.blockFormat(0).heading == 2 && m2.blockFormat(3).indent == 1 && m2.blockFormat(4).list == ListKind::Ordered);
    const std::size_t both = m2.plainText().find("both");
    UI_CHECK(m2.formatOfByte(both).bold && m2.formatOfByte(both).italic && !m2.formatOfByte(both - 2).italic);
    UI_CHECK(m2.formatOfByte(m2.plainText().find("site")).link == "https://a.b" && m2.formatOfByte(m2.plainText().find("gone")).strike);
    RichDocument m3;
    m3.fromMarkdown(m2.toMarkdown());
    UI_CHECK(m3 == m2);
    // layout: wrapping, caret and hit testing with a fixed-advance measure (bold is 20% wider)
    const Measure meas = [](std::string_view s, f32 px, const CharFormat& f) { return f32(s.size()) * px * (f.bold ? 0.6f : 0.5f); };
    auto lh = [](f32 px) { return px * 1.25f; };
    RichDocument ld;
    ld.setPlainText("aaaa bbbb cccc\nxxxxxxxxxxxxxxxxxxxx");
    const RichLayout L = layout(ld, 100.f, 10.f, meas, lh); // 20 chars per line
    UI_CHECK(L.lines.size() == 2); // 20 x fit exactly
    const RichLayout L2 = layout(ld, 50.f, 10.f, meas, lh); // 10 chars per line
    UI_CHECK(L2.lines.size() == 2 + 2 && L2.lines[0].b == 10 && L2.lines[2].b - L2.lines[2].a == 10);
    UI_CHECK(L2.hitTest({26.f, 2.f}, ld, meas) == 5 && L2.caretRect(5, ld, meas).x == 25.f);
    UI_CHECK(L2.lineOf(10) == 1 && L2.hitTest({0.f, 13.f}, ld, meas) == 10);
    // bold fragments take their own advance: the caret after a bold word is further right, a bold line wraps earlier
    ld.modifyFormat(0, 4, [](CharFormat& f) { f.bold = true; });
    const RichLayout Lb = layout(ld, 100.f, 10.f, meas, lh);
    UI_CHECK(Lb.caretRect(4, ld, meas).x == 24.f && Lb.caretRect(5, ld, meas).x == 29.f);
    RichDocument hd; // a heading is bold: measured in the bold face
    hd.setPlainText("aaaaaaaaaa");
    BlockFormat hb;
    hb.heading = 5; // scale 1.0
    hd.setBlockFormat(0, hb);
    const RichLayout Lh = layout(hd, 0.f, 10.f, meas, lh);
    UI_CHECK(Lh.lines.size() == 1 && std::abs(Lh.lines[0].width - 60.f) < 0.01f && Lh.lines[0].frags[0].fmt.bold);
    {   // justify: inner lines fill the width exactly, the paragraph's last line stays left
        RichDocument jd;
        jd.fromHtml("<p style=\"text-align:justify\">aa bb cc dd ee ff gg</p>");
        const Measure m = [](std::string_view s, f32, const CharFormat&) { return f32(s.size()) * 10.f; };
        const auto J = layout(jd, 75.f, 14.f, m, [](f32) { return 16.f; });
        UI_CHECK(J.lines.size() >= 2);
        if (J.lines.size() >= 2) {
            const auto& l0 = J.lines[0];
            UI_CHECK(l0.frags.size() >= 2);
            const auto& last = l0.frags.back();
            f32 endX = last.x + last.w;
            for (std::size_t k = last.b; k > last.a && jd.plainText()[k - 1] == ' '; --k) endX -= 10.f;
            UI_CHECK(std::abs(endX - (l0.x + 75.f)) < 0.5f);
            const auto& lz = J.lines.back();
            UI_CHECK(lz.frags.back().x + lz.frags.back().w <= 75.f);
            UI_CHECK(std::abs(lz.frags.front().x - lz.x) < 0.01f);
        }
    }
}

void gateUndo() {
    RichTextEditor ed;
    ed.insertText("Hello");
    const RichDocument before = ed.doc;
    UI_CHECK(ed.canUndo() && !ed.canRedo() && ed.undoStack().count() == 1 && ed.undoStack().undoText() == "Typing");
    ed.insertText("abc");
    ed.insertText("def"); // one typing run with "Hello": still one command
    UI_CHECK_EQ(ed.undoStack().count(), 1);
    ed.setCursor(ed.doc.size()); // a cursor move ends the run
    ed.insertText("ghi");
    UI_CHECK_EQ(ed.undoStack().count(), 2);
    UI_CHECK(ed.undo());
    UI_CHECK(ed.doc.plainText() == "Helloabcdef" && ed.cursor == 11);
    UI_CHECK(ed.redo());
    UI_CHECK(ed.doc.plainText() == "Helloabcdefghi" && ed.canUndo() && !ed.canRedo());
    ed.backspace(); ed.backspace();
    UI_CHECK(ed.doc.plainText() == "Helloabcdefg" && ed.undoStack().undoText() == "Delete");
    UI_CHECK(ed.undo());
    UI_CHECK(ed.doc.plainText() == "Helloabcdefghi");
    // formatting and paragraph commands, named; undo restores formats and the selection
    ed.setCursor(0); ed.setCursor(5, true);
    ed.toggleBold();
    UI_CHECK(ed.undoStack().undoText() == "Format" && ed.doc.formatOfByte(0).bold);
    ed.setAlign(rich::Align::Right);
    UI_CHECK(ed.undoStack().undoText() == "Paragraph" && ed.doc.blockFormat(0).align == rich::Align::Right);
    ed.undo();
    UI_CHECK(ed.doc.blockFormat(0).align == rich::Align::Left && ed.doc.formatOfByte(0).bold);
    ed.undo();
    UI_CHECK(!ed.doc.formatOfByte(0).bold && ed.selStart() == 0 && ed.selEnd() == 5);
    while (ed.undo()) {}
    UI_CHECK(ed.doc.plainText().empty() && !ed.canUndo() && ed.canRedo());
    ed.insertText("x"); // a new edit drops the redo tail
    UI_CHECK(!ed.canRedo());
    ed.resetHistory();
    UI_CHECK(!ed.canUndo() && ed.undoStack().count() == 0);

    // a stack shared with the application: its commands and the editor's interleave on one history
    UndoStack app;
    int value = 0;
    {
        RichTextEditor shared;
        shared.setUndoStack(&app);
        shared.insertText("doc");
        app.push("Set value", [&] { value = 7; }, [&] { value = 0; });
        shared.setCursor(3);
        shared.insertText("ument");
        UI_CHECK(app.count() == 3 && value == 7 && shared.doc.plainText() == "document");
        UI_CHECK(app.undoText() == "Typing");
        app.undo(); // through the application's stack, not the editor
        UI_CHECK(shared.doc.plainText() == "doc" && shared.cursor == 3);
        app.undo();
        UI_CHECK(value == 0 && shared.doc.plainText() == "doc");
        UI_CHECK(shared.undo()); // the editor's undo drives the same stack
        UI_CHECK(shared.doc.plainText().empty() && !app.canUndo());
        app.redo(); app.redo(); app.redo();
        UI_CHECK(value == 7 && shared.doc.plainText() == "document");
        // replacing the document makes the older commands no-ops instead of resurrecting the old text
        shared.doc.setPlainText("fresh");
        shared.resetHistory();
        app.undo();
        UI_CHECK(shared.doc.plainText() == "fresh");
        app.redo();
    }
    // the editor is gone, its commands are still on the application's stack: undo / redo must not touch it
    app.undo();
    app.undo();
    UI_CHECK_EQ(value, 0);
    app.redo();
    UI_CHECK_EQ(value, 7);
}

struct Screen {
    UiContext ui{{.viewport = {640, 420}, .font = &font()}};
    Screen() { w::install(ui); step(ui); }
};
/// Lit pixels of `img` inside `r`.
int inkIn(const Image& img, Rect r, u32 bg) {
    int n = 0;
    for (int y = std::max(0, int(r.y)); y < std::min(img.height, int(r.bottom())); ++y)
        for (int x = std::max(0, int(r.x)); x < std::min(img.width, int(r.right())); ++x)
            n += img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] != bg;
    return n;
}

void gateWidget() {
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle te = w::richTextEdit(ui, ui.root());
    ui.setCss(te, "width", "400px");
    ui.setCss(te, "height", "200px");
    static std::string changed, link;
    changed.clear();
    link.clear();
    ui.connect(te, SignalKind::TextChanged, [](UiContext&, NodeHandle, const SignalArgs& a) { changed = a.text; });
    ui.connect(te, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { link = a.text; });
    step(ui);
    click(ui, ctr(ui.rect(te)));
    UI_CHECK(ui.focused() == te);
    ui.textInput("Hi ");
    ui.keyDown(Key::B, kModCtrl);
    ui.textInput("there");
    UI_CHECK(changed == "Hi there");
    RichTextEditor* re = w::richEditor(ui, te);
    UI_CHECK(re && re->doc.formatOfByte(4).bold && !re->doc.formatOfByte(1).bold);
    ui.keyDown(Key::Home, kModShift);
    ui.keyDown(Key::U, kModCtrl);
    UI_CHECK(re->doc.formatOfByte(0).underline && re->doc.formatOfByte(7).underline);
    ui.keyDown(Key::C, kModCtrl);
    ui.keyDown(Key::End);
    ui.keyDown(Key::Enter);
    ui.keyDown(Key::V, kModCtrl);
    UI_CHECK(changed == "Hi there\nHi there");
    ui.keyDown(Key::Z, kModCtrl);
    UI_CHECK(changed == "Hi there\n");
    ui.keyDown(Key::Y, kModCtrl);
    UI_CHECK(changed == "Hi there\nHi there");
    ui.keyDown(Key::Up);
    UI_CHECK(re->doc.blockAt(re->cursor) == 0);

    // real faces: a bold / italic fragment is laid out and painted in the family's bold / italic face
    const ComputedStyle st = ui.computed(te);
    const fuse::text::Font* regular = ui.fontFor(std::span<const u32>(st.fontFamily), 400, false);
    const fuse::text::Font* bold = ui.fontFor(std::span<const u32>(st.fontFamily), 700, false);
    const fuse::text::Font* italic = ui.fontFor(std::span<const u32>(st.fontFamily), 400, true);
    UI_CHECK(regular && bold && italic && bold != regular && italic != regular);
    w::setRichHtml(ui, te, "<p>mmmmmmmmmm</p><p><b>mmmmmmmmmm</b></p><p><i>mmmmmmmmmm</i></p>");
    step(ui);
    const f32 px = st.fontSize;
    const f32 wr = ui.measureString(*regular, "mmmmmmmmmm", px), wb = ui.measureString(*bold, "mmmmmmmmmm", px);
    const Rect cr0 = ui.contentRect(te);
    // click right of each line's end puts the caret at the line end; its x is the line's advance in that face
    const f32 lineH = std::ceil(regular->metrics(px).lineHeight());
    re->setCursor(10);
    step(ui);
    Image img;
    render(ui, img);
    const u32 bg = img.pixels[std::size_t(int(cr0.y) + 2) * std::size_t(img.width) + std::size_t(int(cr0.right()) - 4)];
    const int inkRegular = inkIn(img, {cr0.x, cr0.y, wr + 4.f, lineH}, bg);
    const int inkBold = inkIn(img, {cr0.x, cr0.y + lineH, wb + 4.f, lineH}, bg);
    std::printf("  advance of 10 m: regular %.1f, bold %.1f; ink regular %d, bold %d\n", double(wr), double(wb), inkRegular, inkBold);
    UI_CHECK(inkRegular > 100 && inkBold > inkRegular * 11 / 10); // heavier strokes, not a double strike of the same glyphs
    if (wb != wr) { // a real bold face has its own advances: hit testing follows them
        click(ui, {cr0.x + wb - 1.f, cr0.y + lineH * 1.5f});
        UI_CHECK_EQ(re->cursor, std::size_t(21)); // end of the bold line (11 + 10)
    }

    w::setRichHtml(ui, te, "<p>Go <a href=\"https://fuse.dev\">here</a> now</p><p>second</p>");
    step(ui);
    const Rect cr = ui.contentRect(te);
    const f32 hx = cr.x + ui.measureString("Go h", ui.computed(te).fontSize);
    ui.setModifiers(kModCtrl);
    click(ui, {hx, cr.y + 6.f});
    ui.setModifiers(0);
    UI_CHECK(link == "https://fuse.dev");
    click(ui, {cr.x + 2.f, cr.y + ui.font()->metrics(ui.computed(te).fontSize).lineHeight() * 1.5f});
    UI_CHECK(re->cursor == re->doc.blockStart(1));
    w::setRichMarkdown(ui, te, "# Big\n\n- **b** _i_ ~~s~~ <u>u</u> [l](https://l)\n");
    UI_CHECK(w::richMarkdown(ui, te) == "# Big\n\n- **b** _i_ ~~s~~ <u>u</u> [l](https://l)\n");
    step(ui);
    ui.setFocus(te, FocusReason::Programmatic);
    re->selectAll();
    ui.invalidate(te);
    step(ui); // paints selection, list marker, heading, decorations and caret
    w::setRichReadOnly(ui, te, true);
    link.clear();
    ui.textInput("x");
    UI_CHECK(re->doc.plainText() == "Big\nb i s u l");
    // theme: the class's rules follow the theme tokens
    UI_CHECK(w::setTheme(ui, w::ThemeVariant::Light).has_value());
    step(ui);
    Color field;
    UI_CHECK(w::themeTokenColor({.variant = w::ThemeVariant::Light}, "--field", field));
    UI_CHECK(ui.computed(te).background == field);
}

void gateView() {
    Screen s;
    UiContext& ui = s.ui;
    RichDocument d;
    d.fromHtml("<h2>Title</h2><p>Plain <b>bold</b> <i>ital</i> <u>under</u> <s>gone</s> "
               "<span style=\"color:#e0703a\">red</span> <span style=\"font-size:20px\">big</span> "
               "<a href=\"https://fuse.dev\">site</a> a &lt; b</p><ul><li>item</li></ul>");
    const std::string markup = d.toRichTextMarkup(14.f);
    UI_CHECK(markup.find("<size=21><b>Title</b></size>\n") == 0); // h2 = 1.5 x 14
    UI_CHECK(markup.find("<b>bold</b>") != std::string::npos && markup.find("<a href=\"https://fuse.dev\">site</a>") != std::string::npos);
    UI_CHECK(markup.find("a &lt; b") != std::string::npos && markup.find("\n\xE2\x80\xA2 item") != std::string::npos);
    // the read-only view shows the document: same text, the link, the same faces
    const NodeHandle view = w::richText(ui, ui.root(), markup, "view");
    ui.setCss(view, "width", "600px");
    step(ui);
    UI_CHECK(w::richTextPlain(ui, view) == "Title\nPlain bold ital under gone red big site a < b\n\xE2\x80\xA2 item");
    UI_CHECK_EQ(w::richTextLinkCount(ui, view), 1u);
    UI_CHECK(w::richTextLink(ui, view, 0) == "https://fuse.dev");
    // view markup read back: the same character formats (block formats are flattened by design)
    RichDocument back;
    back.fromRichTextMarkup(markup);
    UI_CHECK(back.plainText() == "Title\nPlain bold ital under gone red big site a < b\n\xE2\x80\xA2 item");
    const std::string& t = back.plainText();
    UI_CHECK(back.formatOfByte(t.find("bold")).bold && !back.formatOfByte(t.find("Plain")).bold);
    UI_CHECK(back.formatOfByte(t.find("ital")).italic && back.formatOfByte(t.find("under")).underline && back.formatOfByte(t.find("gone")).strike);
    UI_CHECK(back.formatOfByte(t.find("red")).hasColor && back.formatOfByte(t.find("red")).color == Color::rgb(0xE0703A));
    UI_CHECK(back.formatOfByte(t.find("big")).size == 20.f && back.formatOfByte(t.find("site")).link == "https://fuse.dev");
    UI_CHECK(back.formatOfByte(0).bold && back.formatOfByte(0).size == 21.f && back.blockCount() == 3);
    // a document made of inline formats only survives the round trip exactly
    RichDocument inl;
    inl.fromHtml("<p>one <b>two <i>three</i></b></p><p><u>four</u> five</p>");
    RichDocument inl2;
    inl2.fromRichTextMarkup(inl.toRichTextMarkup());
    UI_CHECK(inl2 == inl);
    // editor -> view through the widget API; the editor and the view lay the same line out to the same width
    const NodeHandle te = w::richTextEdit(ui, ui.root());
    ui.setCss(te, "width", "600px");
    w::setRichHtml(ui, te, "<p>Same <b>bold words</b> and <i>italic words</i> here</p>");
    const NodeHandle line = w::panel(ui, ui.root());
    ui.addClass(line, "row"); // in a row the view is as wide as its one line measures
    const NodeHandle v2 = w::richText(ui, line, w::richTextMarkup(ui, te), "v2");
    step(ui);
    step(ui);
    UI_CHECK(w::richTextPlain(ui, v2) == w::richEditor(ui, te)->doc.plainText());
    const ComputedStyle st = ui.computed(te);
    const Measure m = [&](std::string_view str, f32 px, const CharFormat& f) {
        const fuse::text::Font* font = ui.fontFor(std::span<const u32>(st.fontFamily), f.bold ? 700 : 400, f.italic);
        return ui.measureString(*font, str, px);
    };
    const RichLayout L = layout(w::richEditor(ui, te)->doc, 600.f, st.fontSize, m, [](f32 px) { return px * 1.25f; });
    UI_CHECK(L.lines.size() == 1);
    std::printf("  one line: editor layout %.1f px, view %.1f px\n", double(L.lines[0].width), double(ui.rect(v2).w));
    UI_CHECK(std::abs(L.lines[0].width - ui.rect(v2).w) <= 2.f); // the view's measured width is the same run widths
}

void gateGolden(const char* outDir) {
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "padding", "10px");
    const NodeHandle te = w::richTextEdit(ui, ui.root());
    ui.setCss(te, "width", "600px");
    ui.setCss(te, "height", "380px");
    w::setRichHtml(ui, te, "<h2>Rich <i>text</i> editor</h2>"
                           "<p style=\"text-align:justify\">Justified text spreads the slack of every line but the last over its spaces; "
                           "<b>bold is the bold face</b> and <i>italic the italic face</i> of the node's family.</p>"
                           "<p>Plain, <b>bold</b>, <i>italic</i>, <b><i>both</i></b>, <u>under</u>, <s>strike</s>, "
                           "<span style=\"color:#e0703a\">colour</span>, <span style=\"font-size:22px\">big</span> and a "
                           "<a href=\"x\">link</a> wrapping across the width of the editor.</p>"
                           "<ol><li>first</li><li>second</li></ol><ul><li>bullet<ul><li>nested</li></ul></li></ul>"
                           "<p style=\"text-align:center\">centred</p><p style=\"text-align:right\">right</p>");
    ui.setFocus(te, FocusReason::Programmatic);
    w::richEditor(ui, te)->setCursor(5);
    w::richEditor(ui, te)->setCursor(16, true);
    for (int i = 0; i < 30; ++i) step(ui);
    ui.update(0.f);
    Image img;
    render(ui, img);
    const char* golden = "golden/extras_richtext.png";
    if (outDir) stbi_write_png((std::string(outDir) + "/extras_richtext.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden, img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden);
        return;
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden, &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) return;
    UI_CHECK(gw == img.width && gh == img.height);
    if (gw == img.width && gh == img.height) {
        int bad = 0;
        for (int i = 0; i < gw * gh; ++i)
            for (int c = 0; c < 4; ++c)
                if (std::abs(int(ref[i * 4 + c]) - int((img.pixels[std::size_t(i)] >> (8 * c)) & 0xFF)) > 3) { ++bad; break; }
        std::printf("richtext golden diff: %d pixels > 3\n", bad);
        UI_CHECK(bad <= gw * gh / 1000);
    }
    stbi_image_free(ref);
}
} // namespace

int main(int argc, char** argv) {
    std::string gate;
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
        else if (std::strncmp(argv[i], "--out=", 6) == 0) outDir = argv[i] + 6;
    }
    if (gate == "model") gateModel();
    else if (gate == "undo") gateUndo();
    else if (gate == "widget") gateWidget();
    else if (gate == "view") gateView();
    else if (gate == "golden") gateGolden(outDir);
    else {
        std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
        return 2;
    }
    return finish(("fuse_ui_extras_richtext_" + gate).c_str());
}
