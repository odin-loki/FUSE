// Document classes: what a .fuseui node's "class" may name beyond a plain registered widget class - the compound
// widgets of the catalogue (group box, tab widget, dock area, dialog, table view over inline data, ...) and the
// registered classes that need their factory (state pools, ranges). One table: factory, class-specific properties,
// and where the document's children of such a node go. See document.hpp for the property reference.
#include "doc_classes.hpp"

#include "fuse/text/from_chars.hpp"
#include "fuse/ui/icons.hpp"
#include "fuse/ui/widgets/modelview.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

namespace fuse::ui::doc {
namespace w = fuse::ui::widgets;
namespace {

// ---- JSON helpers -----------------------------------------------------------------------------------------------------
std::string_view str(const JsonValue& n, std::string_view key, std::string_view fallback = {}) {
    const JsonValue* v = n.get(key);
    return v && v->isString() ? std::string_view(v->string) : fallback;
}
f32 num(const JsonValue& n, std::string_view key, f32 fallback) {
    const JsonValue* v = n.get(key);
    return v && v->isNumber() ? f32(v->number) : fallback;
}
bool flag(const JsonValue& n, std::string_view key, bool fallback) {
    const JsonValue* v = n.get(key);
    return v && v->isBool() ? v->boolean : fallback;
}
/// An array of strings joined with '\n' (the item-list form of the widgets); non-strings become empty lines.
std::string joined(const JsonValue& v) {
    std::string out;
    if (!v.isArray()) return out;
    for (std::size_t k = 0; k < v.array.size(); ++k) {
        if (k) out += '\n';
        if (v.array[k].isString()) out += v.array[k].string;
    }
    return out;
}
Color color(const JsonValue* v, Color fallback) {
    Color c = fallback;
    if (v && v->isString()) (void)parseColor(v->string, c);
    return c;
}
/// "YYYY-MM-DD[ HH:MM]" -> fields; false when it does not parse.
bool parseDate(std::string_view s, int f[5]) {
    f[3] = f[4] = 0;
    int got = 0;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end && got < 5) {
        int v = 0, digits = 0;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); ++digits; }
        if (!digits) return false;
        f[got++] = v;
        if (p < end) ++p; // separator
    }
    return got == 3 || got == 5;
}

// ---- inline data models (table / list / grid views built from document data) -------------------------------------------
// The views do not own their model: models built from a document live here, keyed by their view, and are dropped
// once the view or its whole context is gone (checked whenever another one is made). The table outlives contexts, so
// an entry is matched by the context's uid and its `ui` is only used while UiContext::exists(uid).
struct OwnedModel {
    const UiContext* ui = nullptr;
    u64 uid = 0; ///< ui->uid() once adopted
    NodeHandle view;
    std::unique_ptr<w::StringTableModel> model;
    std::unique_ptr<w::StringTreeModel> tree; ///< treemodelview
};
std::vector<OwnedModel>& models() { static std::vector<OwnedModel> m; return m; }
void dropDeadModels() {
    std::erase_if(models(), [](const OwnedModel& m) { return m.view.valid() && (!UiContext::exists(m.uid) || !m.ui->alive(m.view)); });
}
w::StringTableModel* newModel(std::string_view headers) {
    dropDeadModels();
    auto& all = models();
    all.push_back({nullptr, 0, {}, std::make_unique<w::StringTableModel>(headers), nullptr});
    return all.back().model.get();
}
w::StringTreeModel* newTreeModel(std::string_view headers) {
    dropDeadModels();
    auto& all = models();
    all.push_back({nullptr, 0, {}, nullptr, std::make_unique<w::StringTreeModel>(headers)});
    return all.back().tree.get();
}
w::StringTreeModel* treeModelOf(const UiContext& ui, NodeHandle view) {
    for (OwnedModel& m : models())
        if (m.uid == ui.uid() && m.view == view) return m.tree.get();
    return nullptr;
}
/// Fills a tree model from "items": ["Scene\tgroup", "\tCamera\tcamera", ...]: the tabs at the start of a line are
/// its depth (as in a treeview's items), the tabs after that separate the cells of the columns.
void fillTreeModel(w::StringTreeModel& m, const JsonValue& items) {
    while (m.childCount(w::kRootItem)) m.remove(m.childAt(w::kRootItem, 0));
    if (!items.isArray()) return;
    std::vector<w::ItemId> path{w::kRootItem}; // path[d] = the parent of a line of depth d
    for (const JsonValue& it : items.array) {
        const std::string_view line = it.isString() ? std::string_view(it.string) : std::string_view{};
        std::size_t depth = 0;
        while (depth < line.size() && line[depth] == '\t') ++depth;
        depth = std::min(depth, path.size() - 1); // a line may be at most one level below the one before it
        path.resize(depth + 1);
        path.push_back(m.add(path[depth], line.substr(std::min(line.size(), depth))));
    }
}
/// Built-in icons by name ("save", "chevron-left", ...; fuse/ui/icons.hpp).
bool iconByName(const JsonValue& v, Icon& out) { return v.isString() && findIcon(v.string, out); }
/// Pack icons shown by `svgimage` nodes: one store handle per (pack, icon), kept (a reloaded document reuses them).
w::SvgHandle packIconHandle(const IconSet& set, u32 icon) {
    static std::map<std::pair<const IconSet*, u32>, w::SvgHandle> cache;
    const auto key = std::make_pair(&set, icon);
    if (const auto it = cache.find(key); it != cache.end() && w::svgGet(it->second)) return it->second;
    const w::SvgHandle h = w::svgAdopt(SvgImage(set.image(icon)));
    cache[key] = h;
    return h;
}
/// Inline SVG sources of documents: parsed once per distinct source and kept (a reloaded document reuses them).
w::SvgHandle svgBySource(const std::string& xml) {
    static std::map<std::string, w::SvgHandle> cache;
    if (const auto it = cache.find(xml); it != cache.end()) return it->second;
    const auto made = w::svgCreate(xml);
    const w::SvgHandle h = made ? *made : w::SvgHandle{};
    cache.emplace(xml, h);
    return h;
}
void adopt(const UiContext& ui, NodeHandle view, const w::StringTableModel* model) {
    for (OwnedModel& m : models())
        if (m.model.get() == model) { m.ui = &ui; m.uid = ui.uid(); m.view = view; }
}
w::StringTableModel* modelOf(const UiContext& ui, NodeHandle view) {
    for (OwnedModel& m : models())
        if (m.uid == ui.uid() && m.view == view) return m.model.get();
    return nullptr;
}
/// Fills a model from "rows": [[cell, ...], ...] or "items": [text, ...].
void fillModel(w::StringTableModel& m, const JsonValue& n) {
    m.clear();
    if (const JsonValue* rows = n.get("rows"); rows && rows->isArray())
        for (const JsonValue& r : rows->array) {
            std::string line;
            if (r.isArray())
                for (std::size_t k = 0; k < r.array.size(); ++k) {
                    if (k) line += '\t';
                    const JsonValue& c = r.array[k];
                    if (c.isString()) line += c.string;
                    else if (c.isNumber()) { char b[32]; std::snprintf(b, sizeof b, "%g", c.number); line += b; }
                }
            else if (r.isString()) line = r.string;
            m.addRow(line);
        }
    if (const JsonValue* items = n.get("items"); items && items->isArray())
        for (const JsonValue& it : items->array) m.addRow(it.isString() ? std::string_view(it.string) : std::string_view{});
}

// ---- built-in classes ---------------------------------------------------------------------------------------------------
using Make = NodeHandle (*)(UiContext&, NodeHandle, std::string_view, const JsonValue&);
using Prop = bool (*)(UiContext&, NodeHandle, std::string_view, const JsonValue&);
using Child = NodeHandle (*)(UiContext&, NodeHandle, std::size_t, const JsonValue&);

bool itemsProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) { // item list, range untouched
    if (key != "items") return false;
    ui.setText(n, joined(v));
    return true;
}
bool menuButtonProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key != "text" && key != "items") return false;
    // text = "title\nitem\nitem": either half can be set
    const std::string cur(ui.text(n));
    const std::size_t nl = cur.find('\n');
    std::string title = cur.substr(0, nl), items = nl == std::string::npos ? std::string{} : cur.substr(nl + 1);
    if (key == "text") { if (!v.isString() || v.string.find('{') != std::string::npos) return false; title = v.string; }
    else items = joined(v);
    ui.setText(n, title + "\n" + items);
    ui.setA11y(n, A11yRole::Button, title);
    return true;
}
bool tabOptionProp(UiContext& ui, NodeHandle bar, std::string_view key, const JsonValue& v);
bool tabWidgetProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    return key == "tabs" /* (factory) */ || tabOptionProp(ui, w::tabWidgetBar(ui, n), key, v); // "closable", "movable", "scrollButtons"
}
NodeHandle tabWidgetChild(UiContext& ui, NodeHandle n, std::size_t i, const JsonValue&) { return w::tabPage(ui, n, u32(i)); }
NodeHandle stackChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue&) { return w::stackAddPage(ui, n); }
bool splitterProp(UiContext&, NodeHandle, std::string_view key, const JsonValue&) { return key == "stacked" || key == "sizes"; }
NodeHandle splitterChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue& child) {
    return w::splitterPane(ui, n, num(child, "paneSize", 0.f)); // a child's "paneSize" (px) fixes its pane
}
NodeHandle dockAreaChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue& child) {
    return str(child, "class") == "dock" ? n : w::dockRegion(ui, n, w::DockSide::Center);
}
bool dockProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "side") return true; // read by the factory
    if (key == "text" && v.isString()) {
        ui.setText(ui.firstChild(n), v.string);
        ui.setA11y(n, A11yRole::Group, v.string);
        return true;
    }
    if (key == "float" && v.isArray() && v.array.size() == 4) { // [x, y, w, h]
        w::dockFloat(ui, n, {f32(v.array[0].number), f32(v.array[1].number), f32(v.array[2].number), f32(v.array[3].number)});
        return true;
    }
    return false;
}
NodeHandle dockChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue&) { return w::dockBody(ui, n); }
bool groupBoxProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "checkable") return true;
    if (key == "text" && v.isString() && v.string.find('{') == std::string::npos) {
        ui.setText(ui.firstChild(n), v.string);
        ui.setA11y(n, A11yRole::Group, v.string);
        return true;
    }
    return false;
}
NodeHandle groupBoxChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue&) { return w::groupBoxBody(ui, n); }
// A dialog's document node is its popup scope (the node widgets::dialog gives the id to); the card is its child.
bool dialogProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "text" && v.isString() && v.string.find('{') == std::string::npos) { // the title label + the a11y name
        const NodeHandle card = ui.firstChild(n);
        ui.setText(ui.firstChild(card), v.string);
        ui.setA11y(card, A11yRole::Dialog, v.string);
        return true;
    }
    return false;
}
NodeHandle dialogChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue&) { return w::dialogBody(ui, ui.firstChild(n)); }
NodeHandle mainWindowChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue&) { return w::mainWindowCentral(ui, n); }
bool statusBarProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key != "text" || !v.isString() || v.string.find('{') != std::string::npos) return false;
    w::statusMessage(ui, n, v.string);
    return true;
}
bool tableProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "columns" || key == "tileWidth" || key == "tileHeight") return true;
    if (key != "rows" && key != "items") return false;
    w::StringTableModel* m = modelOf(ui, n);
    if (!m) return false;
    JsonValue holder;
    holder.type = JsonValue::Type::Object;
    holder.object.emplace_back(std::string(key), v);
    fillModel(*m, holder);
    if (ui.registry().get(ui.classOf(n)).name == "tableview") w::tableModelChanged(ui, n);
    else ui.invalidate(n);
    return true;
}
bool treeModelProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "columns" || key == "editable") return true; // read by the factory
    w::StringTreeModel* m = treeModelOf(ui, n);
    if (!m) return false;
    if (key == "items") { // (the model reports each change; the view follows incrementally)
        fillTreeModel(*m, v);
        return true;
    }
    if (key == "expanded" && v.isBool()) { // true: every node shown now is expanded, level by level (inline trees are small)
        w::treeCollapseAll(ui, n);
        if (v.boolean)
            for (u32 row = 0; row < w::viewRowCount(ui, n) && row < 4096u; ++row) (void)w::setTreeRowExpanded(ui, n, row, true);
        return true;
    }
    return false;
}
bool svgImageProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "icon") { // an icon of the context's pack (attached before the document loads), else a built-in one
        if (const IconSet* set = ui.icons(); set && v.isString())
            if (const i32 i = set->find(v.string); i >= 0) {
                w::setSvgImage(ui, n, packIconHandle(*set, u32(i)));
                return true;
            }
        Icon icon;
        if (!iconByName(v, icon)) return false;
        w::setSvgImage(ui, n, w::iconHandle(icon));
        return true;
    }
    if (key == "svg" && v.isString()) { // inline SVG source
        w::setSvgImage(ui, n, svgBySource(v.string));
        return true;
    }
    return false;
}
bool propertyGridProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key != "rows" || !v.isArray()) return false;
    for (const JsonValue& r : v.array) {
        if (!r.isObject()) continue;
        static constexpr std::pair<std::string_view, w::PropKind> kKinds[] = {
            {"group", w::PropKind::Group}, {"bool", w::PropKind::Bool}, {"number", w::PropKind::Number},
            {"text", w::PropKind::Text}, {"enum", w::PropKind::Enum}, {"color", w::PropKind::Color}};
        w::PropertyDesc d;
        d.name = str(r, "name");
        for (const auto& [name, kind] : kKinds)
            if (str(r, "kind", "text") == name) d.kind = kind;
        d.lo = num(r, "min", 0.f);
        d.hi = num(r, "max", 100.f);
        d.step = num(r, "step", 1.f);
        const std::string options = r.get("options") ? joined(*r.get("options")) : std::string{};
        d.options = options;
        std::string value;
        if (const JsonValue* val = r.get("value")) {
            if (val->isString()) value = val->string;
            else if (val->isBool()) value = val->boolean ? "true" : "false";
            else if (val->isNumber()) { char b[32]; std::snprintf(b, sizeof b, "%g", val->number); value = b; }
        }
        w::propertyRow(ui, n, d, value);
    }
    return true;
}
bool colorPickerProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key != "color") return false;
    w::setColorPickerColor(ui, n, color(&v, Color::rgb(0xFFFFFF)));
    return true;
}
bool calendarProp(UiContext&, NodeHandle, std::string_view key, const JsonValue&) { return key == "date"; } // (factory)
bool dateTimeProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    int f[5];
    if (key != "date" || !v.isString() || !parseDate(v.string, f)) return false;
    w::setDateTimeEditValue(ui, n, f[0], f[1], f[2], f[3], f[4]);
    return true;
}
bool chartProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "kind") return true;
    if (key == "series" && v.isArray()) { // [{ "name", "color", "values": [...] }]
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            const JsonValue& s = v.array[i];
            std::vector<f32> values;
            if (const JsonValue* vals = s.get("values"); vals && vals->isArray())
                for (const JsonValue& x : vals->array) values.push_back(x.isNumber() ? f32(x.number) : 0.f);
            static constexpr u32 kPalette[] = {0x3D8BFD, 0x4CC26A, 0xFFB454, 0xFF6B6B, 0xB48EFF};
            w::chartSetSeries(ui, n, u32(i), str(s, "name"), values, color(s.get("color"), Color::rgb(kPalette[i % 5])));
        }
        return true;
    }
    if (key == "spans" && v.isArray()) { // flame graph: [{ "name", "start", "duration", "depth", "color" }]
        std::vector<w::FlameSpan> spans;
        for (const JsonValue& s : v.array)
            spans.push_back({str(s, "name"), num(s, "start", 0.f), num(s, "duration", 0.f), u32(num(s, "depth", 0.f)), color(s.get("color"), {})});
        w::chartSetFlame(ui, n, spans);
        return true;
    }
    return false;
}
bool markdownProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key != "text" || !v.isString()) return false;
    w::setMarkdown(ui, n, v.string);
    return true;
}
bool creationOnly(UiContext&, NodeHandle, std::string_view key, const JsonValue&) {
    return key == "families" || key == "size" || key == "language" || key == "profile" || key == "duration" ||
           key == "imageWidth" || key == "imageHeight" || key == "image" || key == "source" || key == "range";
}
bool nodeGraphProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "nodes" && v.isArray()) { // [{ "title", "x", "y", "inputs": [name...], "outputs": [name...] }]
        for (const JsonValue& g : v.array) {
            std::vector<w::GraphPin> in, out;
            if (const JsonValue* a = g.get("inputs"); a && a->isArray())
                for (const JsonValue& p : a->array) in.push_back({p.isString() ? std::string_view(p.string) : std::string_view{}, 0});
            if (const JsonValue* a = g.get("outputs"); a && a->isArray())
                for (const JsonValue& p : a->array) out.push_back({p.isString() ? std::string_view(p.string) : std::string_view{}, 0});
            w::nodeGraphAddNode(ui, n, str(g, "title"), {num(g, "x", 0.f), num(g, "y", 0.f)}, in, out);
        }
        return true;
    }
    if (key == "links" && v.isArray()) { // [[fromNode, outPin, toNode, inPin], ...]
        for (const JsonValue& l : v.array)
            if (l.isArray() && l.array.size() == 4)
                w::nodeGraphConnect(ui, n, u32(l.array[0].number), u32(l.array[1].number), u32(l.array[2].number), u32(l.array[3].number));
        return true;
    }
    return false;
}
bool curveProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "range") return true;
    if (key != "keys" || !v.isArray()) return false; // [[t, v], ...]
    for (const JsonValue& k : v.array)
        if (k.isArray() && k.array.size() >= 2) w::curveAddKey(ui, n, f32(k.array[0].number), f32(k.array[1].number));
    return true;
}
bool timelineProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "duration") return true;
    if (key != "tracks" || !v.isArray()) return false; // [{ "name", "clips": [{ "start", "end", "text", "color" }] }]
    for (const JsonValue& t : v.array) {
        const u32 track = w::timelineAddTrack(ui, n, str(t, "name"));
        if (const JsonValue* clips = t.get("clips"); clips && clips->isArray())
            for (const JsonValue& c : clips->array)
                w::timelineAddClip(ui, n, track, num(c, "start", 0.f), num(c, "end", 1.f), str(c, "text"), color(c.get("color"), Color::rgb(0x3D8BFD)));
    }
    return true;
}

// ---- Qt-parity options and the extras widgets (widgets/edit_options.hpp, widgets/extra.hpp) -------------------------------
bool lineEditProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    w::LineEditOptions o = w::lineEditOptions(ui, n);
    if (key == "placeholder" && v.isString()) o.placeholder = v.string;
    else if (key == "echo" && v.isString())
        o.echo = v.string == "password" ? w::EchoMode::Password : v.string == "noecho" ? w::EchoMode::NoEcho
               : v.string == "passwordEchoOnEdit" ? w::EchoMode::PasswordEchoOnEdit : w::EchoMode::Normal;
    else if (key == "maxLength" && v.isNumber()) o.maxLength = u32(std::max(0.0, v.number));
    else if (key == "readOnly" && v.isBool()) o.readOnly = v.boolean;
    else if (key == "inputMask" && v.isString()) o.inputMask = v.string;
    else return false;
    w::setLineEditOptions(ui, n, std::move(o));
    return true;
}
bool spinBoxProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    w::SpinBoxOptions o = w::spinBoxOptions(ui, n);
    if (key == "prefix" && v.isString()) o.prefix = v.string;
    else if (key == "suffix" && v.isString()) o.suffix = v.string;
    else if (key == "specialValueText" && v.isString()) o.specialValueText = v.string;
    else if (key == "wrapping" && v.isBool()) o.wrapping = v.boolean;
    else if (key == "decimals" && v.isNumber()) o.decimals = i32(v.number);
    else return false;
    w::setSpinBoxOptions(ui, n, std::move(o));
    return true;
}
bool tabOptionProp(UiContext& ui, NodeHandle bar, std::string_view key, const JsonValue& v) {
    if (!v.isBool()) return false;
    if (key == "closable") w::setTabsClosable(ui, bar, v.boolean);
    else if (key == "movable") w::setTabsMovable(ui, bar, v.boolean);
    else if (key == "scrollButtons") w::setTabUsesScrollButtons(ui, bar, v.boolean);
    else return false;
    return true;
}
bool lcdProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "digits") return true; // read by the factory
    if (key == "text" && v.isString()) { w::setLcdText(ui, n, v.string); return true; }
    if (key == "base" && v.isNumber()) { w::setLcdBase(ui, n, u32(std::max(0.0, v.number))); return true; }
    return false;
}
bool keySequenceProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key != "text" || !v.isString()) return false;
    Shortcut s;
    (void)parseShortcut(v.string, s);
    w::setKeySequence(ui, n, s);
    return true;
}
bool toolBoxProp(UiContext&, NodeHandle, std::string_view key, const JsonValue&) { return key == "pages"; } // (factory)
NodeHandle toolBoxChild(UiContext& ui, NodeHandle n, std::size_t i, const JsonValue&) { return w::toolBoxPage(ui, n, u32(i)); }
Rect rectOf(const JsonValue* v, Rect fallback) {
    if (!v || !v->isArray() || v->array.size() != 4) return fallback;
    return {f32(v->array[0].number), f32(v->array[1].number), f32(v->array[2].number), f32(v->array[3].number)};
}
bool mdiWindowProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "text") return true; // the title: read by the factory
    if (key == "rect" && v.isArray() && v.array.size() == 4) { w::setMdiGeometry(ui, n, rectOf(&v, {})); return true; }
    return false;
}
NodeHandle mdiWindowChild(UiContext& ui, NodeHandle n, std::size_t, const JsonValue&) { return w::mdiBody(ui, n); }
u32 standardButtonsOf(const JsonValue* v) {
    static constexpr std::pair<std::string_view, u32> kNames[] = {
        {"ok", w::StdButton::Ok}, {"save", w::StdButton::Save}, {"saveAll", w::StdButton::SaveAll}, {"open", w::StdButton::Open},
        {"yes", w::StdButton::Yes}, {"yesToAll", w::StdButton::YesToAll}, {"no", w::StdButton::No}, {"noToAll", w::StdButton::NoToAll},
        {"abort", w::StdButton::Abort}, {"retry", w::StdButton::Retry}, {"ignore", w::StdButton::Ignore}, {"close", w::StdButton::Close},
        {"cancel", w::StdButton::Cancel}, {"discard", w::StdButton::Discard}, {"help", w::StdButton::Help}, {"apply", w::StdButton::Apply},
        {"reset", w::StdButton::Reset}, {"restoreDefaults", w::StdButton::RestoreDefaults}};
    u32 mask = 0;
    if (v && v->isArray())
        for (const JsonValue& b : v->array)
            for (const auto& [name, bit] : kNames)
                if (b.isString() && b.string == name) mask |= bit;
    return mask;
}
w::ButtonBoxLayout buttonLayoutOf(std::string_view s) {
    return s == "windows" ? w::ButtonBoxLayout::Windows : s == "mac" ? w::ButtonBoxLayout::Mac : s == "kde" ? w::ButtonBoxLayout::Kde
         : s == "gnome" ? w::ButtonBoxLayout::Gnome : w::ButtonBoxLayout::Platform;
}
bool buttonBoxProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "buttons" && v.isArray()) { w::setDialogButtonBoxStandardButtons(ui, n, standardButtonsOf(&v)); return true; }
    if (key == "layout" && v.isString()) { w::setDialogButtonBoxOptions(ui, n, {.layout = buttonLayoutOf(v.string)}); return true; }
    return false;
}
bool editComboProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "insertPolicy") return true; // read by the factory
    if (key == "items" && v.isArray()) {
        const i32 cur = w::comboCurrentIndex(ui, n);
        while (w::comboCount(ui, n)) w::comboRemoveItem(ui, n, w::comboCount(ui, n) - 1);
        for (const JsonValue& it : v.array) w::comboAddItem(ui, n, it.isString() ? std::string_view(it.string) : std::string_view{});
        if (cur >= 0 && u32(cur) < w::comboCount(ui, n)) w::setComboCurrentIndex(ui, n, cur);
        return true;
    }
    if (key == "placeholder" && v.isString()) return lineEditProp(ui, w::comboLineEdit(ui, n), key, v);
    return false;
}
// Graphics scenes built from document data: owned here, keyed by their view, dropped with it (as the inline models).
struct OwnedScene {
    u64 uid = 0;
    const UiContext* ui = nullptr;
    NodeHandle view;
    std::unique_ptr<w::GraphicsScene> scene;
};
std::vector<OwnedScene>& scenes() { static std::vector<OwnedScene> s; return s; }
w::GraphicsScene* sceneFor(UiContext& ui, NodeHandle view) {
    auto& all = scenes();
    std::erase_if(all, [](const OwnedScene& s) { return !UiContext::exists(s.uid) || !s.ui->alive(s.view); });
    for (OwnedScene& s : all)
        if (s.uid == ui.uid() && s.view == view) return s.scene.get();
    all.push_back({ui.uid(), &ui, view, std::make_unique<w::GraphicsScene>()});
    return all.back().scene.get();
}
bool graphicsViewProp(UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
    if (key == "zoom" && v.isNumber()) { w::setViewTransform(ui, n, f32(v.number), w::viewPan(ui, n)); return true; }
    if (key != "items" || !v.isArray()) return false;
    // [{ "shape": rect|ellipse|line|text|polygon, "rect": [x, y, w, h], "points": [[x, y]...], "fill", "stroke",
    //    "strokeWidth", "z", "text", "movable", "selectable" }]
    auto scene = std::make_unique<w::GraphicsScene>();
    for (const JsonValue& it : v.array) {
        w::GraphicsItem g;
        const std::string_view shape = str(it, "shape", "rect");
        g.shape = shape == "ellipse" ? w::GraphicsShape::Ellipse : shape == "line" ? w::GraphicsShape::Line
                : shape == "text" ? w::GraphicsShape::Text : shape == "polygon" ? w::GraphicsShape::Polygon : w::GraphicsShape::Rect;
        g.rect = rectOf(it.get("rect"), {});
        if (const JsonValue* pts = it.get("points"); pts && pts->isArray())
            for (const JsonValue& p : pts->array)
                if (p.isArray() && p.array.size() == 2) g.points.push_back({f32(p.array[0].number), f32(p.array[1].number)});
        g.fill = color(it.get("fill"), g.fill);
        g.stroke = color(it.get("stroke"), g.stroke);
        g.strokeWidth = num(it, "strokeWidth", g.strokeWidth);
        g.z = num(it, "z", 0.f);
        g.text = std::string(str(it, "text"));
        g.movable = flag(it, "movable", true);
        g.selectable = flag(it, "selectable", true);
        scene->add(std::move(g));
    }
    w::GraphicsScene* owned = sceneFor(ui, n); // the view keeps pointing at the same scene object: swap its content
    *owned = std::move(*scene);
    w::setGraphicsScene(ui, n, owned);
    return true;
}

struct Builtin {
    std::string_view name;
    Make make;
    Prop prop = nullptr;
    Child child = nullptr;
};
const Builtin kBuiltins[] = {
    // registered classes that need their factory
    {"slider", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::slider(ui, p, 0.f, 1.f, 0.f, 0.f, id); }},
    {"progressbar", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::progressBar(ui, p, 0.f, id); }},
    {"dial", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::dial(ui, p, 0.f, 1.f, 0.f, id); }},
    {"spinbox", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::spinBox(ui, p, 0.f, 100.f, 0.f, 1.f, id); },
     &spinBoxProp}, // "prefix", "suffix", "specialValueText", "wrapping", "decimals"
    {"scrollbar", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::scrollBar(ui, p, !flag(n, "horizontal", false), id); },
     [](UiContext&, NodeHandle, std::string_view key, const JsonValue&) { return key == "horizontal"; }},
    {"lineedit", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::lineEdit(ui, p, {}, id); },
     &lineEditProp}, // "placeholder", "echo": normal|password|noecho|passwordEchoOnEdit, "maxLength", "readOnly", "inputMask"
    {"textedit", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::textEdit(ui, p, {}, id); }},
    {"richtext", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::richText(ui, p, {}, id); }},
    {"header", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::headerView(ui, p, {}, 120.f, id); },
     [](UiContext& ui, NodeHandle n, std::string_view key, const JsonValue& v) {
         if (key != "items") return false;
         w::setHeaderTitles(ui, n, joined(v));
         return true;
     }},
    {"listview", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::listView(ui, p, {}, -1, id); }, &itemsProp},
    {"treeview", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::treeView(ui, p, {}, id); }, &itemsProp},
    {"menubutton", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::menuBarMenu(ui, p, {}, {}, id); }, &menuButtonProp},
    {"codeedit",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         const std::string_view l = str(n, "language", "lua");
         return w::codeEdit(ui, p, {}, l == "cpp" ? w::CodeLanguage::Cpp : l == "css" ? w::CodeLanguage::Css : l == "plain" ? w::CodeLanguage::Plain : w::CodeLanguage::Lua, id);
     }, &creationOnly},
    {"colorpicker", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::colorPicker(ui, p, color(n.get("color"), Color::rgb(0xFFFFFF)), id); }, &colorPickerProp},
    {"calendar", // "date": "YYYY-MM-DD" (the month shown is the date's)
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         int f[5] = {2026, 1, 1, 0, 0};
         if (!parseDate(str(n, "date"), f)) { f[0] = 2026; f[1] = 1; f[2] = 1; }
         return w::calendar(ui, p, f[0], f[1], f[2], id);
     }, &calendarProp},
    {"datetimeedit", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::dateTimeEdit(ui, p, 2026, 1, 1, 0, 0, id); }, &dateTimeProp},
    {"chart",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         const std::string_view k = str(n, "kind", "line");
         return w::chart(ui, p, k == "bar" ? w::ChartKind::Bar : k == "area" ? w::ChartKind::Area : k == "scatter" ? w::ChartKind::Scatter : k == "flame" ? w::ChartKind::Flame : w::ChartKind::Line, id);
     }, &chartProp},
    {"nodegraph", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::nodeGraph(ui, p, id); }, &nodeGraphProp},
    {"curveeditor",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         f32 r[4] = {0.f, 1.f, 0.f, 1.f}; // "range": [tMin, tMax, vMin, vMax]
         if (const JsonValue* a = n.get("range"); a && a->isArray() && a->array.size() == 4)
             for (int i = 0; i < 4; ++i) r[i] = f32(a->array[std::size_t(i)].number);
         return w::curveEditor(ui, p, r[0], r[1], r[2], r[3], id);
     }, &curveProp},
    {"timeline", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::timeline(ui, p, num(n, "duration", 10.f), id); }, &timelineProp},
    {"viewport",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         return w::viewportWidget(ui, p, u32(num(n, "image", 0.f)), u32(num(n, "imageWidth", 16.f)), u32(num(n, "imageHeight", 9.f)), id);
     }, &creationOnly},
    {"video", // "source": "testpattern" (the built-in pattern) or absent (no source: an app attaches one in code)
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         return w::videoWidget(ui, p, str(n, "source") == "testpattern" ? &w::testPatternSource() : nullptr, id);
     }, &creationOnly},
    {"kineticscroll",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         const std::string_view pr = str(n, "profile", "ios");
         return w::kineticScrollArea(ui, p, pr == "android" ? w::ScrollProfile::android() : pr == "desktop" ? w::ScrollProfile::desktop() : w::ScrollProfile::ios(), id);
     }, &creationOnly},
    {"tableview", // "columns": [title...], "rows": [[cell...]...]
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         std::string headers;
         if (const JsonValue* c = n.get("columns"); c && c->isArray())
             for (std::size_t k = 0; k < c->array.size(); ++k) headers += (k ? "\t" : "") + (c->array[k].isString() ? c->array[k].string : std::string{});
         w::StringTableModel* m = newModel(headers.empty() ? "Value" : headers);
         const NodeHandle t = w::tableView(ui, p, &m->model(), id);
         adopt(ui, t, m);
         return t;
     }, &tableProp},
    {"gridview", // "items": [text...], "tileWidth", "tileHeight"
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         w::StringTableModel* m = newModel("name");
         const NodeHandle g = w::gridView(ui, p, &m->model(), num(n, "tileWidth", 96.f), num(n, "tileHeight", 72.f), id);
         adopt(ui, g, m);
         return g;
     }, &tableProp},
    {"treemodelview", // "columns": [title...], "items": [line...] (leading tabs = depth, then tab-separated cells), "editable", "expanded"
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         std::string headers;
         if (const JsonValue* c = n.get("columns"); c && c->isArray())
             for (std::size_t k = 0; k < c->array.size(); ++k) headers += (k ? "\t" : "") + (c->array[k].isString() ? c->array[k].string : std::string{});
         w::StringTreeModel* m = newTreeModel(headers.empty() ? "Name" : headers);
         m->setEditable(flag(n, "editable", false));
         const NodeHandle t = w::treeModelView(ui, p, &m->model(), id);
         for (OwnedModel& o : models())
             if (o.tree.get() == m) { o.ui = &ui; o.uid = ui.uid(); o.view = t; }
         return t;
     }, &treeModelProp},
    {"svgimage", // "icon": a built-in icon name, or "svg": inline source; sized by the style (width / height)
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::svgImage(ui, p, {}, 0.f, 0.f, id); }, &svgImageProp},
    // names that are not registered classes: variants and compound widgets
    {"doublespinbox", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::doubleSpinBox(ui, p, 0.f, 1.f, 0.f, 0.1f, id); },
     &spinBoxProp},
    {"plaintextedit", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::plainTextEdit(ui, p, {}, id); }},
    {"modellistview", // "items": [text...]
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) {
         w::StringTableModel* m = newModel("name");
         const NodeHandle l = w::modelListView(ui, p, &m->model(), id);
         adopt(ui, l, m);
         return l;
     }, &tableProp},
    {"scrollarea", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::scrollArea(ui, p, id); }},
    {"tabwidget",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         const NodeHandle t = w::tabWidget(ui, p, n.get("tabs") ? joined(*n.get("tabs")) : std::string{}, id);
         for (u32 i = 0; w::tabPage(ui, t, i).valid(); ++i) ui.setCss(w::tabPage(ui, t, i), "flex-grow", "1"); // pages fill the widget
         return t;
     },
     &tabWidgetProp, &tabWidgetChild},
    {"stack", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::stack(ui, p, id); }, nullptr, &stackChild},
    {"splitter", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::splitter(ui, p, flag(n, "stacked", false), id); },
     &splitterProp, &splitterChild},
    {"dockarea", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::dockArea(ui, p, id); }, nullptr, &dockAreaChild},
    {"dock", // a child of a dockarea: "text" = title, "side": left | right | bottom | center
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         if (!ui.hasClass(p, "dockarea")) return NodeHandle{};
         const std::string_view s = str(n, "side", "left");
         return w::dockWidget(ui, p, str(n, "text"), s == "right" ? w::DockSide::Right : s == "bottom" ? w::DockSide::Bottom : s == "center" ? w::DockSide::Center : w::DockSide::Left, id);
     }, &dockProp, &dockChild},
    {"groupbox", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::groupBox(ui, p, {}, flag(n, "checkable", false), true, id); },
     &groupBoxProp, &groupBoxChild},
    {"toolbar", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::toolBar(ui, p, id); }},
    {"statusbar", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::statusBar(ui, p, id); }, &statusBarProp},
    {"mainwindow", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::mainWindow(ui, p, id); }, nullptr, &mainWindowChild},
    {"dialog", // lives in the overlay layer; "visible" opens / closes it; "text" = title
     [](UiContext& ui, NodeHandle, std::string_view id, const JsonValue& n) { return ui.parent(w::dialog(ui, str(n, "text"), id)); },
     &dialogProp, &dialogChild},
    {"propertygrid", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::propertyGrid(ui, p, id); }, &propertyGridProp},
    {"markdown", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::markdownView(ui, p, str(n, "text"), id); }, &markdownProp},
    {"fontpicker",
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         return w::fontPicker(ui, p, n.get("families") ? joined(*n.get("families")) : std::string{}, num(n, "size", 14.f), id);
     }, &creationOnly},
    // Qt-parity widgets of widgets/edit_options.hpp and widgets/extra.hpp
    {"editcombo", // "items": [text...], "value" = current index, "insertPolicy": none|top|bottom|alphabetical, "placeholder"
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         const std::string_view pol = str(n, "insertPolicy", "bottom");
         return w::editableComboBox(ui, p, n.get("items") ? joined(*n.get("items")) : std::string{}, 0,
                                    pol == "none" ? w::InsertPolicy::NoInsert : pol == "top" ? w::InsertPolicy::InsertAtTop
                                    : pol == "alphabetical" ? w::InsertPolicy::InsertAlphabetically : w::InsertPolicy::InsertAtBottom, id);
     }, &editComboProp},
    {"lcdnumber", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) { return w::lcdNumber(ui, p, u32(num(n, "digits", 6.f)), id); },
     &lcdProp},
    {"keysequenceedit", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::keySequenceEdit(ui, p, {}, id); },
     &keySequenceProp},
    {"toolbox", // "pages": [title...]; child k is the content of page k; "value" = the open page
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         return w::toolBox(ui, p, n.get("pages") ? joined(*n.get("pages")) : std::string{}, id);
     }, &toolBoxProp, &toolBoxChild},
    {"mdiarea", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::mdiArea(ui, p, id); }},
    {"mdiwindow", // a child of an mdiarea: "text" = title, "rect": [x, y, w, h]; children go to the body
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         return w::mdiSubWindow(ui, p, str(n, "text"), rectOf(n.get("rect"), {12.f, 12.f, 240.f, 160.f}), id);
     }, &mdiWindowProp, &mdiWindowChild},
    {"graphicsview", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) {
         const NodeHandle v = w::graphicsView(ui, p, nullptr, id);
         if (ui.alive(v)) w::setGraphicsScene(ui, v, sceneFor(ui, v)); // the document owns the scene
         return v;
     }, &graphicsViewProp},
    {"vkeyboard", [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue&) { return w::virtualKeyboard(ui, p, id); }},
    {"dialogbuttonbox", // "buttons": ["ok", "cancel", "apply", ...], "layout": platform|windows|mac|kde|gnome
     [](UiContext& ui, NodeHandle p, std::string_view id, const JsonValue& n) {
         return w::dialogButtonBox(ui, p, standardButtonsOf(n.get("buttons")), {.layout = buttonLayoutOf(str(n, "layout", "platform"))}, id);
     }, &buttonBoxProp},
};
const Builtin* builtin(std::string_view name) {
    for (const Builtin& b : kBuiltins)
        if (b.name == name) return &b;
    return nullptr;
}
std::vector<DocumentClass>& custom() { static std::vector<DocumentClass> c; return c; }
const DocumentClass* customClass(std::string_view name) {
    for (const DocumentClass& c : custom())
        if (c.name == name) return &c;
    return nullptr;
}
} // namespace

void registerDocumentClass(DocumentClass c) {
    for (DocumentClass& e : custom())
        if (e.name == c.name) { e = std::move(c); return; }
    custom().push_back(std::move(c));
}
bool isDocumentClass(std::string_view name) { return builtin(name) != nullptr || customClass(name) != nullptr; }
std::vector<std::string> documentClassNames() {
    std::vector<std::string> out;
    for (const Builtin& b : kBuiltins) out.emplace_back(b.name);
    for (const DocumentClass& c : custom())
        if (!builtin(c.name)) out.push_back(c.name);
    std::sort(out.begin(), out.end());
    return out;
}

namespace detail {
bool creatable(const UiContext& ui, std::string_view cls) {
    if (ui.registry().find(cls) != 0xFFFFFFFFu || customClass(cls)) return true;
    // a built-in entry that is the factory of a widget class needs that class registered (a context trimmed to a
    // cooked registration list must not grow it back); the compound names build from what is there
    const Builtin* b = builtin(cls);
    return b && b >= builtin("doublespinbox");
}
bool compound(const UiContext& ui, std::string_view cls) {
    return isDocumentClass(cls) && ui.registry().find(cls) == 0xFFFFFFFFu;
}
NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id, const JsonValue& n) {
    if (const DocumentClass* c = customClass(cls); c && c->make) return c->make(ui, parent, id, n);
    if (const Builtin* b = builtin(cls)) return b->make(ui, parent, id, n);
    auto made = ui.create(cls, parent, id);
    return made ? *made : NodeHandle{};
}
bool prop(UiContext& ui, NodeHandle node, std::string_view cls, std::string_view key, const JsonValue& v) {
    if (const DocumentClass* c = customClass(cls); c && c->prop && c->prop(ui, node, key, v)) return true;
    if (const Builtin* b = builtin(cls); b && b->prop && b->prop(ui, node, key, v)) return true;
    // any node
    if (key == "tooltip" && v.isString()) { w::setTooltip(ui, node, v.string); return true; }
    if (key == "contextMenu" && v.isArray()) { w::setContextMenu(ui, node, joined(v)); return true; }
    if (key == "paneSize") return true; // read by the splitter the node is a child of
    if (cls == "tabbar" && tabOptionProp(ui, node, key, v)) return true; // "closable", "movable", "scrollButtons"
    if (key == "icon") { // an icon on a button / tool button (16 px, left of the text)
        const std::string_view name = ui.registry().get(ui.classOf(node)).name;
        PropValue pv;
        if ((name != "button" && name != "toolbutton") || !v.isString() || !parsePropValue(fuse::ui::Prop::Icon, v.string, pv)) return false;
        // with a pack attached (UiContext::setIcons) the name becomes the `icon` style property: the pack's icon, or
        // one a pack attached later brings; a name only the built-in set has is set as before (widgets::iconOf)
        const bool inPack = ui.icons() && ui.icons()->findHash(pv.ident()) >= 0;
        Icon builtin;
        if (!inPack && iconByName(v, builtin)) {
            w::setIcon(ui, node, builtin);
            return true;
        }
        if (!ui.icons()) return false; // no pack: only the built-in names exist
        ui.set(node, fuse::ui::Prop::Icon, pv);
        return true;
    }
    return false;
}
NodeHandle childParent(UiContext& ui, NodeHandle node, std::string_view cls, std::size_t index, const JsonValue& child) {
    if (const DocumentClass* c = customClass(cls); c && c->child) return c->child(ui, node, index, child);
    if (const Builtin* b = builtin(cls); b && b->child) return b->child(ui, node, index, child);
    return node;
}
bool setVisible(UiContext& ui, NodeHandle node, bool on) {
    const NodeHandle card = ui.firstChild(node);
    if (!ui.hasClass(node, "modal") || !ui.hasClass(card, "dialog")) return false; // a dialog scope: open / close it
    // "doc-open" remembers that the document opened it (computed visibility lags one update behind)
    if (on && !(ui.hasClass(node, "doc-open") && w::popupOpen(ui, node))) {
        ui.addClass(node, "doc-open");
        w::openDialog(ui, card);
    } else if (!on && ui.hasClass(node, "doc-open")) {
        ui.removeClass(node, "doc-open");
        w::closeDialog(ui, card);
    }
    return true;
}
bool setValue(UiContext& ui, NodeHandle node, f32 v) {
    if (ui.hasClass(node, "stack")) { // a page index shows that page (pages added later show it as they come)
        if (ui.childCount(node)) w::setStackIndex(ui, node, u32(std::max(0.f, v)));
        else ui.setValue(node, std::max(0.f, v));
        return true;
    }
    if (ui.hasClass(node, "tabwidget")) {
        ui.setValue(w::tabWidgetBar(ui, node), v, true);
        return true;
    }
    if (ui.hasClass(node, "toolbox")) { // the open page
        w::setToolBoxIndex(ui, node, u32(std::max(0.f, v)));
        return true;
    }
    if (ui.registry().get(ui.classOf(node)).name == "editcombo") { // the current item (its text goes into the field)
        w::setComboCurrentIndex(ui, node, i32(v));
        return true;
    }
    return false;
}
} // namespace detail

} // namespace fuse::ui::doc
