// fuse_ui_hot_reload_style: a style sheet file reloads in place on change (StyleSheetFileWatcher): the sheet list does
// not grow, the sheet keeps its place in the cascade, the nodes restyle, widget paint code sees new theme tokens; an
// edit that does not parse keeps the last good sheet and reports file:line; a document watched next to it reloads
// with node identity as before.
#include "fuse/ui/doc/hot_reload.hpp"
#include "test_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::doc;
namespace fs = std::filesystem;

namespace {
/// Writes `text` and moves the file time forward by `tick` seconds (file clocks can be coarser than the test).
void put(const fs::path& p, const std::string& text, int tick) {
    { std::ofstream(p, std::ios::trunc | std::ios::binary) << text; }
    std::error_code ec;
    fs::last_write_time(p, fs::last_write_time(p, ec) + std::chrono::seconds(tick), ec);
}
} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "fuse_ui_hot_reload_style";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path css = dir / "theme.css", docPath = dir / "main.fuseui";
    put(css, ".hud { padding: 4px; color: #00ff00; }", 0);
    put(docPath, R"({"fuseui":1,"root":{"class":"panel","children":[{"class":"label","id":"t","classes":["hud"],"text":"one"}]}})", 0);

    UiContext ui({.viewport = {200, 200}, .font = &font()});
    widgets::install(ui);
    auto text = readTextFile(docPath.string());
    UI_CHECK(text.has_value());
    auto d = Document::load(ui, *text, nullptr);
    UI_CHECK(d.has_value());
    if (!d) return finish("fuse_ui_hot_reload_style");
    DocumentFileWatcher docWatch(docPath.string());

    StyleSheetFileWatcher watch(css.string());
    const u32 before = ui.styles().sheetCount();
    UI_CHECK(watch.poll(ui) == StyleSheetFileWatcher::Result::Reloaded); // the first poll loads
    UI_CHECK(watch.sheet() != 0 && ui.styles().hasSheet(watch.sheet()));
    UI_CHECK_EQ(ui.styles().sheetCount(), before + 1);
    UI_CHECK(ui.addStyleSheet(".hud { font-size: 30px; }").has_value()); // a later sheet: stays later
    ui.update(0.016f);
    const NodeHandle t = ui.find("t");
    UI_CHECK(ui.computed(t).color == Color::rgb(0x00FF00) && ui.computed(t).padding.l == 4.f && ui.computed(t).fontSize == 30.f);
    UI_CHECK(watch.poll(ui) == StyleSheetFileWatcher::Result::Unchanged);

    // an edit: replaced in place
    put(css, ":root { --track: #ff00ff; }\n.hud { padding: 9px; color: #ff0000; font-size: 11px; }", 2);
    UI_CHECK(watch.poll(ui) == StyleSheetFileWatcher::Result::Reloaded);
    UI_CHECK_EQ(ui.styles().sheetCount(), before + 2);
    ui.update(0.016f);
    UI_CHECK(ui.computed(t).color == Color::rgb(0xFF0000) && ui.computed(t).padding.l == 9.f);
    UI_CHECK_EQ(ui.computed(t).fontSize, 30.f);                               // the later sheet still wins
    UI_CHECK(ui.themeColor(hashId("--track"), {}) == Color::rgb(0xFF00FF)); // theme tokens for paint code
    UI_CHECK_EQ(watch.reloadCount(), 2u);

    // a broken edit: the last good sheet stays, the error names the file and line; not re-parsed until it changes
    put(css, ".hud { padding: 9px; color: #ff0000; }\n.broken { color: ", 4);
    std::string error;
    UI_CHECK(watch.poll(ui, &error) == StyleSheetFileWatcher::Result::Failed);
    UI_CHECK(error.find("theme.css:") != std::string::npos);
    ui.update(0.016f);
    UI_CHECK(ui.computed(t).color == Color::rgb(0xFF0000) && ui.styles().sheetCount() == before + 2);
    UI_CHECK(watch.poll(ui) == StyleSheetFileWatcher::Result::Unchanged);
    put(css, ".hud { color: #0000ff; }", 6);
    UI_CHECK(watch.poll(ui) == StyleSheetFileWatcher::Result::Reloaded);
    ui.update(0.016f);
    UI_CHECK(ui.computed(t).color == Color::rgb(0x0000FF) && ui.computed(t).padding.l != 9.f);

    // the document next to it reloads with node identity; the reloaded node is styled by the reloaded sheet
    put(docPath, R"({"fuseui":1,"root":{"class":"panel","children":[{"class":"label","id":"t","classes":["hud"],"text":"two"}]}})", 2);
    UI_CHECK(docWatch.poll(**d) == DocumentFileWatcher::Result::Reloaded);
    ui.update(0.016f);
    UI_CHECK(ui.find("t") == t && ui.text(t) == "two" && ui.computed(t).color == Color::rgb(0x0000FF));

    // a deleted file: the sheet stays
    fs::remove(css, ec);
    UI_CHECK(watch.poll(ui) == StyleSheetFileWatcher::Result::Missing);
    UI_CHECK(ui.styles().hasSheet(watch.sheet()));
    fs::remove_all(dir, ec);
    return finish("fuse_ui_hot_reload_style");
}
