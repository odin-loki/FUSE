// Rich text (QTextDocument / QTextEdit parity): a styled document model, a headless editor (cursor, selection,
// formatting, undo) with HTML / Markdown import-export, and the `richtextedit` retained widget that lays it out
// with the context font. Bold is synthesised (double strike), italic is a synthetic oblique (rich::obliqueGlyphs shears the glyph quads).
#pragma once

#include "fuse/ui/ui.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::rich {

struct CharFormat {
    bool bold = false, italic = false, underline = false, strike = false;
    bool hasColor = false;
    Color color{};
    f32 size = 0.f;   ///< pixels; 0 = the widget's font size
    std::string link; ///< href; non-empty = hyperlink
    friend bool operator==(const CharFormat&, const CharFormat&) = default;
};
enum class Align : u8 { Left, Center, Right, Justify };
enum class ListKind : u8 { None, Bullet, Ordered };
struct BlockFormat {
    Align align = Align::Left;
    ListKind list = ListKind::None;
    u8 heading = 0; ///< 0 = paragraph, 1..6
    u8 indent = 0;  ///< list nesting / block indent level
    friend bool operator==(const BlockFormat&, const BlockFormat&) = default;
};
struct Run { std::string text; CharFormat fmt; };

/// Text plus one format per byte and one BlockFormat per paragraph. Positions are byte offsets into plainText(),
/// where paragraphs are joined by '\n'.
class RichDocument {
public:
    RichDocument();
    std::size_t size() const { return m_text.size(); }
    const std::string& plainText() const { return m_text; }
    void clear();
    void setPlainText(std::string_view s);
    /// Inserts `s` ('\n' splits paragraphs; new paragraphs copy the block format of the one split).
    void insert(std::size_t pos, std::string_view s, const CharFormat& fmt);
    void erase(std::size_t pos, std::size_t len);
    /// Calls `fn` on the format of every byte in [a, b).
    void modifyFormat(std::size_t a, std::size_t b, const std::function<void(CharFormat&)>& fn);
    CharFormat formatAt(std::size_t pos) const; ///< format of the byte before `pos` (typing format)
    const CharFormat& formatOfByte(std::size_t i) const { return m_formats[m_fmt[i]]; }

    std::size_t blockCount() const { return m_blocks.size(); }
    std::size_t blockAt(std::size_t pos) const;
    std::size_t blockStart(std::size_t block) const;
    std::size_t blockEnd(std::size_t block) const; ///< position of its '\n' (or size())
    const BlockFormat& blockFormat(std::size_t block) const { return m_blocks[block]; }
    void setBlockFormat(std::size_t block, const BlockFormat& f) { m_blocks[block] = f; }
    std::vector<Run> runs(std::size_t block) const;
    /// Number shown before an ordered list item (1-based, counts consecutive siblings at the same indent).
    u32 listNumber(std::size_t block) const;

    std::string toHtml() const;
    void fromHtml(std::string_view html);
    std::string toMarkdown() const;
    void fromMarkdown(std::string_view md);

    friend bool operator==(const RichDocument& a, const RichDocument& b);

private:
    u32 intern(const CharFormat& f);
    std::string m_text;
    std::vector<u32> m_fmt;
    std::vector<CharFormat> m_formats;
    std::vector<BlockFormat> m_blocks;
};

/// Headless editing model shared by the widget and tests.
class RichTextEditor {
public:
    RichDocument doc;
    std::size_t cursor = 0, anchor = 0;
    CharFormat typing; ///< format applied to inserted text

    std::size_t selStart() const { return std::min(cursor, anchor); }
    std::size_t selEnd() const { return std::max(cursor, anchor); }
    bool hasSelection() const { return cursor != anchor; }
    std::string selectedText() const { return doc.plainText().substr(selStart(), selEnd() - selStart()); }
    void setCursor(std::size_t pos, bool select = false);
    void selectAll() { anchor = 0; cursor = doc.size(); }

    void insertText(std::string_view s); ///< replaces the selection
    void newParagraph();                 ///< Enter: an empty list item leaves the list
    void backspace();
    void del();
    void moveLeft(bool select);
    void moveRight(bool select);
    void wordLeft(bool select);
    void wordRight(bool select);
    void home(bool select); ///< paragraph start
    void end(bool select);

    /// Character formatting: applies to the selection, or sets the typing format when there is none.
    void applyFormat(const std::function<void(CharFormat&)>& fn);
    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void toggleStrike();
    void setColor(Color c);
    void setSize(f32 px);
    void setLink(std::string_view href);
    /// Block formatting of every paragraph touched by the selection.
    void modifyBlocks(const std::function<void(BlockFormat&)>& fn);
    void setAlign(Align a);
    void setList(ListKind k);
    void setHeading(u8 level);
    void indent(int delta);

    bool undo();
    bool redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void resetHistory() { m_undo.clear(); m_redo.clear(); m_coalesce = false; }

    std::function<void()> onChanged; ///< after every document edit

private:
    struct Snap { RichDocument doc; std::size_t cursor, anchor; };
    void snapshot(bool coalescable = false);
    void changed();
    void eraseSelection();
    std::vector<Snap> m_undo, m_redo;
    bool m_coalesce = false;
};

/// Laid-out rich text (word wrapped).
struct RichFragment { std::size_t a, b; f32 x, w, size; CharFormat fmt; };
struct RichLine {
    std::size_t block, a, b; ///< byte range (b excludes the paragraph break)
    f32 x, y, h, baseline, width;
    std::string marker; ///< list bullet / number on the paragraph's first line
    std::vector<RichFragment> frags;
};
struct RichLayout {
    std::vector<RichLine> lines;
    f32 height = 0.f;
    std::size_t hitTest(Vec2 p, const RichDocument& d, const std::function<f32(std::string_view, f32)>& measure) const;
    Rect caretRect(std::size_t pos, const RichDocument& d, const std::function<f32(std::string_view, f32)>& measure) const;
    std::size_t lineOf(std::size_t pos) const;
};
/// `measure(text, px)` -> advance; `lineHeight(px)` -> line box height.
RichLayout layout(const RichDocument& d, f32 width, f32 basePx, const std::function<f32(std::string_view, f32)>& measure,
                  const std::function<f32(f32)>& lineHeight);
f32 headingScale(u8 level);

/// Shears the Glyph commands dl.cmds()[first..] into a synthetic oblique about `baseline` (x shift = slope x height).
void obliqueGlyphs(DrawList& dl, std::size_t first, f32 baseline, f32 slope = 0.2f);

} // namespace fuse::ui::rich

namespace fuse::ui::widgets {
/// QTextEdit: editable rich text. Signals: TextChanged (plain text), Submitted (text = href) on Ctrl+click of a
/// link. Shortcuts: Ctrl+B/I/U, Ctrl+Z/Y, Ctrl+A/C/X/V (clipboard carries plain text), Tab / Shift+Tab indent.
NodeHandle richTextEdit(UiContext& ui, NodeHandle parent, std::string_view id = {});
rich::RichTextEditor* richEditor(UiContext& ui, NodeHandle n);
/// Call after changing richEditor()->doc directly (relayout + repaint + TextChanged).
void richTextChanged(UiContext& ui, NodeHandle n);
void setRichHtml(UiContext& ui, NodeHandle n, std::string_view html);
std::string richHtml(UiContext& ui, NodeHandle n);
void setRichMarkdown(UiContext& ui, NodeHandle n, std::string_view md);
std::string richMarkdown(UiContext& ui, NodeHandle n);
void setRichReadOnly(UiContext& ui, NodeHandle n, bool ro); ///< QTextBrowser: links clickable without Ctrl
} // namespace fuse::ui::widgets
