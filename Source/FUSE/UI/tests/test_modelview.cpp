// Model / view gates (plan §4.11; fuse/ui/widgets/modelview.hpp). One binary, one ctest gate per `--gate=<name>`
// (fuse_ui_mv_<name>) and one golden per `--scene=<name>` (fuse_ui_mv_golden_<name>, tests/golden/modelview_<name>
// [_full].png, FUSE_UI_UPDATE_GOLDEN=1 rewrites; same tolerance as fuse_ui_widget_golden). `--out=<dir>` writes PNGs.
//   notify        model notifications: rows inserted / removed / moved / data changed / reset keep selection and scroll
//   selection     SelectionModel ranges; single / multi / extended / cell selection in table, list, grid; sharing
//   selection_1m  range selection over 1,000,000 rows stores ranges: no per-row allocation
//   tree          treeModelView: expansion beyond 64 rows, keys, type-ahead, tree table, notifications; string treeView
//   tree_1m       a lazy tree with > 1M nodes: constant node count, bounded model reads per frame, no steady allocs
//   editing       in-place editors: double click / F2 / typing, Enter / Escape / Tab, spin, combo, check boxes
//   view          horizontal scroll, frozen / hidden / reordered columns, resize to contents, row height, tooltips,
//                 context-menu hook, typed roles
//   proxy         multi-key sort, all-column and wildcard filters, proxy over proxy, tree filter keeping ancestors
//   async         AsyncModel: placeholders, deferred delivery (same thread and a worker thread), stale tickets, LRU
//   dir_model     engine-adapter example: a std::filesystem directory model with lazy children behind the tree view
#include "test_alloc_hooks.hpp"
#include "test_util.hpp"
#include "fuse/text/from_chars.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned long long> g_allocs{0}, g_bytes{0};
}
// Every global allocation form is replaced and counted (test_alloc_hooks.hpp).
void fuse::ui::test::countAlloc(std::size_t n) noexcept {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
}

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
std::string g_out;
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void settle(UiContext& ui) { ui.update(1.f); ui.update(1.f); } // lets the double-click / tooltip / type-ahead timers run out
void click(UiContext& ui, Vec2 p, u8 mods = 0) {
    ui.setModifiers(mods);
    ui.pointerMove(p);
    ui.pointerDown(p);
    ui.pointerUp(p);
    ui.setModifiers(0);
    ui.update(1.f / 60.f);
}
void drag(UiContext& ui, Vec2 a, Vec2 b) {
    ui.pointerMove(a);
    ui.pointerDown(a);
    ui.update(1.f / 60.f);
    ui.pointerMove({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f});
    ui.pointerMove(b);
    ui.update(1.f / 60.f);
    ui.pointerUp(b);
    ui.update(1.f / 60.f);
}
void key(UiContext& ui, Key k, u8 mods = 0) {
    ui.keyDown(k, mods);
    ui.keyUp(k, mods);
    ui.update(1.f / 60.f);
}
/// Middle of a cell (context coordinates); the row must be in the window.
Vec2 cellPt(UiContext& ui, NodeHandle view, u32 row, u32 col) { return ctr(w::viewCellRect(ui, view, row, col)); }
void clickCell(UiContext& ui, NodeHandle view, u32 row, u32 col, u8 mods = 0) {
    click(ui, cellPt(ui, view, row, col), mods);
    settle(ui); // never an accidental double click
}
void shot(UiContext& ui, const char* name) {
    if (g_out.empty()) return;
    ui.update();
    Image img;
    render(ui, img);
    stbi_write_png((g_out + "/" + name).c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
}
std::string num(long long v) { return std::to_string(v); }

// ---- models used by the gates ----------------------------------------------------------------------------------------
/// A flat model over a vector with a notifier: col 0 = the string, col 1 = its length, col 2 = "x".
struct VecModel {
    std::vector<std::string> rows;
    w::ModelNotifier notifier;
    w::ItemModel model;
    mutable u64 reads = 0;
    VecModel() {
        model.user = this;
        model.columns = 3;
        model.notifier = &notifier;
        model.rows = [](const void* u) { return u32(static_cast<const VecModel*>(u)->rows.size()); };
        model.data = [](const void* u, u32 r, u32 c, std::string& sc) -> std::string_view {
            const auto* m = static_cast<const VecModel*>(u);
            ++m->reads;
            if (c == 0) return m->rows[r];
            if (c == 1) { sc = std::to_string(m->rows[r].size()); return sc; }
            return "x";
        };
        model.header = [](const void*, u32 c) -> std::string_view {
            static constexpr std::string_view h[] = {"Text", "Length", "Flag"};
            return h[c];
        };
    }
    void fill(int n) {
        rows.clear();
        for (int i = 0; i < n; ++i) rows.push_back("item " + std::to_string(i));
    }
};
struct BigModel { // 1,000,000 computed rows
    mutable u64 reads = 0;
    w::ItemModel model;
    BigModel() {
        model.user = this;
        model.columns = 3;
        model.rows = [](const void*) { return 1000000u; };
        model.data = [](const void* u, u32 r, u32 c, std::string& scratch) -> std::string_view {
            ++static_cast<const BigModel*>(u)->reads;
            char b[32];
            char* e = b;
            if (c == 0) { std::memcpy(b, "entity_", 7); e = fuse::text::toChars(b + 7, b + sizeof b, r).ptr; }
            else e = fuse::text::toChars(b, b + sizeof b, (r * 7919u + c) % 100000u).ptr;
            scratch.assign(b, e);
            return scratch;
        };
        model.header = [](const void*, u32 c) -> std::string_view {
            static constexpr std::string_view h[] = {"Name", "A", "B"};
            return h[c];
        };
    }
};

// ---- notify ---------------------------------------------------------------------------------------------------------------
int gateNotify() {
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    VecModel m;
    m.fill(1000);
    const NodeHandle t = w::tableView(ui, ui.root(), &m.model, "t");
    ui.setCss(t, "flex-grow", "1");
    ui.update();
    UI_CHECK_EQ(m.notifier.subscribers(), 1u);
    int changes = 0;
    ui.connect(t, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    // plain single selection: the selected row and the window follow inserted / removed rows
    w::setViewCurrent(ui, t, 500);
    ui.update();
    UI_CHECK_EQ(i32(ui.value(t)), 500);
    const u32 f0 = w::tableFirstVisible(ui, t);
    UI_CHECK(f0 > 400u && f0 <= 500u);
    m.rows.insert(m.rows.begin(), 10, "new");
    m.notifier.rowsInserted(0, 10);
    ui.update();
    UI_CHECK_EQ(i32(ui.value(t)), 510);
    UI_CHECK_EQ(m.rows[std::size_t(ui.value(t))], std::string("item 500"));
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), f0 + 10u); // the same rows stay in view
    m.rows.erase(m.rows.begin(), m.rows.begin() + 20);
    m.notifier.rowsRemoved(0, 20);
    ui.update();
    UI_CHECK_EQ(i32(ui.value(t)), 490);
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), f0 - 10u);
    UI_CHECK_EQ(m.rows[490], std::string("item 500"));
    // rows inserted below the window change neither
    m.rows.insert(m.rows.begin() + 900, 5, "tail");
    m.notifier.rowsInserted(900, 5);
    ui.update();
    UI_CHECK_EQ(i32(ui.value(t)), 490);
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), f0 - 10u);
    // a block move carries the selected row with it
    {
        std::vector<std::string> block(m.rows.begin() + 485, m.rows.begin() + 495); // rows 485..494 -> start at 100
        m.rows.erase(m.rows.begin() + 485, m.rows.begin() + 495);
        m.rows.insert(m.rows.begin() + 100, block.begin(), block.end());
        m.notifier.rowsMoved(485, 10, 100);
        ui.update();
        UI_CHECK_EQ(i32(ui.value(t)), 105);
        UI_CHECK_EQ(m.rows[105], std::string("item 500"));
    }
    // data changed: only rows in the window repaint
    w::setViewFirstVisible(ui, t, 0);
    ui.update();
    ui.update();
    m.reads = 0;
    m.rows[800] = "changed far away";
    m.notifier.dataChanged(800);
    ui.update();
    UI_CHECK(!ui.stats().repainted);
    UI_CHECK_EQ(m.reads, 0ull);
    m.rows[3] = "changed here";
    m.notifier.dataChanged(3);
    ui.update();
    UI_CHECK(ui.stats().repainted);
    UI_CHECK(m.reads > 0ull);
    // removing the selected row deselects (plain single selection has no neighbour rule)
    const int before = changes;
    m.rows.erase(m.rows.begin() + 105);
    m.notifier.rowsRemoved(105, 1);
    ui.update();
    UI_CHECK_EQ(i32(ui.value(t)), -1);
    UI_CHECK(changes > before);

    // a view with a selection model: ranges split and shift
    w::setViewSelectionMode(ui, t, w::SelectionMode::Extended);
    w::SelectionModel* sel = w::viewSelectionModel(ui, t);
    UI_CHECK(sel != nullptr);
    sel->selectRows(5, 9);
    sel->setCurrent(7);
    m.rows.insert(m.rows.begin() + 7, 3, "mid");
    m.notifier.rowsInserted(7, 3);
    ui.update();
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(2)); // {5,6} and {10..12}; the new rows are not selected
    UI_CHECK_EQ(sel->selectedRows(), 5ull);
    UI_CHECK(sel->isSelected(6) && !sel->isSelected(7) && !sel->isSelected(9) && sel->isSelected(10) && sel->isSelected(12));
    UI_CHECK_EQ(sel->currentRow(), 10ll);
    UI_CHECK_EQ(i32(ui.value(t)), 10); // value() mirrors the current row
    m.rows.erase(m.rows.begin() + 6, m.rows.begin() + 11); // rows 6..10: the tail of one range, the head of the other
    m.notifier.rowsRemoved(6, 5);
    ui.update();
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(1)); // {5} + {6,7} joined
    UI_CHECK_EQ(sel->selectedRows(), 3ull);
    UI_CHECK(sel->isSelected(5) && sel->isSelected(7) && !sel->isSelected(8));
    // reset: selection gone, window clamped
    w::setViewFirstVisible(ui, t, 600);
    m.fill(12);
    m.notifier.reset();
    ui.update();
    UI_CHECK(sel->empty());
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), 0u);
    UI_CHECK_EQ(w::viewRowCount(ui, t), 12u);
    // inserting at the very top while at the top keeps the top
    m.rows.insert(m.rows.begin(), "first");
    m.notifier.rowsInserted(0, 1);
    ui.update();
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), 0u);
    // a destroyed view drops its subscription at the next report
    ui.destroy(t);
    ui.update();
    m.notifier.dataChanged(0);
    UI_CHECK_EQ(m.notifier.subscribers(), 0u);
    return finish("fuse_ui_mv_notify");
}

// ---- selection --------------------------------------------------------------------------------------------------------------
int gateSelection() {
    // the model on its own
    {
        w::SelectionModel s;
        int notes = 0;
        s.subscribe([](void* u, u64, const w::SelectionModel&) { ++*static_cast<int*>(u); return true; }, &notes);
        s.selectRows(10, 20);
        s.selectRows(21, 30); // adjacent: one range
        UI_CHECK_EQ(s.ranges().size(), std::size_t(1));
        UI_CHECK_EQ(s.selectedRows(), 21ull);
        s.toggle(15);
        UI_CHECK_EQ(s.ranges().size(), std::size_t(2));
        UI_CHECK(!s.isSelected(15) && s.isSelected(14) && s.isSelected(16));
        s.toggle(15);
        UI_CHECK_EQ(s.ranges().size(), std::size_t(1));
        UI_CHECK_EQ(notes, 4);
        s.clear();
        // cells: a rectangle, a hole punched in it
        s.select({2, 4, 1, 3});
        UI_CHECK_EQ(s.selectedCells(8), 9ull);
        UI_CHECK(s.isSelected(3, 2) && !s.isSelected(3, 0) && !s.isSelected(5, 2));
        s.deselect({3, 3, 2, 2});
        UI_CHECK_EQ(s.selectedCells(8), 8ull);
        UI_CHECK(!s.isSelected(3, 2) && s.isSelected(3, 1) && s.isSelected(3, 3) && s.isSelected(2, 2) && s.isSelected(4, 2));
        UI_CHECK_EQ(s.selectedRows(), 3ull);
        s.setSelection({7, 7});
        UI_CHECK(s.rowSelected(7) && s.isSelected(7, 31) && s.selectedRows() == 1);
        // rows moved: the block's selection travels
        s.setSelection({10, 12});
        s.setCurrent(11);
        s.rowsMoved(10, 3, 2);
        UI_CHECK(s.isSelected(2) && s.isSelected(4) && !s.isSelected(5) && !s.isSelected(10));
        UI_CHECK_EQ(s.currentRow(), 3ll);
    }
    UiContext ui({.viewport = {900, 520}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    ui.setCss(ui.root(), "flex-direction", "row");
    VecModel m;
    m.fill(200);
    const NodeHandle t = w::tableView(ui, ui.root(), &m.model, "t");
    ui.setCss(t, "width", "420px");
    const NodeHandle list = w::modelListView(ui, ui.root(), &m.model, "list");
    ui.setCss(list, "width", "200px");
    const NodeHandle grid = w::gridView(ui, ui.root(), &m.model, 90.f, 70.f, "grid");
    ui.setCss(grid, "flex-grow", "1");
    ui.update();
    // default: plain single selection, no model
    UI_CHECK(w::viewSelectionModel(ui, t) == nullptr);
    clickCell(ui, t, 3, 0);
    UI_CHECK_EQ(i32(ui.value(t)), 3);
    // extended
    w::setViewSelectionMode(ui, t, w::SelectionMode::Extended);
    w::SelectionModel* sel = w::viewSelectionModel(ui, t);
    UI_CHECK(sel && sel->rowSelected(3) && sel->currentRow() == 3); // the plain selection carried over
    int signals = 0;
    ui.connect(t, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++signals; });
    clickCell(ui, t, 2, 0);
    UI_CHECK(sel->rowSelected(2) && sel->selectedRows() == 1);
    clickCell(ui, t, 5, 0, kModCtrl); // Ctrl+click toggles
    UI_CHECK(sel->rowSelected(2) && sel->rowSelected(5) && sel->selectedRows() == 2);
    UI_CHECK_EQ(signals, 2);
    clickCell(ui, t, 8, 1, kModShift); // Shift+click: the range from the anchor (5), replacing the rest
    UI_CHECK(!sel->rowSelected(2) && sel->rowSelected(5) && sel->rowSelected(8) && sel->selectedRows() == 4);
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(1));
    clickCell(ui, t, 12, 0, kModShift | kModCtrl); // Ctrl+Shift adds the range
    UI_CHECK_EQ(sel->selectedRows(), 8ull);
    clickCell(ui, t, 6, 0, kModCtrl); // toggling inside a range splits it
    UI_CHECK(!sel->rowSelected(6) && sel->rowSelected(7) && sel->selectedRows() == 7);
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(2));
    // keyboard: plain arrows select, Shift extends from the anchor, Ctrl moves only the current row
    clickCell(ui, t, 4, 0);
    key(ui, Key::Down);
    UI_CHECK(sel->rowSelected(5) && sel->selectedRows() == 1 && sel->currentRow() == 5);
    key(ui, Key::Down, kModShift);
    key(ui, Key::Down, kModShift);
    UI_CHECK(sel->rowSelected(5) && sel->rowSelected(7) && sel->selectedRows() == 3 && sel->currentRow() == 7);
    key(ui, Key::Up, kModShift);
    UI_CHECK(sel->selectedRows() == 2 && !sel->rowSelected(7));
    key(ui, Key::Down, kModCtrl);
    key(ui, Key::Down, kModCtrl);
    UI_CHECK(sel->currentRow() == 8 && sel->selectedRows() == 2 && !sel->rowSelected(8)); // current != selected
    UI_CHECK_EQ(i32(ui.value(t)), 8);
    key(ui, Key::Space); // toggles the current row
    UI_CHECK(sel->rowSelected(8) && sel->selectedRows() == 3);
    key(ui, Key::A, kModCtrl);
    UI_CHECK(sel->selectedRows() == 200 && sel->ranges().size() == 1);
    // multi: clicks toggle, arrows do not select
    w::setViewSelectionMode(ui, t, w::SelectionMode::Multi);
    sel->clear();
    clickCell(ui, t, 1, 0);
    clickCell(ui, t, 3, 0);
    clickCell(ui, t, 1, 0);
    UI_CHECK(!sel->rowSelected(1) && sel->rowSelected(3) && sel->selectedRows() == 1);
    key(ui, Key::Down);
    UI_CHECK(sel->currentRow() == 2 && sel->selectedRows() == 1);
    // none
    w::setViewSelectionMode(ui, t, w::SelectionMode::None);
    clickCell(ui, t, 4, 0);
    UI_CHECK(sel->empty() && sel->currentRow() == 4);
    // cells
    w::setViewSelectionMode(ui, t, w::SelectionMode::Extended, w::SelectionUnit::Cells);
    clickCell(ui, t, 1, 1);
    UI_CHECK(sel->isSelected(1, 1) && !sel->isSelected(1, 0) && sel->selectedCells(3) == 1);
    clickCell(ui, t, 3, 2, kModShift);
    UI_CHECK(sel->selectedCells(3) == 6 && sel->isSelected(2, 1) && sel->isSelected(3, 2) && !sel->isSelected(2, 0));
    UI_CHECK(w::viewCurrentColumn(ui, t) == 2u && w::viewCurrentRow(ui, t) == 3);
    key(ui, Key::Left);
    UI_CHECK(w::viewCurrentColumn(ui, t) == 1u && sel->selectedCells(3) == 1 && sel->isSelected(3, 1));
    key(ui, Key::Right, kModShift);
    UI_CHECK(sel->selectedCells(3) == 2 && sel->isSelected(3, 2));
    clickCell(ui, t, 6, 0, kModCtrl);
    UI_CHECK(sel->selectedCells(3) == 3 && sel->isSelected(6, 0));
    shot(ui, "selection_cells.png");

    // one selection model shared by the table and the list (same model): each follows the other
    w::SelectionModel shared;
    w::setViewSelectionMode(ui, t, w::SelectionMode::Extended);
    w::setViewSelectionModel(ui, t, &shared);
    w::setViewSelectionMode(ui, list, w::SelectionMode::Extended);
    w::setViewSelectionModel(ui, list, &shared);
    UI_CHECK(w::viewSelectionModel(ui, list) == &shared && shared.tracking());
    int listSignals = 0;
    ui.connect(list, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++listSignals; });
    clickCell(ui, t, 2, 0);
    clickCell(ui, t, 4, 0, kModShift);
    UI_CHECK(shared.selectedRows() == 3 && i32(ui.value(list)) == 4 && listSignals == 2);
    clickCell(ui, list, 7, 0, kModCtrl);
    UI_CHECK(shared.selectedRows() == 4 && shared.rowSelected(7) && i32(ui.value(t)) == 7);
    // the shared model follows the notifier once, not once per view
    m.rows.insert(m.rows.begin(), "top");
    m.notifier.rowsInserted(0, 1);
    ui.update();
    UI_CHECK(shared.rowSelected(3) && shared.rowSelected(5) && shared.rowSelected(8) && !shared.rowSelected(2) && shared.selectedRows() == 4);
    UI_CHECK_EQ(shared.currentRow(), 8ll);

    // grid view
    clickCell(ui, t, 0, 0);
    const Rect gr = ui.contentRect(grid);
    auto tile = [&](u32 i) { const u32 nc = w::gridColumns(ui, grid); return Vec2{gr.x + (f32(i % nc) + 0.5f) * 90.f, gr.y + (f32(i / nc) + 0.5f) * 70.f}; };
    click(ui, tile(1));
    UI_CHECK_EQ(i32(ui.value(grid)), 1);
    w::setViewSelectionMode(ui, grid, w::SelectionMode::Extended);
    w::SelectionModel* gs = w::viewSelectionModel(ui, grid);
    UI_CHECK(gs && gs->rowSelected(1));
    click(ui, tile(3), kModCtrl);
    click(ui, tile(5), kModShift);
    UI_CHECK(gs->rowSelected(1) == false && gs->rowSelected(3) && gs->rowSelected(4) && gs->rowSelected(5) && gs->selectedRows() == 3);
    click(ui, tile(0), kModCtrl);
    UI_CHECK(gs->selectedRows() == 4 && i32(ui.value(grid)) == 0);
    key(ui, Key::Right, kModShift);
    UI_CHECK(gs->rowSelected(0) && gs->rowSelected(1) && gs->selectedRows() == 2);
    key(ui, Key::A, kModCtrl);
    UI_CHECK_EQ(gs->selectedRows(), 201ull);
    shot(ui, "selection.png");
    // views die before the shared selection model and the notifier: nothing dangles
    ui.destroy(t);
    ui.destroy(list);
    ui.destroy(grid);
    ui.update();
    shared.selectRow(1);
    return finish("fuse_ui_mv_selection");
}

int gateSelection1M() {
    UiContext ui({.viewport = {800, 600}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    BigModel big;
    const NodeHandle t = w::tableView(ui, ui.root(), &big.model, "big");
    ui.setCss(t, "flex-grow", "1");
    w::setViewSelectionMode(ui, t, w::SelectionMode::Extended);
    w::SelectionModel* sel = w::viewSelectionModel(ui, t);
    ui.update();
    ui.setFocus(t);
    clickCell(ui, t, 0, 0);
    for (int i = 0; i < 30; ++i) { key(ui, Key::PageDown, kModShift); key(ui, Key::PageUp, kModShift); } // warm-up
    clickCell(ui, t, 0, 0);
    const unsigned long long a0 = g_allocs.load(), b0 = g_bytes.load();
    const auto t0 = std::chrono::steady_clock::now();
    key(ui, Key::End, kModShift); // Shift+End: rows 0 .. 999,999
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const unsigned long long allocs = g_allocs.load() - a0, bytes = g_bytes.load() - b0;
    std::printf("range select over 1M rows: %.3f ms, %llu allocs (%llu bytes), %zu range(s)\n", ms, allocs, bytes, sel->ranges().size());
    UI_CHECK_EQ(sel->selectedRows(), 1000000ull);
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(1));
    UI_CHECK(allocs <= 8 && bytes <= 16 * 1024); // ranges, not rows
    UI_CHECK(ms < 50.0);
    UI_CHECK(sel->rowSelected(0) && sel->rowSelected(500000) && sel->rowSelected(999999));
    // scrolling a fully selected million rows: steady frames do not allocate
    const Vec2 mid = ctr(ui.rect(t));
    ui.pointerMove(mid);
    for (int i = 0; i < 60; ++i) { ui.wheel(mid, {0.f, 30.f}); ui.update(1.f / 60.f); }
    const unsigned long long a1 = g_allocs.load();
    for (int i = 0; i < 300; ++i) { ui.wheel(mid, {0.f, 30.f}); ui.update(1.f / 60.f); }
    UI_CHECK_NO_ALLOC(g_allocs.load() - a1);
    // punching holes makes ranges, still not rows
    key(ui, Key::A, kModCtrl);
    w::setViewFirstVisible(ui, t, 500000);
    ui.update();
    clickCell(ui, t, 500003, 0, kModCtrl);
    clickCell(ui, t, 500007, 0, kModCtrl);
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(3));
    UI_CHECK_EQ(sel->selectedRows(), 999998ull);
    UI_CHECK(!sel->rowSelected(500003) && sel->rowSelected(500004));
    // 20,000 scattered single rows: lookups stay a binary search (paint cost does not grow with the range count)
    sel->clear();
    for (u32 r = 0; r < 40000; r += 2) sel->selectRow(r);
    UI_CHECK_EQ(sel->ranges().size(), std::size_t(20000));
    w::setViewFirstVisible(ui, t, 100);
    std::vector<double> frame;
    for (int i = 0; i < 200; ++i) {
        const auto f0 = std::chrono::steady_clock::now();
        ui.wheel(mid, {0.f, -1.f});
        ui.update(1.f / 60.f);
        frame.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count());
    }
    std::sort(frame.begin(), frame.end());
    std::printf("20k ranges: frame median %.3f ms\n", frame[frame.size() / 2]);
    UI_CHECK(frame[frame.size() / 2] <= 4.0);
    return finish("fuse_ui_mv_selection_1m");
}

// ---- tree -------------------------------------------------------------------------------------------------------------------
int gateTree() {
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    w::StringTreeModel m("Name\tKind\tSize");
    const w::ItemId scene = m.add(w::kRootItem, "Scene\tgroup\t");
    const w::ItemId assets = m.add(w::kRootItem, "Assets\tgroup\t");
    const w::ItemId empty = m.add(w::kRootItem, "Empty\tgroup\t");
    std::vector<w::ItemId> kids;
    for (int i = 0; i < 200; ++i) {
        kids.push_back(m.add(scene, "node_" + num(i) + "\tmesh\t" + num(i * 3)));
        m.add(kids.back(), "lod0\tgeometry\t1");
        m.add(kids.back(), "lod1\tgeometry\t2");
        m.add(kids.back(), "material\tasset\t3");
    }
    const w::ItemId lights = m.add(assets, "Lights\tgroup\t");
    m.add(lights, "Sun\tlight\t1");
    m.add(lights, "Fill\tlight\t2");
    m.add(assets, "Terrain\tmesh\t900");
    const NodeHandle tree = w::treeModelView(ui, ui.root(), &m.model(), "tree");
    ui.setCss(tree, "flex-grow", "1");
    ui.update();
    ui.setFocus(tree);
    const u32 nodes = ui.nodeCount();
    UI_CHECK(ui.alive(tree));
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 3u);
    UI_CHECK(w::treeItemAt(ui, tree, 1) == assets && w::treeExpandedCount(ui, tree) == 0u);
    int toggles = 0;
    f32 lastToggleRow = -1.f;
    bool lastOpen = false;
    ui.connect(tree, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& a) {
        if (a.text != "expand") return; // (a header click is "sort")
        ++toggles;
        lastToggleRow = a.value;
        lastOpen = a.flag;
    });
    // expansion is per node, with no limit on the row
    UI_CHECK(w::setTreeRowExpanded(ui, tree, 0, true));
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 203u);
    UI_CHECK(toggles == 1 && lastOpen && lastToggleRow == 0.f);
    UI_CHECK(w::treeRowExpanded(ui, tree, 0) && !w::treeRowExpanded(ui, tree, 1));
    UI_CHECK(w::setTreeRowExpanded(ui, tree, 151, true)); // node_150: well past row 63
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 206u);
    UI_CHECK(w::treeItemAt(ui, tree, 151) == kids[150] && w::treeRowDepth(ui, tree, 152) == 2u && w::treeRowDepth(ui, tree, 151) == 1u);
    UI_CHECK(w::treeItemAt(ui, tree, 155) == kids[151]);
    UI_CHECK(w::setTreeItemExpanded(ui, tree, kids[199], true));
    UI_CHECK(w::setTreeItemExpanded(ui, tree, kids[10], true));
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 212u);
    UI_CHECK_EQ(w::treeExpandedCount(ui, tree), 4u);
    UI_CHECK_EQ(w::treeRowOf(ui, tree, kids[150], scene), 154ll); // node_10's three children came before it
    UI_CHECK(w::setTreeItemExpanded(ui, tree, kids[10], false));
    UI_CHECK_EQ(w::treeRowOf(ui, tree, kids[150], scene), 151ll);
    UI_CHECK(!w::setTreeRowExpanded(ui, tree, 152, true)); // a leaf
    UI_CHECK_EQ(ui.nodeCount(), nodes);
    // the "Assets" row moved down with everything that opened above it
    UI_CHECK(w::treeItemAt(ui, tree, 207) == assets && w::treeItemAt(ui, tree, 208) == empty);
    // pointer: the expander toggles, the label selects
    {
        const Rect r = w::viewCellRect(ui, tree, 0, 0);
        click(ui, {r.x - 2.f, r.y + r.h * 0.5f}); // the arrow sits left of the text
        settle(ui);
        UI_CHECK_EQ(w::viewRowCount(ui, tree), 3u);
        UI_CHECK(!lastOpen && w::treeExpandedCount(ui, tree) == 0u);
        clickCell(ui, tree, 1, 0);
        UI_CHECK_EQ(i32(ui.value(tree)), 1);
        click(ui, cellPt(ui, tree, 1, 0)); // double click toggles too
        click(ui, cellPt(ui, tree, 1, 0));
        settle(ui);
        UI_CHECK(w::treeItemExpanded(ui, tree, assets));
        UI_CHECK_EQ(w::viewRowCount(ui, tree), 5u);
    }
    // keyboard: Right expands then steps in, Left steps out then collapses
    clickCell(ui, tree, 2, 0); // Lights
    key(ui, Key::Right);
    UI_CHECK(w::treeItemExpanded(ui, tree, lights) && w::viewRowCount(ui, tree) == 7u);
    key(ui, Key::Right);
    UI_CHECK_EQ(i32(ui.value(tree)), 3); // Sun
    key(ui, Key::Down);
    UI_CHECK_EQ(i32(ui.value(tree)), 4); // Fill
    key(ui, Key::Left);
    UI_CHECK_EQ(i32(ui.value(tree)), 2); // back on Lights
    key(ui, Key::Left);
    UI_CHECK(!w::treeItemExpanded(ui, tree, lights) && w::viewRowCount(ui, tree) == 5u);
    key(ui, Key::Left);
    UI_CHECK_EQ(i32(ui.value(tree)), 1); // Assets
    key(ui, Key::Home);
    UI_CHECK_EQ(i32(ui.value(tree)), 0);
    key(ui, Key::Right); // Scene opens: 200 children
    key(ui, Key::End);
    UI_CHECK_EQ(i32(ui.value(tree)), i32(w::viewRowCount(ui, tree)) - 1);
    UI_CHECK(w::tableFirstVisible(ui, tree) > 150u);
    key(ui, Key::PageUp);
    UI_CHECK(i32(ui.value(tree)) == i32(w::viewRowCount(ui, tree)) - 1 - i32(w::viewVisibleRows(ui, tree)));
    key(ui, Key::Home);
    int submitted = -1;
    ui.connect(tree, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { submitted = i32(a.value); });
    key(ui, Key::Down);
    key(ui, Key::Enter);
    UI_CHECK_EQ(submitted, 1);
    // type-ahead: a prefix, refined by the next letter; a new first letter moves on
    settle(ui);
    ui.textInput("n");
    ui.textInput("o");
    ui.textInput("d");
    ui.textInput("e");
    ui.textInput("_");
    ui.textInput("1");
    ui.textInput("7");
    ui.update(1.f / 60.f);
    UI_CHECK(w::treeItemAt(ui, tree, u32(ui.value(tree))) == kids[17]);
    ui.textInput("3");
    ui.update(1.f / 60.f);
    UI_CHECK(w::treeItemAt(ui, tree, u32(ui.value(tree))) == kids[173]);
    UI_CHECK(w::tableFirstVisible(ui, tree) > 100u); // scrolled to it
    settle(ui); // the prefix times out
    ui.textInput("a");
    ui.update(1.f / 60.f);
    UI_CHECK(w::treeItemAt(ui, tree, u32(ui.value(tree))) == assets);
    settle(ui);
    // notifications: rows appear and vanish under expanded parents only, the current row follows
    key(ui, Key::Home);
    w::setViewCurrent(ui, tree, 50); // node_49
    ui.update();
    const u32 firstBefore = w::tableFirstVisible(ui, tree);
    const u32 rowsBefore = w::viewRowCount(ui, tree);
    const w::ItemId added = m.add(scene, "inserted\tmesh\t7", 0);
    ui.update();
    UI_CHECK_EQ(w::viewRowCount(ui, tree), rowsBefore + 1u);
    UI_CHECK(i32(ui.value(tree)) == 51 && w::treeItemAt(ui, tree, 51) == kids[49] && w::treeItemAt(ui, tree, 1) == added);
    UI_CHECK_EQ(w::tableFirstVisible(ui, tree), firstBefore + 1u);
    m.add(kids[5], "hidden child\tgeometry\t0"); // kids[5] is collapsed: no row
    ui.update();
    UI_CHECK_EQ(w::viewRowCount(ui, tree), rowsBefore + 1u);
    UI_CHECK(w::setTreeItemExpanded(ui, tree, kids[5], true));
    UI_CHECK_EQ(w::viewRowCount(ui, tree), rowsBefore + 5u);
    m.remove(kids[5]); // an expanded node goes away with its rows
    ui.update();
    UI_CHECK_EQ(w::viewRowCount(ui, tree), rowsBefore);
    UI_CHECK(w::treeItemAt(ui, tree, u32(ui.value(tree))) == kids[49]);
    UI_CHECK(!w::treeItemExpanded(ui, tree, kids[5]));
    m.remove(added);
    m.setCell(kids[0], 0, "renamed");
    ui.update();
    std::string sc;
    UI_CHECK(m.model().itemData(m.model().user, w::treeItemAt(ui, tree, 1), 0, sc) == "renamed");
    // tree table: a header, cells of the other columns, extended selection
    w::setViewSelectionMode(ui, tree, w::SelectionMode::Extended);
    w::SelectionModel* sel = w::viewSelectionModel(ui, tree);
    key(ui, Key::Home);
    UI_CHECK(!w::viewCellRect(ui, tree, 0, 1).empty() && w::viewCellRect(ui, tree, 0, 1).x > w::viewCellRect(ui, tree, 0, 0).x);
    UI_CHECK_EQ(w::viewCellRect(ui, tree, 0, 0).x + 16.f, w::viewCellRect(ui, tree, 1, 0).x); // one level of indentation
    clickCell(ui, tree, 2, 1);
    clickCell(ui, tree, 6, 1, kModShift);
    UI_CHECK(sel->selectedRows() == 5 && sel->rowSelected(4));
    UI_CHECK(w::setTreeRowExpanded(ui, tree, 3, true)); // rows open inside the selection: they are not selected
    UI_CHECK(sel->selectedRows() == 5 && sel->rowSelected(3) && !sel->rowSelected(4) && sel->rowSelected(7) && sel->rowSelected(9));
    shot(ui, "tree.png");
    UI_CHECK(w::setTreeRowExpanded(ui, tree, 3, false));
    UI_CHECK(sel->selectedRows() == 5 && sel->rowSelected(4) && sel->ranges().size() == 1);
    UI_CHECK(w::setTreeRowExpanded(ui, tree, 0, false)); // collapsing the parent deselects what it hid
    UI_CHECK(sel->empty() && w::viewRowCount(ui, tree) == 5u); // Scene, Assets + its two, Empty
    UI_CHECK_EQ(sel->currentRow(), 0ll); // the current row handed over to the collapsed node
    w::setTreeRowExpanded(ui, tree, 0, true);
    w::tableModelChanged(ui, tree); // no notification to go by: back to the collapsed tree
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 3u);
    UI_CHECK_EQ(ui.nodeCount(), nodes);

    // the string tree view: collapsing works past line 63 too
    std::string lines;
    for (int i = 0; i < 100; ++i) { lines += "group " + num(i) + "\n\tchild a\n\tchild b\n"; }
    lines.pop_back();
    const NodeHandle st = w::treeView(ui, ui.root(), lines, "stringtree");
    ui.update();
    UI_CHECK_EQ(w::treeVisibleRows(ui, st), 300u);
    w::setTreeExpanded(ui, st, 150, false); // "group 50"
    UI_CHECK(!w::treeExpanded(ui, st, 150) && w::treeExpanded(ui, st, 147) && w::treeExpanded(ui, st, 3));
    UI_CHECK_EQ(w::treeVisibleRows(ui, st), 298u);
    w::setTreeExpanded(ui, st, 297, false);
    w::setTreeExpanded(ui, st, 0, false);
    UI_CHECK_EQ(w::treeVisibleRows(ui, st), 294u);
    w::setTreeExpanded(ui, st, 150, true);
    UI_CHECK_EQ(w::treeVisibleRows(ui, st), 296u);
    UI_CHECK(!w::treeExpanded(ui, st, 297) && !w::treeExpanded(ui, st, 0));
    return finish("fuse_ui_mv_tree");
}

/// A computed tree that never materialises: root -> "pool" (1,000,000 children, 5 leaves each) and "stream" (children
/// arrive 500 at a time through fetchMore, up to 200,000).
struct LazyTree {
    static constexpr w::ItemId kPool = 1, kStream = 2, kPoolBase = 10, kLeafBase = 2000000, kStreamBase = 100000000;
    static constexpr u32 kPoolSize = 1000000, kStreamMax = 200000, kBatch = 500;
    mutable u64 reads = 0, countCalls = 0, fetches = 0;
    mutable u32 streamFetched = 0;
    w::ItemModel model;
    LazyTree() {
        model.user = this;
        model.columns = 2;
        model.childCount = [](const void* u, w::ItemId p) -> u32 {
            const auto* m = static_cast<const LazyTree*>(u);
            ++m->countCalls;
            if (p == w::kRootItem) return 2;
            if (p == kPool) return kPoolSize;
            if (p == kStream) return m->streamFetched;
            if (p >= kPoolBase && p < kPoolBase + kPoolSize) return 5;
            return 0;
        };
        model.childAt = [](const void*, w::ItemId p, u32 r) -> w::ItemId {
            if (p == w::kRootItem) return r == 0 ? kPool : kStream;
            if (p == kPool) return kPoolBase + r;
            if (p == kStream) return kStreamBase + r;
            return kLeafBase + (p - kPoolBase) * 5 + r;
        };
        model.hasChildren = [](const void*, w::ItemId i) { return i == kPool || i == kStream || (i >= kPoolBase && i < kPoolBase + kPoolSize); };
        model.itemData = [](const void* u, w::ItemId i, u32 c, std::string& sc) -> std::string_view {
            ++static_cast<const LazyTree*>(u)->reads;
            if (i == kPool) return c ? "1000000" : "pool";
            if (i == kStream) return c ? "lazy" : "stream";
            char b[40];
            char* e = b;
            if (c) { e = fuse::text::toChars(b, b + sizeof b, u32(i % 9973)).ptr; }
            else if (i >= kStreamBase) { std::memcpy(b, "event ", 6); e = fuse::text::toChars(b + 6, b + sizeof b, u32(i - kStreamBase)).ptr; }
            else if (i >= kLeafBase) { std::memcpy(b, "part ", 5); e = fuse::text::toChars(b + 5, b + sizeof b, u32((i - kLeafBase) % 5)).ptr; }
            else { std::memcpy(b, "object ", 7); e = fuse::text::toChars(b + 7, b + sizeof b, u32(i - kPoolBase)).ptr; }
            sc.assign(b, e);
            return sc;
        };
        model.header = [](const void*, u32 c) -> std::string_view { return c ? "Value" : "Name"; };
        model.canFetchMore = [](const void* u, w::ItemId p) { return p == kStream && static_cast<const LazyTree*>(u)->streamFetched < kStreamMax; };
        model.fetchMore = [](const void* u, w::ItemId p) {
            const auto* m = static_cast<const LazyTree*>(u);
            if (p != kStream) return;
            m->streamFetched = std::min(kStreamMax, m->streamFetched + kBatch);
            ++m->fetches;
        };
    }
};
int gateTree1M() {
    UiContext ui({.viewport = {800, 600}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    LazyTree lt;
    const NodeHandle tree = w::treeModelView(ui, ui.root(), &lt.model, "lazy");
    ui.setCss(tree, "flex-grow", "1");
    ui.update();
    ui.setFocus(tree);
    const u32 nodes = ui.nodeCount();
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 2u);
    UI_CHECK(w::setTreeRowExpanded(ui, tree, 0, true)); // a million rows appear: one record
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 1000002u);
    UI_CHECK_EQ(w::treeExpandedCount(ui, tree), 1u);
    // expand 300 objects scattered over the million (from the back, so earlier rows do not shift)
    const auto e0 = std::chrono::steady_clock::now();
    for (int i = 299; i >= 0; --i) UI_CHECK(w::setTreeRowExpanded(ui, tree, 1u + u32(i) * 3331u, true));
    const double expandMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count();
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 1000002u + 1500u);
    UI_CHECK_EQ(w::treeExpandedCount(ui, tree), 301u);
    UI_CHECK(w::treeItemAt(ui, tree, 1) == LazyTree::kPoolBase && w::treeItemAt(ui, tree, 2) == LazyTree::kLeafBase);
    UI_CHECK(w::treeRowDepth(ui, tree, 2) == 2u && w::treeRowDepth(ui, tree, 7) == 1u);
    // the row after the 3rd expanded object: 3 * 3331 objects and 3 * 5 parts lie before it
    UI_CHECK(w::treeItemAt(ui, tree, 1u + 3u * 3331u + 15u) == LazyTree::kPoolBase + 3u * 3331u);
    const Vec2 mid = ctr(ui.rect(tree));
    ui.pointerMove(mid);
    using clock = std::chrono::steady_clock;
    for (int i = 0; i < 120; ++i) { ui.wheel(mid, {0.f, -3.f}); ui.update(1.f / 60.f); } // warm-up
    const unsigned long long a0 = g_allocs.load(), b0 = g_bytes.load();
    lt.reads = lt.countCalls = 0;
    std::vector<double> ms;
    const int frames = 900;
    ms.reserve(frames);
    for (int i = 0; i < frames; ++i) {
        const auto t0 = clock::now();
        if (i == frames / 3) key(ui, Key::End);
        else ui.wheel(mid, {0.f, i < frames / 3 ? -40.f : 25.f});
        ui.update(1.f / 60.f);
        ms.push_back(std::chrono::duration<double, std::milli>(clock::now() - t0).count());
    }
    const unsigned long long allocs = g_allocs.load() - a0, bytes = g_bytes.load() - b0;
    std::sort(ms.begin(), ms.end());
    const double med = ms[ms.size() / 2], p95 = ms[ms.size() * 95 / 100];
    const u32 vis = w::viewVisibleRows(ui, tree);
    std::printf("1M-node lazy tree: expand 300 in %.2f ms; %d frames, frame CPU median %.3f ms, p95 %.3f ms; %llu allocs (%llu bytes); "
                "%.1f cell reads/frame, %.1f childCount calls/frame; nodes %u, records %u\n", expandMs, frames, med, p95, allocs, bytes,
                double(lt.reads) / frames, double(lt.countCalls) / frames, ui.nodeCount(), w::treeExpandedCount(ui, tree));
    UI_CHECK(med <= 4.0);
    UI_CHECK(p95 <= 8.0);
    UI_CHECK(bytes <= 64 * 1024);
    UI_CHECK_EQ(ui.nodeCount(), nodes);                                   // no per-row nodes
    UI_CHECK(double(lt.reads) / frames <= double((vis + 2) * 2 + 4));     // only visible cells are read
    UI_CHECK(double(lt.countCalls) / frames <= double(vis + 4));          // and nothing walks the children
    UI_CHECK_EQ(w::treeExpandedCount(ui, tree), 301u);                    // state = expanded nodes, not rows
    // lazy children: the stream fetches a batch when it opens, then as its end scrolls into view
    key(ui, Key::End);
    const u32 last = w::viewRowCount(ui, tree) - 1;
    UI_CHECK(w::treeItemAt(ui, tree, last) == LazyTree::kStream);
    UI_CHECK_EQ(lt.fetches, 0ull);
    key(ui, Key::Right); // expand
    UI_CHECK_EQ(lt.fetches, 1ull);
    UI_CHECK_EQ(w::viewRowCount(ui, tree), last + 1u + LazyTree::kBatch);
    ui.update();
    UI_CHECK_EQ(lt.fetches, 1ull); // its end is not in view: nothing more is fetched
    key(ui, Key::End);             // ... now it is
    ui.update();
    UI_CHECK(lt.fetches >= 2ull && lt.fetches <= 5ull); // bounded per frame
    const u64 f1 = lt.fetches;
    for (int i = 0; i < 50; ++i) { key(ui, Key::End); }
    UI_CHECK(lt.fetches > f1 && lt.fetches <= f1 + 50ull * 5ull);
    UI_CHECK_EQ(w::viewRowCount(ui, tree), last + 1u + lt.streamFetched);
    UI_CHECK(lt.streamFetched < LazyTree::kStreamMax); // far from materialised
    UI_CHECK_EQ(ui.nodeCount(), nodes);
    // collapsing the pool drops every record below it
    key(ui, Key::Home);
    key(ui, Key::Left);
    UI_CHECK_EQ(w::treeExpandedCount(ui, tree), 1u); // only the stream is left
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 2u + lt.streamFetched);
    shot(ui, "tree_1m.png");
    return finish("fuse_ui_mv_tree_1m");
}

// ---- editing ----------------------------------------------------------------------------------------------------------------
struct EditModel { // Name (text), Count (number), Kind (one of a list), Done (check box), Id (read only)
    struct Row { std::string name; int count; std::string kind; bool done; };
    std::vector<Row> rows;
    w::ModelNotifier notifier;
    w::ItemModel model;
    int setCalls = 0;
    bool rejectEmpty = false;
    EditModel() {
        for (int i = 0; i < 60; ++i) rows.push_back({"row " + std::to_string(i), i * 2, i % 2 ? "mesh" : "light", i % 3 == 0});
        model.user = this;
        model.columns = 5;
        model.notifier = &notifier;
        model.rows = [](const void* u) { return u32(static_cast<const EditModel*>(u)->rows.size()); };
        model.data = [](const void* u, u32 r, u32 c, std::string& sc) -> std::string_view {
            const Row& row = static_cast<const EditModel*>(u)->rows[r];
            switch (c) {
            case 0: return row.name;
            case 1: sc = std::to_string(row.count); return sc;
            case 2: return row.kind;
            case 3: return {};
            default: sc = "#" + std::to_string(r); return sc;
            }
        };
        model.header = [](const void*, u32 c) -> std::string_view {
            static constexpr std::string_view h[] = {"Name", "Count", "Kind", "Done", "Id"};
            return h[c];
        };
        model.flags = [](const void*, w::ItemId, u32 c) -> u32 {
            u32 f = w::kItemSelectable | w::kItemEnabled;
            if (c <= 2) f |= w::kItemEditable;
            if (c == 3) f |= w::kItemCheckable;
            return f;
        };
        model.role = [](const void* u, w::ItemId i, u32 c, w::ItemRole role, w::ItemValue& out, std::string&) {
            const Row& row = static_cast<const EditModel*>(u)->rows[std::size_t(i)];
            if (role == w::ItemRole::CheckState && c == 3) { out.integer = row.done ? 1 : 0; return true; }
            if (role == w::ItemRole::Edit && c == 1) { out.number = row.count; out.text = {}; return false; } // falls back to the text
            return false;
        };
        model.setData = [](const void* u, w::ItemId i, u32 c, w::ItemRole role, const w::ItemValue& v) {
            auto* m = const_cast<EditModel*>(static_cast<const EditModel*>(u));
            Row& row = m->rows[std::size_t(i)];
            ++m->setCalls;
            if (role == w::ItemRole::CheckState && c == 3) row.done = v.integer != 0;
            else if (role != w::ItemRole::Edit) return false;
            else if (c == 0) { if (m->rejectEmpty && v.text.empty()) return false; row.name.assign(v.text); }
            else if (c == 1) row.count = int(v.number);
            else if (c == 2) row.kind.assign(v.text);
            else return false;
            m->notifier.dataChanged(u32(i), 1, c, c);
            return true;
        };
    }
};
int gateEditing() {
    UiContext ui({.viewport = {760, 480}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    EditModel m;
    const NodeHandle t = w::tableView(ui, ui.root(), &m.model, "t");
    ui.setCss(t, "flex-grow", "1");
    const NodeHandle other = w::button(ui, ui.root(), "Elsewhere", "other");
    static const w::SpinEditorSpec spin{0.f, 500.f, 1.f};
    static const std::string kinds = "light\nmesh\ncamera\nprobe";
    static const w::CellDelegate spinDelegate = w::spinBoxDelegate(&spin), comboDelegate = w::comboBoxDelegate(&kinds);
    w::setTableDelegate(ui, t, 1, &spinDelegate);
    w::setTableDelegate(ui, t, 2, &comboDelegate);
    ui.update();
    const u32 nodes = ui.nodeCount();
    // double click opens a line edit over the cell: one extra node. An editable cell edits, it does not activate
    // the row (Submitted is for double clicks on cells that are not editable, and for Enter)
    int submitted = 0;
    ui.connect(t, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
    click(ui, cellPt(ui, t, 2, 0));
    UI_CHECK(!w::viewEditor(ui, t).valid());
    click(ui, cellPt(ui, t, 2, 0));
    UI_CHECK_EQ(submitted, 0);
    NodeHandle ed = w::viewEditor(ui, t);
    UI_CHECK(ed.valid() && ui.focused() == ed && ui.parent(ed) == t);
    UI_CHECK_EQ(ui.nodeCount(), nodes + 1u);
    UI_CHECK(ui.classOf(ed) == ui.registry().find("lineedit"));
    UI_CHECK(ui.editBuffer(ed).text() == "row 2");
    {
        const Rect cell = w::viewCellRect(ui, t, 2, 0), er = ui.rect(ed);
        UI_CHECK(std::abs(er.x - cell.x) <= 1.f && std::abs(er.y - cell.y) <= 1.f && std::abs(er.w - cell.w) <= 1.f && std::abs(er.h - cell.h) <= 1.f);
    }
    ui.textInput("renamed"); // the old text is selected: typing replaces it
    ui.update();
    shot(ui, "editing.png");
    key(ui, Key::Enter);
    UI_CHECK_EQ(m.rows[2].name, std::string("renamed"));
    UI_CHECK(!w::viewEditor(ui, t).valid() && ui.focused() == t);
    UI_CHECK_EQ(ui.nodeCount(), nodes);
    UI_CHECK_EQ(submitted, 0); // Enter went to the editor
    settle(ui);
    w::setViewEditTriggers(ui, t, w::kEditAll & ~u32(w::kEditDoubleClick)); // without the trigger the row activates
    click(ui, cellPt(ui, t, 2, 0));
    click(ui, cellPt(ui, t, 2, 0));
    UI_CHECK(!w::viewEditor(ui, t).valid());
    UI_CHECK_EQ(submitted, 1);
    w::setViewEditTriggers(ui, t, w::kEditAll);
    settle(ui);
    // F2 edits the current cell; Escape cancels
    clickCell(ui, t, 4, 0);
    key(ui, Key::F2);
    ed = w::viewEditor(ui, t);
    UI_CHECK(ed.valid());
    ui.textInput("discarded");
    key(ui, Key::Escape);
    UI_CHECK(!w::viewEditor(ui, t).valid() && m.rows[4].name == "row 4" && ui.focused() == t);
    // typing on an editable cell opens the editor with the typed text
    const int calls = m.setCalls;
    ui.textInput("Z");
    ui.update();
    ed = w::viewEditor(ui, t);
    UI_CHECK(ed.valid() && ui.editBuffer(ed).text() == "Z");
    ui.textInput("ed");
    key(ui, Key::Enter);
    UI_CHECK(m.rows[4].name == "Zed" && m.setCalls == calls + 1);
    // Tab commits and moves to the next editable cell: the spin box column
    key(ui, Key::F2);
    ui.textInput("tabbed");
    key(ui, Key::Tab);
    UI_CHECK_EQ(m.rows[4].name, std::string("tabbed"));
    ed = w::viewEditor(ui, t);
    UI_CHECK(ed.valid() && ui.classOf(ed) == ui.registry().find("spinbox"));
    UI_CHECK_EQ(ui.value(ed), 8.f); // row 4's count
    UI_CHECK(w::viewCurrentColumn(ui, t) == 1u && w::viewCurrentRow(ui, t) == 4);
    ui.setValue(ed, 123.f, true);
    key(ui, Key::Tab); // -> the combo column
    UI_CHECK_EQ(m.rows[4].count, 123);
    ed = w::viewEditor(ui, t);
    UI_CHECK(ed.valid() && ui.classOf(ed) == ui.registry().find("combobox"));
    UI_CHECK_EQ(std::string(w::comboText(ui, ed)), std::string("light")); // row 4's kind is preselected
    ui.setValue(ed, 2.f, true);
    key(ui, Key::Tab); // Done / Id are not editable: on to the next row's name
    UI_CHECK_EQ(m.rows[4].kind, std::string("camera"));
    ed = w::viewEditor(ui, t);
    UI_CHECK(ed.valid() && w::viewCurrentRow(ui, t) == 5 && w::viewCurrentColumn(ui, t) == 0u);
    key(ui, Key::Tab, kModShift); // and back
    UI_CHECK(w::viewCurrentRow(ui, t) == 4 && w::viewCurrentColumn(ui, t) == 2u);
    key(ui, Key::Escape);
    UI_CHECK_EQ(ui.nodeCount(), nodes);
    // check boxes toggle in place, with no editor
    {
        const Rect cell = w::viewCellRect(ui, t, 6, 3);
        UI_CHECK(m.rows[6].done);
        click(ui, {cell.x + 12.f, cell.y + cell.h * 0.5f});
        UI_CHECK(!m.rows[6].done && !w::viewEditor(ui, t).valid());
        settle(ui);
        key(ui, Key::Space); // the current cell's box
        UI_CHECK(m.rows[6].done);
        click(ui, {cell.x + cell.w - 10.f, cell.y + cell.h * 0.5f}); // beside the box: selects only
        UI_CHECK(m.rows[6].done);
        settle(ui);
    }
    // read-only cells do not open; programmatic edit / commit / cancel
    UI_CHECK(!w::viewEdit(ui, t, 1, 4));
    UI_CHECK(w::viewEdit(ui, t, 1, 0));
    ui.editBuffer(w::viewEditor(ui, t)).setText("by api");
    UI_CHECK(w::viewCommitEdit(ui, t));
    UI_CHECK_EQ(m.rows[1].name, std::string("by api"));
    m.rejectEmpty = true;
    UI_CHECK(w::viewEdit(ui, t, 1, 0));
    ui.editBuffer(w::viewEditor(ui, t)).setText("");
    UI_CHECK(!w::viewCommitEdit(ui, t)); // the model said no
    UI_CHECK_EQ(m.rows[1].name, std::string("by api"));
    // a click elsewhere commits
    UI_CHECK(w::viewEdit(ui, t, 3, 0));
    ui.textInput("clicked away");
    click(ui, ctr(ui.rect(other)));
    UI_CHECK(m.rows[3].name == "clicked away" && !w::viewEditor(ui, t).valid());
    // the editor follows its cell while it is in the window and commits when it scrolls out
    UI_CHECK(w::viewEdit(ui, t, 2, 0));
    ed = w::viewEditor(ui, t);
    ui.update();
    const f32 y0 = ui.rect(ed).y;
    ui.wheel(ctr(ui.rect(t)), {0.f, -1.f / 3.f}); // one row
    ui.update();
    UI_CHECK(w::viewEditor(ui, t) == ed && ui.rect(ed).y < y0 - 10.f);
    ui.editBuffer(ed).setText("scrolled out");
    ui.wheel(ctr(ui.rect(t)), {0.f, -5.f});
    ui.update();
    UI_CHECK(!w::viewEditor(ui, t).valid() && m.rows[2].name == "scrolled out");
    UI_CHECK_EQ(ui.nodeCount(), nodes);
    // edit triggers can be switched off
    w::setViewFirstVisible(ui, t, 0);
    ui.update();
    w::setViewEditTriggers(ui, t, w::kEditKeyF2);
    click(ui, cellPt(ui, t, 7, 0));
    click(ui, cellPt(ui, t, 7, 0));
    UI_CHECK(!w::viewEditor(ui, t).valid());
    key(ui, Key::F2);
    UI_CHECK(w::viewEditor(ui, t).valid());
    key(ui, Key::Escape);
    // editing a tree model's cell goes to the node id
    w::StringTreeModel tm("Name\tNote");
    const w::ItemId a = tm.add(w::kRootItem, "a\t1");
    const w::ItemId b = tm.add(a, "b\t2");
    tm.setEditable(true);
    const NodeHandle tree = w::treeModelView(ui, ui.root(), &tm.model(), "tree");
    ui.update();
    w::setTreeRowExpanded(ui, tree, 0, true);
    ui.update();
    UI_CHECK(w::viewEdit(ui, tree, 1, 0));
    ui.editBuffer(w::viewEditor(ui, tree)).setText("child");
    key(ui, Key::Enter);
    UI_CHECK(tm.cell(b, 0) == "child" && tm.cell(a, 0) == "a");
    return finish("fuse_ui_mv_editing");
}

// ---- view features ----------------------------------------------------------------------------------------------------------
struct RoleModel { // 5 columns; roles on a few cells
    w::ItemModel model;
    RoleModel() {
        model.user = this;
        model.columns = 5;
        model.rows = [](const void*) { return 300u; };
        model.data = [](const void*, u32 r, u32 c, std::string& sc) -> std::string_view {
            if (c == 1 && r == 2) return "a very long cell text that cannot possibly fit into its column";
            sc = "r" + std::to_string(r) + "c" + std::to_string(c);
            return sc;
        };
        model.header = [](const void*, u32 c) -> std::string_view {
            static constexpr std::string_view h[] = {"Zero", "One", "Two", "Three", "Four"};
            return h[c];
        };
        model.role = [](const void*, w::ItemId i, u32 c, w::ItemRole role, w::ItemValue& out, std::string&) {
            if (role == w::ItemRole::Background && i == 1 && c == 2) { out.color = Color::rgb(0x00C800); return true; }
            if (role == w::ItemRole::Foreground && i == 3 && c == 0) { out.color = Color::rgb(0xFF4040); return true; }
            if (role == w::ItemRole::Decoration && c == 0 && i < 6) { out.color = Color::rgb(0x3060FF); out.integer = i32(i); return true; }
            if (role == w::ItemRole::TextAlign && c == 1 && i != 2) { out.integer = i32(TextAlignment::End); return true; }
            if (role == w::ItemRole::ToolTip && i == 4 && c == 2) { out.text = "explicit tip"; return true; }
            return false;
        };
    }
};
int gateView() {
    UiContext ui({.viewport = {460, 420}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    RoleModel m;
    const NodeHandle t = w::tableView(ui, ui.root(), &m.model, "t");
    ui.setCss(t, "flex-grow", "1");
    ui.update();
    const Rect c = ui.contentRect(t);
    const f32 total = 120.f * 4.f + 160.f;
    // the columns are wider than the view: horizontal scrolling
    UI_CHECK_EQ(w::viewScrollX(ui, t), 0.f);
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 0).x, c.x);
    UI_CHECK(w::viewCellRect(ui, t, 0, 4).empty()); // off to the right
    const u32 visH = w::viewVisibleRows(ui, t);
    ui.setModifiers(kModShift);
    ui.wheel(ctr(c), {0.f, -1.f});
    ui.setModifiers(0);
    ui.update();
    UI_CHECK_EQ(w::viewScrollX(ui, t), 40.f);
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 1).x, c.x + 80.f);
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 0).w, 80.f); // clipped at the view edge
    w::setViewScrollX(ui, t, 1e6f);
    const f32 maxX = total - c.w + 8.f; // the vertical bar takes 8 px
    UI_CHECK_EQ(w::viewScrollX(ui, t), maxX);
    UI_CHECK(!w::viewCellRect(ui, t, 0, 4).empty() && w::viewCellRect(ui, t, 0, 0).empty());
    ui.setFocus(t);
    key(ui, Key::Left);
    UI_CHECK_EQ(w::viewScrollX(ui, t), maxX - 40.f);
    // the bar at the bottom drags
    drag(ui, {c.x + c.w * 0.5f, c.bottom() - 4.f}, {c.x - 50.f, c.bottom() - 4.f});
    UI_CHECK_EQ(w::viewScrollX(ui, t), 0.f);
    drag(ui, {c.x + 20.f, c.bottom() - 4.f}, {c.right() + 80.f, c.bottom() - 4.f});
    UI_CHECK_EQ(w::viewScrollX(ui, t), maxX);
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), 0u); // and never the rows
    // frozen first column: it stays, the rest scrolls under it
    w::setViewFrozenColumns(ui, t, 1);
    w::setViewScrollX(ui, t, 180.f);
    ui.update();
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 0).x, c.x);
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 0).w, 120.f);
    UI_CHECK(w::viewCellRect(ui, t, 0, 1).empty());                 // entirely under the frozen column
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 2).x, c.x + 120.f);       // clipped at its edge (240 - 180 = 60 px in)
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 2).w, 60.f);
    {
        click(ui, {c.x + 60.f, cellPt(ui, t, 3, 0).y}); // a click on the frozen column hits column 0
        UI_CHECK(i32(ui.value(t)) == 3 && w::viewCurrentColumn(ui, t) == 0u);
        click(ui, {c.x + 130.f, cellPt(ui, t, 4, 0).y});
        UI_CHECK(i32(ui.value(t)) == 4 && w::viewCurrentColumn(ui, t) == 2u);
        settle(ui);
    }
    shot(ui, "view_frozen.png");
    w::setViewFrozenColumns(ui, t, 0);
    w::setViewScrollX(ui, t, 0.f);
    // hidden columns
    w::setViewColumnHidden(ui, t, 1, true);
    ui.update();
    UI_CHECK(w::viewColumnHidden(ui, t, 1) && w::viewCellRect(ui, t, 0, 1).empty());
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 2).x, c.x + 120.f);
    w::setViewColumnHidden(ui, t, 3, true);
    w::setViewColumnHidden(ui, t, 4, true);
    ui.update();
    UI_CHECK(w::viewVisibleRows(ui, t) >= visH && w::viewVisibleRows(ui, t) <= visH + 1u); // no bar at the bottom any more
    w::setViewScrollX(ui, t, 100.f);
    UI_CHECK_EQ(w::viewScrollX(ui, t), 0.f); // nothing to scroll
    for (u32 k : {1u, 3u, 4u}) w::setViewColumnHidden(ui, t, k, false);
    ui.update();
    // reordering: by API, and by dragging a header
    w::moveViewColumn(ui, t, 0, 2);
    UI_CHECK(w::viewColumnAt(ui, t, 0) == 1u && w::viewColumnAt(ui, t, 1) == 2u && w::viewColumnAt(ui, t, 2) == 0u);
    UI_CHECK_EQ(w::viewColumnVisual(ui, t, 0), 2u);
    UI_CHECK_EQ(w::viewCellRect(ui, t, 0, 0).x, c.x + 240.f);
    w::moveViewColumn(ui, t, 2, 0);
    UI_CHECK_EQ(w::viewColumnAt(ui, t, 0), 0u);
    int sorts = 0, sortCol = -1;
    ui.connect(t, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& a) { sorts += a.text == "sort"; sortCol = i32(a.value); });
    drag(ui, {c.x + 60.f, c.y + 8.f}, {c.x + 300.f, c.y + 8.f}); // not movable: a press sorts, a drag does nothing more
    UI_CHECK(sorts == 1 && sortCol == 0 && w::viewColumnAt(ui, t, 0) == 0u);
    w::setViewColumnsMovable(ui, t, true);
    drag(ui, {c.x + 60.f, c.y + 8.f}, {c.x + 200.f, c.y + 8.f}); // column 0 dropped past the middle of column 1
    UI_CHECK(w::viewColumnAt(ui, t, 0) == 1u && w::viewColumnAt(ui, t, 1) == 0u && w::viewColumnAt(ui, t, 2) == 2u);
    UI_CHECK_EQ(sorts, 1); // a drag is not a click
    click(ui, {c.x + 60.f, c.y + 8.f}); // a click still sorts: the column now under the pointer is model column 1
    UI_CHECK(sorts == 2 && sortCol == 1);
    drag(ui, {c.x + 300.f, c.y + 8.f}, {c.x + 10.f, c.y + 8.f}); // column 2 to the front
    UI_CHECK(w::viewColumnAt(ui, t, 0) == 2u && w::viewColumnAt(ui, t, 1) == 1u && w::viewColumnAt(ui, t, 2) == 0u);
    w::moveViewColumn(ui, t, 0, 2);
    w::moveViewColumn(ui, t, 0, 1); // back to 0 1 2
    UI_CHECK(w::viewColumnAt(ui, t, 0) == 0u && w::viewColumnAt(ui, t, 1) == 1u && w::viewColumnAt(ui, t, 2) == 2u);
    w::setViewColumnsMovable(ui, t, false);
    w::setViewSortIndicator(ui, t, 2, false);
    settle(ui);
    // resize to contents: by API and by double-clicking the header edge
    const f32 fs = ui.computed(t).fontSize;
    w::resizeViewColumnToContents(ui, t, 1);
    const f32 want = std::ceil(ui.measureString("a very long cell text that cannot possibly fit into its column", fs)) + 14.f;
    UI_CHECK_EQ(w::tableColumnWidth(ui, t, 1), want);
    w::setTableColumnWidth(ui, t, 0, 300.f);
    click(ui, {c.x + 300.f, c.y + 8.f});
    click(ui, {c.x + 300.f, c.y + 8.f});
    settle(ui);
    UI_CHECK(w::tableColumnWidth(ui, t, 0) < 100.f && w::tableColumnWidth(ui, t, 0) > 40.f); // "r10c0" + the swatch
    w::setTableColumnWidth(ui, t, 0, 120.f);
    w::setTableColumnWidth(ui, t, 1, 120.f);
    ui.update();
    // row height, alternating rows
    const u32 vis0 = w::viewVisibleRows(ui, t);
    w::setViewRowHeight(ui, t, 40.f);
    ui.update();
    UI_CHECK(w::viewVisibleRows(ui, t) < vis0 && w::viewCellRect(ui, t, 0, 0).h == 40.f);
    UI_CHECK_EQ(w::viewVisibleRows(ui, t), u32((c.h - (w::viewCellRect(ui, t, 0, 0).y - c.y) - 8.f) / 40.f));
    w::setViewRowHeight(ui, t, 0.f);
    ui.update();
    UI_CHECK_EQ(w::viewVisibleRows(ui, t), vis0);
    const u32 cmds = ui.stats().drawCmds;
    w::setViewAlternatingRows(ui, t, false);
    ui.update();
    UI_CHECK(ui.stats().drawCmds < cmds);
    w::setViewAlternatingRows(ui, t, true);
    // typed roles reach the pixels
    {
        ui.pointerMove({-10.f, -10.f});
        w::setViewCurrent(ui, t, 8);
        ui.update();
        Image img;
        render(ui, img);
        auto px = [&](Vec2 p) { return img.pixels[std::size_t(int(p.y)) * std::size_t(img.width) + std::size_t(int(p.x))]; };
        const Rect bg = w::viewCellRect(ui, t, 1, 2);
        const u32 green = px({bg.x + bg.w - 8.f, bg.y + 3.f});
        UI_CHECK(ch(green, 1) > 180 && ch(green, 0) < 40 && ch(green, 2) < 40); // Background role
        const Rect sw = w::viewCellRect(ui, t, 0, 0);
        const u32 blue = px({sw.x + 12.f, sw.y + sw.h * 0.5f});
        UI_CHECK(ch(blue, 2) > 200 && ch(blue, 0) < 80); // Decoration swatch
        // TextAlign End: the right part of the cell has ink, the left part has none
        const Rect al = w::viewCellRect(ui, t, 5, 1);
        auto ink = [&](f32 x0, f32 x1) {
            int n = 0;
            for (f32 y = al.y + 2.f; y < al.bottom() - 2.f; y += 1.f)
                for (f32 x = x0; x < x1; x += 1.f) n += ch(px({x, y}), 0) > 120;
            return n;
        };
        UI_CHECK(ink(al.x + 6.f, al.x + 40.f) == 0 && ink(al.right() - 50.f, al.right() - 6.f) > 10);
        const Rect fg = w::viewCellRect(ui, t, 3, 0); // Foreground: red text
        int red = 0;
        for (f32 y = fg.y + 2.f; y < fg.bottom() - 2.f; y += 1.f)
            for (f32 x = fg.x + 22.f; x < fg.x + 70.f; x += 1.f) { const u32 p = px({x, y}); red += ch(p, 0) > 150 && ch(p, 1) < 110; }
        UI_CHECK(red > 10);
    }
    shot(ui, "view_roles.png");
    // tooltips: an elided cell shows its text after the delay; a cell that fits shows none; a ToolTip role wins
    UI_CHECK(!w::viewToolTip(ui, t).valid());
    ui.pointerMove(cellPt(ui, t, 2, 1));
    ui.update(0.2f);
    UI_CHECK(!w::viewToolTip(ui, t).valid());
    ui.update(0.4f);
    const NodeHandle tip = w::viewToolTip(ui, t);
    UI_CHECK(tip.valid() && ui.computed(tip).visible);
    UI_CHECK(ui.text(tip) == "a very long cell text that cannot possibly fit into its column");
    ui.pointerMove(cellPt(ui, t, 3, 1)); // fits
    ui.update(1.f);
    ui.update(1.f);
    UI_CHECK(!ui.computed(tip).visible);
    ui.pointerMove(cellPt(ui, t, 4, 2));
    ui.update(0.6f);
    ui.update();
    UI_CHECK(ui.computed(tip).visible && ui.text(tip) == "explicit tip");
    const u32 nodesWithTip = ui.nodeCount();
    ui.pointerMove(cellPt(ui, t, 2, 1));
    ui.update(0.6f);
    ui.update();
    UI_CHECK(ui.computed(tip).visible && ui.nodeCount() == nodesWithTip); // one tooltip node, reused
    w::setViewToolTips(ui, t, false);
    ui.update();
    UI_CHECK(!ui.computed(tip).visible);
    // the context-menu hook
    struct Menu { int calls = 0; u32 row = 0, col = 0; w::ItemId item = 0; Vec2 at; } menu;
    w::setViewContextMenu(ui, t, [](void* u, UiContext&, NodeHandle, u32 row, w::ItemId item, u32 col, Vec2 at) {
        auto* mm = static_cast<Menu*>(u);
        ++mm->calls;
        mm->row = row;
        mm->col = col;
        mm->item = item;
        mm->at = at;
    }, &menu);
    const Vec2 rp = cellPt(ui, t, 6, 2);
    ui.pointerMove(rp);
    ui.pointerDown(rp, MouseButton::Right);
    ui.pointerUp(rp, MouseButton::Right);
    ui.update();
    UI_CHECK(menu.calls == 1 && menu.row == 6u && menu.col == 2u && menu.item == 6u && menu.at == rp);
    UI_CHECK_EQ(i32(ui.value(t)), 6); // the cell under the menu became current
    return finish("fuse_ui_mv_view");
}

// ---- proxies ------------------------------------------------------------------------------------------------------------------
int gateProxy() {
    w::StringTableModel m("Name\tKind\tSize");
    const char* kinds[] = {"mesh", "texture", "audio"};
    for (int i = 0; i < 300; ++i) m.addRow("asset_" + num(i) + "\t" + kinds[i % 3] + "\t" + num((i * 37) % 50));
    std::string sc;
    auto cell = [&](const w::ItemModel& mm, u32 r, u32 c) { return std::string(mm.data(mm.user, r, c, sc)); };
    // multi-key sort: kind ascending, then size descending; remaining ties keep source order (stable)
    w::SortFilterProxy p(&m.model());
    p.sortBy(1, true);
    p.addSortKey(2, false);
    UI_CHECK_EQ(p.sortKeyCount(), 2u);
    bool ok = true;
    for (u32 r = 1; r < p.rows(); ++r) {
        const std::string k0 = cell(p.model(), r - 1, 1), k1 = cell(p.model(), r, 1);
        const int s0 = std::stoi(cell(p.model(), r - 1, 2)), s1 = std::stoi(cell(p.model(), r, 2));
        ok &= k0 < k1 || (k0 == k1 && (s0 > s1 || (s0 == s1 && p.sourceRow(r - 1) < p.sourceRow(r))));
    }
    UI_CHECK(ok);
    UI_CHECK(cell(p.model(), 0, 1) == "audio" && cell(p.model(), 0, 2) == "49");
    p.sortBy(-1, true);
    UI_CHECK(p.sortKeyCount() == 0u && p.sourceRow(17) == 17u);
    // a filter over every column
    p.setFilterAllColumns("TEXT");
    UI_CHECK_EQ(p.rows(), 100u);
    p.setFilterAllColumns("7"); // names and sizes containing a 7
    u32 want = 0;
    for (int i = 0; i < 300; ++i) want += num(i).find('7') != std::string::npos || num((i * 37) % 50).find('7') != std::string::npos;
    UI_CHECK(p.rows() == want && want > 0u);
    // wildcards
    p.setFilterWildcard(0, "asset_1?");
    UI_CHECK_EQ(p.rows(), 10u);
    p.setFilterWildcard(0, "*_2*5");
    UI_CHECK_EQ(p.rows(), 11u); // 25, 205 .. 295
    p.setFilterWildcard(-1, "AUD*");
    UI_CHECK_EQ(p.rows(), 100u);
    p.setFilterWildcard(0, "set_29"); // no wildcard: contains
    UI_CHECK_EQ(p.rows(), 11u);
    p.setFilter(0, "");
    UI_CHECK_EQ(p.rows(), 300u);

    // proxy over proxy over a model with a notifier: the source's reports rebuild both and reach the view
    VecModel src;
    for (int i = 0; i < 100; ++i) src.rows.push_back((i % 2 ? "odd " : "even ") + num(i));
    w::SortFilterProxy filter(&src.model);
    w::SortFilterProxy sorter(&filter.model());
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    const NodeHandle t = w::tableView(ui, ui.root(), &sorter.model(), "t");
    ui.setCss(t, "flex-grow", "1");
    ui.update();
    UI_CHECK_EQ(w::viewRowCount(ui, t), 100u); // both identities: straight through
    src.rows.push_back("odd tail");
    src.notifier.rowsInserted(100, 1);
    UI_CHECK_EQ(w::viewRowCount(ui, t), 101u);
    filter.setFilter(0, "odd");
    sorter.sortBy(1, false); // by text length, longest first
    UI_CHECK_EQ(w::viewRowCount(ui, t), 51u);
    UI_CHECK_EQ(cell(sorter.model(), 0, 0), std::string("odd tail"));
    UI_CHECK_EQ(cell(sorter.model(), 50, 0), std::string("odd 9"));
    UI_CHECK_EQ(filter.sourceRow(sorter.sourceRow(0)), 100u);
    w::setViewFirstVisible(ui, t, 40);
    src.rows.erase(src.rows.begin() + 20, src.rows.end());
    src.notifier.rowsRemoved(20, 81);
    ui.update();
    UI_CHECK_EQ(w::viewRowCount(ui, t), 10u); // no tableModelChanged call
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), 0u);
    src.rows[1] = "now even";
    src.notifier.dataChanged(1);
    UI_CHECK_EQ(sorter.rows(), 9u); // a changed value can leave the filter
    // roles / flags / edits map to the source row
    EditModel em;
    w::SortFilterProxy ep(&em.model);
    ep.setFilter(2, "mesh"); // source rows 1, 3, 5 ...
    ep.sortBy(1, false);     // count descending: source row 59 first
    UI_CHECK(ep.model().flags && ep.model().setData && ep.model().role);
    w::ItemValue v;
    v.text = "edited through the proxy";
    UI_CHECK(ep.model().setData(ep.model().user, 0, 0, w::ItemRole::Edit, v));
    UI_CHECK_EQ(em.rows[59].name, std::string("edited through the proxy"));
    UI_CHECK(ep.model().flags(ep.model().user, 0, 3) & w::kItemCheckable);

    // tree filter: matches keep their ancestors
    w::StringTreeModel tm("Name\tKind");
    const w::ItemId scene = tm.add(w::kRootItem, "Scene\tgroup");
    const w::ItemId lights = tm.add(scene, "Lights\tgroup");
    const w::ItemId sun = tm.add(lights, "Sun\tlight");
    tm.add(lights, "Fill\tlight");
    const w::ItemId meshes = tm.add(scene, "Meshes\tgroup");
    tm.add(meshes, "Terrain\tmesh");
    const w::ItemId sundial = tm.add(meshes, "Sundial\tmesh");
    const w::ItemId assets = tm.add(w::kRootItem, "Assets\tgroup");
    tm.add(assets, "Rock\tmesh");
    w::TreeFilterProxy tf(&tm.model());
    const w::ItemModel& fm = tf.model();
    UI_CHECK(fm.childCount(fm.user, w::kRootItem) == 2u && fm.childCount(fm.user, lights) == 2u); // no filter: the source
    const NodeHandle tree = w::treeModelView(ui, ui.root(), &fm, "tree");
    ui.update();
    w::setTreeRowExpanded(ui, tree, 0, true);
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 4u);
    tf.setFilter(0, "sun");
    UI_CHECK_EQ(tf.keptCount(), 5u); // Scene, Lights, Sun, Meshes, Sundial
    UI_CHECK(fm.childCount(fm.user, w::kRootItem) == 1u && fm.childAt(fm.user, w::kRootItem, 0) == scene);
    UI_CHECK(fm.childCount(fm.user, lights) == 1u && fm.childAt(fm.user, lights, 0) == sun);
    UI_CHECK(fm.childCount(fm.user, meshes) == 1u && fm.childAt(fm.user, meshes, 0) == sundial);
    UI_CHECK(tf.matches(sun) && tf.matches(sundial) && !tf.matches(scene) && !tf.matches(lights));
    UI_CHECK(fm.hasChildren(fm.user, scene) && !fm.hasChildren(fm.user, sun) && !fm.hasChildren(fm.user, assets));
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 1u); // the filter's reset collapsed the view
    w::setTreeRowExpanded(ui, tree, 0, true);
    w::setTreeRowExpanded(ui, tree, 2, true);
    w::setTreeRowExpanded(ui, tree, 1, true);
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 5u);
    UI_CHECK(w::treeItemAt(ui, tree, 2) == sun && w::treeItemAt(ui, tree, 4) == sundial);
    tf.setFilter(1, "light"); // by kind
    UI_CHECK(tf.keptCount() == 4u && tf.matches(sun) && !tf.matches(sundial));
    tf.setFilter(-1, "rock"); // any column
    UI_CHECK(tf.keptCount() == 2u && fm.childAt(fm.user, w::kRootItem, 0) == assets);
    tm.add(lights, "Rocket light\tlight"); // the source reports: the filter rebuilds
    UI_CHECK(tf.keptCount() == 5u && fm.childCount(fm.user, w::kRootItem) == 2u);
    tf.setFilter(0, "");
    UI_CHECK(fm.childCount(fm.user, w::kRootItem) == 2u && fm.childCount(fm.user, lights) == 3u);
    return finish("fuse_ui_mv_proxy");
}

// ---- async ------------------------------------------------------------------------------------------------------------------
struct FakeSource { // answers when told to
    struct Req { u32 first = 0, count = 0; u64 ticket = 0; };
    std::vector<Req> pending;
    static void fetch(void* u, w::AsyncModel&, u32 first, u32 count, u64 ticket) { static_cast<FakeSource*>(u)->pending.push_back({first, count, ticket}); }
    static std::vector<std::string> cells(const Req& r) {
        std::vector<std::string> out;
        for (u32 i = 0; i < r.count; ++i)
            for (u32 c = 0; c < 3; ++c) out.push_back("r" + std::to_string(r.first + i) + "c" + std::to_string(c));
        return out;
    }
    /// Answers the request for the page that starts at `first`; false when there is none.
    bool answer(w::AsyncModel& m, u32 first) {
        for (std::size_t i = 0; i < pending.size(); ++i)
            if (pending[i].first == first) {
                const Req r = pending[i];
                pending.erase(pending.begin() + std::ptrdiff_t(i));
                return m.deliver(r.ticket, cells(r));
            }
        return false;
    }
};
int gateAsync() {
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    FakeSource src;
    w::AsyncModel am(ui, 3, 32, 8);
    am.setSource(&FakeSource::fetch, &src);
    am.setHeaders("A\tB\tC");
    am.setRowCount(100000);
    const NodeHandle t = w::tableView(ui, ui.root(), &am.model(), "t");
    ui.setCss(t, "flex-grow", "1");
    ui.update();
    std::string sc;
    auto cell = [&](u32 r, u32 c) { return std::string(am.model().data(am.model().user, r, c, sc)); };
    // the window was read once: one request per page, placeholder cells
    const u32 vis = w::viewVisibleRows(ui, t);
    const std::size_t pages = (vis + 1 + 31) / 32;
    UI_CHECK_EQ(src.pending.size(), pages);
    UI_CHECK_EQ(am.pendingRequests(), u32(pages));
    UI_CHECK(!am.loaded(0) && cell(0, 0) == "...");
    ui.invalidate(t);
    ui.update();
    ui.update();
    UI_CHECK_EQ(am.fetchCount(), u64(pages)); // repaints do not ask again
    // an answer is committed on the owner thread, at its next update, and repaints the rows
    UI_CHECK(src.answer(am, 0));
    UI_CHECK(!am.loaded(0)); // delivered, not yet committed
    ui.update();
    UI_CHECK(am.loaded(0) && cell(0, 0) == "r0c0" && cell(31, 2) == "r31c2");
    UI_CHECK(ui.stats().repainted);
    if (pages > 1) { src.answer(am, 32); ui.update(); }
    UI_CHECK_EQ(am.pendingRequests(), 0u);
    ui.update();
    UI_CHECK(!ui.stats().repainted);
    // an answer from a worker thread
    w::setViewFirstVisible(ui, t, 6400);
    ui.update();
    UI_CHECK(src.pending.size() >= 1 && src.pending[0].first == 6400u && cell(6400, 0) == "...");
    {
        const FakeSource::Req r = src.pending[0];
        src.pending.erase(src.pending.begin());
        std::thread worker([&am, r] { am.deliver(r.ticket, FakeSource::cells(r)); });
        worker.join();
    }
    UI_CHECK(!am.loaded(6400));
    ui.update();
    UI_CHECK(am.loaded(6400) && cell(6401, 1) == "r6401c1");
    while (!src.pending.empty()) src.answer(am, src.pending[0].first);
    ui.update();
    // a page that arrives for rows outside the window repaints nothing
    w::setViewFirstVisible(ui, t, 50000);
    ui.update();
    UI_CHECK(!src.pending.empty());
    const u32 farPage = src.pending[0].first;
    w::setViewFirstVisible(ui, t, 0); // cached: no request
    ui.update();
    ui.update();
    const u64 fetchesBefore = am.fetchCount();
    UI_CHECK(src.answer(am, farPage));
    ui.update();
    UI_CHECK(am.loaded(farPage) && !ui.stats().repainted);
    UI_CHECK_EQ(am.fetchCount(), fetchesBefore);
    while (!src.pending.empty()) src.answer(am, src.pending[0].first);
    ui.update();
    // the cache is bounded: least recently used pages are dropped and asked for again
    for (u32 r = 1000; r < 1000 + 32 * 30; r += 32) {
        w::setViewFirstVisible(ui, t, r);
        ui.update();
        while (!src.pending.empty()) src.answer(am, src.pending[0].first);
        ui.update();
    }
    UI_CHECK(am.cachedPages() <= 8u);
    UI_CHECK(!am.loaded(0));
    w::setViewFirstVisible(ui, t, 0);
    ui.update();
    UI_CHECK(!src.pending.empty() && src.pending[0].first == 0u);
    // a stale answer (the model was invalidated meanwhile) is refused
    const FakeSource::Req stale = src.pending[0];
    am.setRowCount(500);
    UI_CHECK(!am.deliver(stale.ticket, FakeSource::cells(stale)));
    src.pending.clear();
    ui.update();
    UI_CHECK(w::viewRowCount(ui, t) == 500u && !am.loaded(0) && am.pendingRequests() == u32(src.pending.size()));
    while (!src.pending.empty()) src.answer(am, src.pending[0].first);
    ui.update();
    UI_CHECK(am.loaded(0) && cell(499, 0).empty() == false);
    shot(ui, "async.png");
    // a model destroyed with a commit still queued: the posted command finds nothing
    {
        FakeSource s2;
        auto gone = std::make_unique<w::AsyncModel>(ui, 3, 16, 4);
        gone->setSource(&FakeSource::fetch, &s2);
        gone->setRowCount(64);
        std::string tmp;
        gone->model().data(gone->model().user, 0, 0, tmp);
        UI_CHECK(s2.answer(*gone, 0));
        gone.reset();
        ui.update();
    }
    // steady state: scrolling over cached pages does not allocate
    w::setViewFirstVisible(ui, t, 0);
    for (int i = 0; i < 20; ++i) { ui.wheel(ctr(ui.rect(t)), {0.f, i % 2 ? 1.f : -1.f}); ui.update(1.f / 60.f); }
    const unsigned long long a0 = g_allocs.load();
    for (int i = 0; i < 100; ++i) { ui.wheel(ctr(ui.rect(t)), {0.f, i % 2 ? 1.f : -1.f}); ui.update(1.f / 60.f); }
    UI_CHECK_NO_ALLOC(g_allocs.load() - a0);
    return finish("fuse_ui_mv_async");
}

// ---- directory model (engine-adapter example) ---------------------------------------------------------------------------
/// A file-system tree: a directory is listed the first time a view shows its children (canFetchMore / fetchMore).
struct DirModel {
    struct Node { std::filesystem::path path; std::string name, size; bool dir = false, listed = false; std::vector<w::ItemId> kids; };
    mutable std::vector<Node> nodes; ///< index = id; [0] = the root directory
    mutable int listings = 0;
    w::ItemModel model;
    explicit DirModel(const std::filesystem::path& root) {
        nodes.push_back({root, root.filename().string(), {}, true, false, {}});
        model.user = this;
        model.columns = 2;
        model.childCount = [](const void* u, w::ItemId p) { return u32(static_cast<const DirModel*>(u)->nodes[std::size_t(p)].kids.size()); };
        model.childAt = [](const void* u, w::ItemId p, u32 r) { return static_cast<const DirModel*>(u)->nodes[std::size_t(p)].kids[r]; };
        model.hasChildren = [](const void* u, w::ItemId i) { return static_cast<const DirModel*>(u)->nodes[std::size_t(i)].dir; };
        model.itemData = [](const void* u, w::ItemId i, u32 c, std::string&) -> std::string_view {
            const Node& n = static_cast<const DirModel*>(u)->nodes[std::size_t(i)];
            return c ? std::string_view(n.size) : std::string_view(n.name);
        };
        model.header = [](const void*, u32 c) -> std::string_view { return c ? "Size" : "Name"; };
        model.canFetchMore = [](const void* u, w::ItemId i) {
            const Node& n = static_cast<const DirModel*>(u)->nodes[std::size_t(i)];
            return n.dir && !n.listed;
        };
        model.fetchMore = [](const void* u, w::ItemId i) { static_cast<const DirModel*>(u)->list(i); };
    }
    void list(w::ItemId id) const {
        nodes[std::size_t(id)].listed = true;
        ++listings;
        std::error_code ec;
        std::vector<Node> found;
        for (std::filesystem::directory_iterator it(nodes[std::size_t(id)].path, ec), end; !ec && it != end; it.increment(ec)) {
            Node n;
            n.path = it->path();
            n.name = n.path.filename().string();
            n.dir = it->is_directory(ec);
            if (!n.dir) n.size = std::to_string(it->file_size(ec));
            found.push_back(std::move(n));
        }
        std::sort(found.begin(), found.end(), [](const Node& a, const Node& b) { return a.dir != b.dir ? a.dir : a.name < b.name; }); // folders first
        for (Node& n : found) {
            const w::ItemId k = nodes.size();
            nodes.push_back(std::move(n));
            nodes[std::size_t(id)].kids.push_back(k);
        }
    }
};
int gateDirModel() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("fuse_ui_mv_dir_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "assets" / "textures", ec);
    fs::create_directories(root / "assets" / "empty", ec);
    fs::create_directories(root / "scripts", ec);
    auto put = [&](const fs::path& p, std::size_t bytes) { std::ofstream f(root / p, std::ios::binary); f << std::string(bytes, 'x'); };
    put("readme.txt", 12);
    put("assets/scene.fuse", 300);
    put("assets/textures/rock.png", 2048);
    put("assets/textures/grass.png", 1024);
    put("scripts/main.lua", 77);
    if (!fs::exists(root / "scripts" / "main.lua")) {
        std::printf("fuse_ui_mv_dir_model: no writable temp directory, skipped\n");
        return 77;
    }
    {
        UiContext ui({.viewport = {520, 400}, .font = &font()});
        w::install(ui);
        DirModel dm(root);
        const NodeHandle tree = w::treeModelView(ui, ui.root(), &dm.model, "files");
        ui.setCss(tree, "flex-grow", "1");
        ui.update();
        ui.setFocus(tree);
        std::string sc;
        auto name = [&](u32 row) { return std::string(dm.model.itemData(dm.model.user, w::treeItemAt(ui, tree, row), 0, sc)); };
        auto size = [&](u32 row) { return std::string(dm.model.itemData(dm.model.user, w::treeItemAt(ui, tree, row), 1, sc)); };
        // only the root directory was listed: folders first, then files
        UI_CHECK_EQ(dm.listings, 1);
        UI_CHECK_EQ(w::viewRowCount(ui, tree), 3u);
        UI_CHECK(name(0) == "assets" && name(1) == "scripts" && name(2) == "readme.txt" && size(2) == "12");
        UI_CHECK_EQ(dm.nodes.size(), std::size_t(4)); // nothing below was touched
        key(ui, Key::Home);
        key(ui, Key::Right); // opens "assets": listed now
        UI_CHECK_EQ(dm.listings, 2);
        UI_CHECK_EQ(w::viewRowCount(ui, tree), 6u);
        UI_CHECK(name(1) == "empty" && name(2) == "textures" && name(3) == "scene.fuse" && size(3) == "300");
        UI_CHECK(w::setTreeRowExpanded(ui, tree, 2, true));
        UI_CHECK_EQ(dm.listings, 3);
        UI_CHECK(name(3) == "grass.png" && size(3) == "1024" && name(4) == "rock.png" && w::treeRowDepth(ui, tree, 4) == 2u);
        UI_CHECK(w::setTreeRowExpanded(ui, tree, 1, true)); // an empty folder: listed, no rows
        UI_CHECK(dm.listings == 4 && w::viewRowCount(ui, tree) == 8u);
        // collapsing and reopening does not list again
        UI_CHECK(w::setTreeRowExpanded(ui, tree, 0, false));
        UI_CHECK(w::setTreeRowExpanded(ui, tree, 0, true));
        UI_CHECK(dm.listings == 4 && w::viewRowCount(ui, tree) == 6u);
        ui.textInput("r"); // type-ahead finds the file
        ui.update();
        UI_CHECK_EQ(name(u32(ui.value(tree))), std::string("readme.txt"));
        shot(ui, "dir_model.png");
    }
    fs::remove_all(root, ec);
    return finish("fuse_ui_mv_dir_model");
}

// ---- goldens ----------------------------------------------------------------------------------------------------------------
void sceneTree(Image& img) {
    UiContext ui({.viewport = {560, 360}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "12px");
    w::StringTreeModel m("Name\tKind\tTriangles");
    const w::ItemId scene = m.add(w::kRootItem, "Scene\tgroup\t");
    m.add(scene, "Camera\tcamera\t");
    const w::ItemId lights = m.add(scene, "Lights\tgroup\t");
    m.add(lights, "Sun\tdirectional\t");
    m.add(lights, "Fill\tpoint\t");
    m.add(lights, "Rim\tspot\t");
    const w::ItemId meshes = m.add(scene, "Meshes\tgroup\t");
    const w::ItemId terrain = m.add(meshes, "Terrain\tmesh\t180000");
    m.add(terrain, "lod0\tgeometry\t180000");
    m.add(terrain, "lod1\tgeometry\t45000");
    m.add(meshes, "Rocks\tinstanced mesh\t5200");
    const w::ItemId probes = m.add(scene, "Probes\tgroup\t");
    m.add(probes, "Probe 0\tprobe\t");
    const w::ItemId assets = m.add(w::kRootItem, "Assets\tgroup\t");
    m.add(assets, "Textures\tgroup\t");
    m.add(w::kRootItem, "Settings\tconfig\t");
    const NodeHandle tree = w::treeModelView(ui, ui.root(), &m.model(), "tree");
    ui.setCss(tree, "flex-grow", "1");
    w::setTableColumnWidth(ui, tree, 0, 220.f);
    w::setTableColumnWidth(ui, tree, 1, 150.f);
    w::setViewSortIndicator(ui, tree, 0, true);
    ui.update();
    w::setTreeItemExpanded(ui, tree, scene, true);
    w::setTreeItemExpanded(ui, tree, lights, true);
    w::setTreeItemExpanded(ui, tree, meshes, true); // Terrain and Probes stay collapsed
    w::setViewSelectionMode(ui, tree, w::SelectionMode::Extended);
    ui.setFocus(tree);
    ui.update();
    click(ui, cellPt(ui, tree, 3, 0));             // Sun
    click(ui, cellPt(ui, tree, 5, 0), kModShift);  // .. Rim
    click(ui, cellPt(ui, tree, 8, 1), kModCtrl);   // + Rocks; it is the current row
    settle(ui);
    ui.pointerMove({-10.f, -10.f});
    ui.update();
    UI_CHECK_EQ(w::viewSelectionModel(ui, tree)->selectedRows(), 4ull);
    render(ui, img);
}
void sceneTableEdit(Image& img) {
    UiContext ui({.viewport = {600, 340}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "12px");
    static EditModel m; // (outlives the context: no reports after it)
    static const w::SpinEditorSpec spin{0.f, 500.f, 1.f};
    static const w::CellDelegate spinDelegate = w::spinBoxDelegate(&spin);
    const NodeHandle t = w::tableView(ui, ui.root(), &m.model, "t");
    ui.setCss(t, "flex-grow", "1");
    w::setTableColumnWidth(ui, t, 0, 170.f);
    w::setTableColumnWidth(ui, t, 1, 90.f);
    w::setTableColumnWidth(ui, t, 2, 110.f);
    w::setTableColumnWidth(ui, t, 3, 70.f);
    w::setTableColumnWidth(ui, t, 4, 80.f);
    w::setTableDelegate(ui, t, 1, &spinDelegate);
    w::setViewSortIndicator(ui, t, 1, true);
    w::setViewSelectionMode(ui, t, w::SelectionMode::Extended);
    ui.update();
    ui.setFocus(t);
    click(ui, cellPt(ui, t, 1, 0));
    click(ui, cellPt(ui, t, 3, 0), kModShift);
    click(ui, cellPt(ui, t, 6, 2), kModCtrl);
    click(ui, cellPt(ui, t, 8, 2), kModCtrl);
    settle(ui);
    UI_CHECK(w::viewEdit(ui, t, 6, 0)); // an open line edit over a selected row
    ui.textInput("renaming");
    ui.pointerMove({-10.f, -10.f});
    ui.update();
    UI_CHECK(w::viewEditor(ui, t).valid());
    UI_CHECK_EQ(w::viewSelectionModel(ui, t)->selectedRows(), 5ull);
    render(ui, img);
}
struct Scene { const char* name; void (*fn)(Image&); };
const Scene kScenes[] = {{"tree", &sceneTree}, {"table_edit", &sceneTableEdit}};

void compare(const Scene& s) {
#if FUSE_UI_TEST_TEXT_FULL
    const std::string golden = std::string("golden/modelview_") + s.name + "_full.png"; // FreeType coverage differs from stb_truetype
#else
    const std::string golden = std::string("golden/modelview_") + s.name + ".png";
#endif
    Image img;
    s.fn(img);
    if (!g_out.empty()) stbi_write_png((g_out + "/modelview_" + s.name + ".png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return;
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) {
        std::fprintf(stderr, "missing golden %s (run from the tests directory; FUSE_UI_UPDATE_GOLDEN=1 writes it)\n", golden.c_str());
        return;
    }
    UI_CHECK_EQ(gw, img.width);
    UI_CHECK_EQ(gh, img.height);
    if (gw == img.width && gh == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < gw * gh; ++i)
            for (int c = 0; c < 4; ++c) {
                const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("%s golden diff: %d pixels > 3, max channel diff %d\n", s.name, bad, maxd);
        UI_CHECK(bad <= gw * gh / 1000); // <= 0.1% of pixels may differ (compiler / libm rounding)
    }
    stbi_image_free(ref);
}
} // namespace

int main(int argc, char** argv) {
    std::string gate, scene;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
        else if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
        else if (std::strncmp(argv[i], "--scene=", 8) == 0) scene = argv[i] + 8;
    }
    if (!scene.empty()) {
        for (const Scene& s : kScenes)
            if (scene == s.name) { compare(s); return finish("fuse_ui_mv_golden"); }
        std::fprintf(stderr, "unknown scene %s\n", scene.c_str());
        return 2;
    }
    if (gate == "notify") return gateNotify();
    if (gate == "selection") return gateSelection();
    if (gate == "selection_1m") return gateSelection1M();
    if (gate == "tree") return gateTree();
    if (gate == "tree_1m") return gateTree1M();
    if (gate == "editing") return gateEditing();
    if (gate == "view") return gateView();
    if (gate == "proxy") return gateProxy();
    if (gate == "async") return gateAsync();
    if (gate == "dir_model") return gateDirModel();
    std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
    return 2;
}
