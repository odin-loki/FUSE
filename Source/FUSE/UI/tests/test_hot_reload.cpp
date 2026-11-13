// fuse_ui_hot_reload: style sheets and .fuseui documents reload on file change through FORGE's file watcher;
// a bad edit keeps the last good sheet / tree and reports an error.
#include "fuse/ui/doc/hot_reload.hpp"
#include "fuse/ui/os.hpp"
#include "test_util.hpp"
#include <filesystem>
#include <fstream>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::doc;

static void put(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::trunc) << s; }

static int pollFor(HotReload& h, int want) {
    int got = 0;
    for (int i = 0; i < 40 && got < want; ++i) got += h.poll(50);
    return got;
}

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("fuse_hot_" + std::to_string(u64(os::monotonicSeconds() * 1e9)));
    fs::create_directories(dir);
    std::string css = (dir / "theme.css").string(), doc = (dir / "main.fuseui").string();
    put(css, ".hud { padding: 4px; }");
    put(doc, R"({"fuseui":1,"root":{"class":"panel","children":[{"class":"label","id":"t","text":"one"}]}})");

    UiContext ui({.viewport = {200, 200}, .font = &font()});
    widgets::install(ui);
    std::string text;
    UI_CHECK(readTextFile(doc.c_str(), text));
    auto d = Document::load(ui, text, nullptr);
    UI_CHECK(d.has_value());

    HotReload hot(ui);
    int errors = 0;
    hot.onError([&](const std::string&, const std::string&) { ++errors; });
    u32 before = ui.styles().sheetCount();
    UI_CHECK(hot.watchStyle(css));
    UI_CHECK(hot.watchDocument(doc, **d));
    UI_CHECK_EQ(ui.styles().sheetCount(), before + 1);
    ui.update(0.016f);

    u32 v0 = ui.styles().sheetVersion();
    put(css, ".hud { padding: 9px; color: #ff0000; }");
    UI_CHECK(pollFor(hot, 1) >= 1);
    UI_CHECK(ui.styles().sheetVersion() != v0);
    UI_CHECK_EQ(ui.styles().sheetCount(), before + 1); // replaced in place, not appended
    UI_CHECK_EQ(ui.styles().sheet(before).rules().size(), std::size_t(1));

    put(doc, R"({"fuseui":1,"root":{"class":"panel","children":[{"class":"label","id":"t","text":"two"}]}})");
    UI_CHECK(pollFor(hot, 1) >= 1);
    UI_CHECK((*d)->reusedCount() >= 1); // id'd label kept its node

    put(doc, R"({"fuseui":1,"root":{)"); // broken edit: tree kept, error reported
    pollFor(hot, 1);
    UI_CHECK(errors >= 1);

    fs::remove_all(dir);
    return finish("fuse_ui_hot_reload");
}
