// Gate fuse_ui_hot_reload_file: DocumentFileWatcher (plan §4.12). A `.fuseui` file on disk is polled by time stamp;
// when it changes the document is rebuilt in place and nodes with an id keep their identity: the same NodeHandle,
// the keyboard focus, the typed text of a line edit, a slider's value, a scroll area's offset. A broken file leaves
// the tree alone and is not re-parsed until it changes again; a missing file is reported and recovers.
#include "test_util.hpp"

#include "fuse/ui/doc/hot_reload.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::doc;
namespace w = fuse::ui::widgets;
namespace fs = std::filesystem;

namespace {
const char* kV1 = R"({ "fuseui": 1, "root": { "class": "panel", "id": "main", "children": [
  { "class": "label", "id": "title", "text": "Settings" },
  { "class": "lineedit", "id": "name", "text": "" },
  { "class": "slider", "id": "vol" },
  { "class": "panel", "id": "list", "style": { "overflow": "scroll", "height": "80px" }, "children": [
    { "class": "label", "text": "row 1", "style": { "height": "40px" } },
    { "class": "label", "text": "row 2", "style": { "height": "40px" } },
    { "class": "label", "text": "row 3", "style": { "height": "40px" } },
    { "class": "label", "text": "row 4", "style": { "height": "40px" } } ] },
  { "class": "button", "id": "ok", "text": "OK" }
] } })";
// the designer moved things around, renamed the title, added a row and a button
const char* kV2 = R"({ "fuseui": 1, "root": { "class": "panel", "id": "main", "children": [
  { "class": "label", "id": "title", "text": "Audio settings" },
  { "class": "panel", "id": "list", "style": { "overflow": "scroll", "height": "80px" }, "children": [
    { "class": "label", "text": "row 1", "style": { "height": "40px" } },
    { "class": "label", "text": "row 2", "style": { "height": "40px" } },
    { "class": "label", "text": "row 3", "style": { "height": "40px" } },
    { "class": "label", "text": "row 4", "style": { "height": "40px" } },
    { "class": "label", "text": "row 5", "style": { "height": "40px" } } ] },
  { "class": "slider", "id": "vol" },
  { "class": "lineedit", "id": "name", "text": "" },
  { "class": "panel", "id": "buttons", "children": [
    { "class": "button", "id": "ok", "text": "Apply" },
    { "class": "button", "id": "cancel", "text": "Cancel" } ] }
] } })";

fs::file_time_type g_stamp;
/// Writes the file and moves its time stamp forward (file systems with coarse time stamps would otherwise hide a
/// quick second write).
void writeFile(const fs::path& p, std::string_view text) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    UI_CHECK(f != nullptr);
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    g_stamp += std::chrono::seconds(2);
    fs::last_write_time(p, g_stamp);
}
} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "fuse_ui_hot_reload_file";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "settings.fuseui";
    g_stamp = fs::file_time_type::clock::now();
    writeFile(file, kV1);

    UiContext ui({.viewport = {480, 420}, .font = &font()});
    w::install(ui);
    auto text = readTextFile(file.string());
    UI_CHECK(text.has_value() && *text == kV1);
    UI_CHECK(!readTextFile((dir / "nope.fuseui").string()).has_value());
    auto loaded = Document::load(ui, *text);
    UI_CHECK(loaded.has_value());
    if (!loaded) return finish("fuse_ui_hot_reload_file");
    Document& doc = **loaded;
    DocumentFileWatcher watch(file.string());
    ui.update(0.f);
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Unchanged); // the loaded state is the known state

    // user state: focus + typed text, a slider value, a scroll offset
    const NodeHandle main = ui.find("main"), title = ui.find("title"), name = ui.find("name"), vol = ui.find("vol"),
                     list = ui.find("list"), ok = ui.find("ok");
    UI_CHECK(main.valid() && title.valid() && name.valid() && vol.valid() && list.valid() && ok.valid());
    UiTestDriver drv(ui);
    drv.click("name");
    drv.type("Odin");
    UI_CHECK(ui.focused() == name && ui.text(name) == "Odin");
    ui.setValue(vol, 0.7f, true);
    ui.scrollTo(list, {0.f, 55.f});
    ui.update(0.f);
    UI_CHECK_EQ(ui.scroll(list).y, 55.f);
    const u32 nodesV1 = ui.nodeCount();

    // nothing changed on disk: polls are cheap no-ops
    for (int i = 0; i < 3; ++i) UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Unchanged);
    UI_CHECK_EQ(watch.reloadCount(), 0u);

    // the file changes: one reload, identity and state kept
    writeFile(file, kV2);
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Reloaded);
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Unchanged);
    UI_CHECK_EQ(watch.reloadCount(), 1u);
    ui.update(0.f);
    UI_CHECK(ui.find("main") == main && ui.find("title") == title && ui.find("name") == name && ui.find("vol") == vol &&
             ui.find("list") == list && ui.find("ok") == ok);
    UI_CHECK(ui.text(title) == "Audio settings"); // new document text
    UI_CHECK(ui.text(ok) == "Apply");
    UI_CHECK(ui.focused() == name && ui.text(name) == "Odin"); // focus and the edit state survive
    drv.type("!");
    UI_CHECK(ui.text(name) == "Odin!");                         // and the caret still types at the end
    UI_CHECK_EQ(ui.value(vol), 0.7f);
    UI_CHECK_EQ(ui.scroll(list).y, 55.f);
    UI_CHECK(ui.parent(ok) == ui.find("buttons") && ui.find("cancel").valid()); // moved / added by the new document
    UI_CHECK(ui.rect(list).y < ui.rect(vol).y && ui.rect(vol).y < ui.rect(name).y); // the new order is laid out
    UI_CHECK(ui.childCount(list) == 5u);
    UI_CHECK_EQ(ui.nodeCount(), nodesV1 + 3u); // row 5, the buttons panel, cancel

    // a broken file: reported once, the tree stays, no re-parse until the file changes again
    const u32 nodesV2 = ui.nodeCount();
    writeFile(file, "{ \"fuseui\": 1, \"root\": { ");
    DocError err;
    UI_CHECK(watch.poll(doc, &err) == DocumentFileWatcher::Result::Failed);
    UI_CHECK(!err.message.empty());
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Unchanged);
    ui.update(0.f);
    UI_CHECK(ui.find("name") == name && ui.text(name) == "Odin!" && ui.nodeCount() == nodesV2);

    // the file disappears (an editor replacing it): Missing, then the fixed file reloads
    fs::remove(file, ec);
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Missing);
    UI_CHECK(ui.find("name") == name);
    writeFile(file, kV1);
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Reloaded);
    ui.update(0.f);
    UI_CHECK(ui.find("name") == name && ui.text(name) == "Odin!" && ui.focused() == name);
    UI_CHECK(ui.text(title) == "Settings" && !ui.find("cancel").valid());
    UI_CHECK_EQ(ui.nodeCount(), nodesV1);
    UI_CHECK_EQ(ui.scroll(list).y, 55.f);
    // invalidate forces a reload of an unchanged file
    watch.invalidate();
    UI_CHECK(watch.poll(doc) == DocumentFileWatcher::Result::Reloaded);
    UI_CHECK_EQ(watch.reloadCount(), 3u);

    fs::remove_all(dir, ec);
    return finish("fuse_ui_hot_reload_file");
}
