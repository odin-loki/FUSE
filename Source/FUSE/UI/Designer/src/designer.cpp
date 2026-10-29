#include "fuse/ui/designer/designer.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fuse::ui::designer {

using doc::JsonValue;
namespace w = fuse::ui::widgets;

namespace {
const JsonValue* member(const JsonValue& o, std::string_view k) {
    for (const auto& [key, v] : o.object)
        if (key == k) return &v;
    return nullptr;
}
JsonValue* memberMut(JsonValue& o, std::string_view k) {
    for (auto& [key, v] : o.object)
        if (key == k) return &v;
    return nullptr;
}
void setMember(JsonValue& o, std::string_view k, const JsonValue& v) {
    o.type = JsonValue::Type::Object;
    if (JsonValue* m = memberMut(o, k)) { *m = v; return; }
    o.object.emplace_back(std::string(k), v);
}
void eraseMember(JsonValue& o, std::string_view k) {
    o.object.erase(std::remove_if(o.object.begin(), o.object.end(), [&](const auto& p) { return p.first == k; }), o.object.end());
}
JsonValue str(std::string_view s) { JsonValue v; v.type = JsonValue::Type::String; v.string.assign(s); return v; }
JsonValue num(double d) { JsonValue v; v.type = JsonValue::Type::Number; v.number = d; return v; }

void quote(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                out += b;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}
std::string numText(double d) {
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, d);
    return std::string(b, r.ptr);
}
bool scalar(const JsonValue& v) { return !v.isArray() && !v.isObject(); }
void write(std::string& out, const JsonValue& v, int ind) {
    const std::string pad(std::size_t(ind) * 2, ' '), pad2(std::size_t(ind + 1) * 2, ' ');
    switch (v.type) {
    case JsonValue::Type::Null: out += "null"; return;
    case JsonValue::Type::Bool: out += v.boolean ? "true" : "false"; return;
    case JsonValue::Type::Number: out += numText(v.number); return;
    case JsonValue::Type::String: quote(out, v.string); return;
    case JsonValue::Type::Array: {
        const bool flat = std::all_of(v.array.begin(), v.array.end(), scalar);
        if (v.array.empty()) { out += "[]"; return; }
        out += flat ? "[" : "[\n";
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            if (!flat) out += pad2;
            write(out, v.array[i], ind + 1);
            if (i + 1 < v.array.size()) out += flat ? ", " : ",\n";
        }
        if (!flat) { out += '\n'; out += pad; }
        out += ']';
        return;
    }
    case JsonValue::Type::Object: {
        if (v.object.empty()) { out += "{}"; return; }
        const bool flat = v.object.size() <= 4 && std::all_of(v.object.begin(), v.object.end(), [](const auto& p) { return scalar(p.second); });
        out += flat ? "{ " : "{\n";
        for (std::size_t i = 0; i < v.object.size(); ++i) {
            if (!flat) out += pad2;
            quote(out, v.object[i].first);
            out += ": ";
            write(out, v.object[i].second, ind + 1);
            if (i + 1 < v.object.size()) out += flat ? ", " : ",\n";
        }
        out += flat ? " }" : "\n" + pad + "}";
        return;
    }
    }
}

bool fromJson(const JsonValue& j, DesignNode& out, std::string& err) {
    if (!j.isObject()) { err = "node is not an object"; return false; }
    out.props = JsonValue{};
    out.props.type = JsonValue::Type::Object;
    out.children.clear();
    for (const auto& [k, v] : j.object) {
        if (k == "children") {
            if (!v.isArray()) { err = "children must be an array"; return false; }
            for (const JsonValue& c : v.array) {
                out.children.emplace_back();
                if (!fromJson(c, out.children.back(), err)) return false;
            }
        } else {
            out.props.object.emplace_back(k, v);
        }
    }
    if (!member(out.props, "class")) { err = "node without a class"; return false; }
    return true;
}
JsonValue toJson(const DesignNode& n) {
    JsonValue j = n.props;
    if (!n.children.empty()) {
        JsonValue arr;
        arr.type = JsonValue::Type::Array;
        for (const DesignNode& c : n.children) arr.array.push_back(toJson(c));
        j.object.emplace_back("children", std::move(arr));
    }
    return j;
}
// preorder walk helpers
template <class N, class F>
bool walk(N& n, u32& idx, u32 parent, u32 depth, F&& f) {
    const u32 me = idx++;
    if (f(n, me, parent, depth)) return true;
    for (auto& c : n.children)
        if (walk(c, idx, me, depth + 1, f)) return true;
    return false;
}
} // namespace

std::string writeJson(const JsonValue& v, int indent) {
    std::string out;
    write(out, v, indent);
    return out;
}

std::string_view DesignNode::cls() const {
    const JsonValue* c = member(props, "class");
    return c && c->isString() ? std::string_view(c->string) : std::string_view{};
}
std::string_view DesignNode::id() const {
    const JsonValue* c = member(props, "id");
    return c && c->isString() ? std::string_view(c->string) : std::string_view{};
}

// ---- DesignDocument ---------------------------------------------------------------------------------------------------
bool DesignDocument::load(std::string_view json, std::string* error) {
    auto parsed = doc::parseJson(json);
    std::string err;
    if (!parsed) {
        err = "line " + std::to_string(parsed.error().line) + ":" + std::to_string(parsed.error().column) + ": " + parsed.error().message;
    } else if (!parsed->isObject() || !parsed->get("root")) {
        err = "not a .fuseui document (no root)";
    } else {
        DesignNode root;
        if (fromJson(*parsed->get("root"), root, err)) {
            m_header = JsonValue{};
            m_header.type = JsonValue::Type::Object;
            for (const auto& [k, v] : parsed->object)
                if (k != "root") m_header.object.emplace_back(k, v);
            m_root = std::move(root);
            m_undo.clear();
            m_redo.clear();
            ++m_rev;
            return true;
        }
    }
    if (error) *error = err;
    return false;
}

std::string DesignDocument::save() const {
    JsonValue top = m_header;
    top.type = JsonValue::Type::Object;
    if (!member(top, "fuseui")) top.object.insert(top.object.begin(), {"fuseui", num(1)});
    top.object.emplace_back("root", toJson(m_root));
    return writeJson(top) + "\n";
}

namespace {
void stripEditorKeys(JsonValue& v) {
    std::erase_if(v.object, [](const auto& kv) { return kv.first.starts_with("x-"); });
    if (JsonValue* c = const_cast<JsonValue*>(v.get("children")))
        for (JsonValue& e : c->array) stripEditorKeys(e);
}
} // namespace

std::string DesignDocument::previewText() const {
    JsonValue top = m_header;
    top.type = JsonValue::Type::Object;
    if (!member(top, "fuseui")) top.object.insert(top.object.begin(), {"fuseui", num(1)});
    JsonValue r = toJson(m_root);
    stripEditorKeys(r);
    top.object.emplace_back("root", std::move(r));
    return writeJson(top);
}

void DesignDocument::reset() {
    load("{\"fuseui\": 1, \"root\": {\"class\": \"panel\", \"id\": \"root\", \"style\": {\"padding\": \"12px\", \"gap\": \"6px\"}}}");
}

u32 DesignDocument::count() const {
    u32 idx = 0;
    walk(m_root, idx, kNone, 0, [](const DesignNode&, u32, u32, u32) { return false; });
    return idx;
}
const DesignNode* DesignDocument::node(u32 index) const { return const_cast<DesignDocument*>(this)->nodeMut(index); }
DesignNode* DesignDocument::nodeMut(u32 index) {
    DesignNode* hit = nullptr;
    u32 idx = 0;
    walk(m_root, idx, kNone, 0, [&](DesignNode& n, u32 me, u32, u32) { if (me == index) { hit = &n; return true; } return false; });
    return hit;
}
u32 DesignDocument::parentOf(u32 index) const {
    u32 p = kNone, idx = 0;
    walk(m_root, idx, kNone, 0, [&](const DesignNode&, u32 me, u32 parent, u32) { if (me == index) { p = parent; return true; } return false; });
    return p;
}
u32 DesignDocument::depthOf(u32 index) const {
    u32 d = 0, idx = 0;
    walk(m_root, idx, kNone, 0, [&](const DesignNode&, u32 me, u32, u32 depth) { if (me == index) { d = depth; return true; } return false; });
    return d;
}
u32 DesignDocument::findId(std::string_view id) const {
    u32 hit = kNone, idx = 0;
    walk(m_root, idx, kNone, 0, [&](const DesignNode& n, u32 me, u32, u32) { if (n.id() == id) { hit = me; return true; } return false; });
    return hit;
}
bool DesignDocument::isContainer(std::string_view cls) { return cls == "panel" || cls == "scrollarea" || cls == "card"; }

void DesignDocument::checkpoint() {
    m_undo.push_back(save());
    if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    m_redo.clear();
    ++m_rev;
}

u32 DesignDocument::ensureIds() {
    u32 added = 0, idx = 0;
    walk(m_root, idx, kNone, 0, [&](DesignNode& n, u32, u32, u32) {
        if (n.id().empty()) {
            std::string base(n.cls());
            for (int k = 1;; ++k) {
                const std::string cand = base + std::to_string(k);
                if (findId(cand) == kNone) { setMember(n.props, "id", str(cand)); break; }
            }
            ++added;
        }
        return false;
    });
    return added;
}

u32 DesignDocument::insert(u32 parent, std::string_view cls) {
    DesignNode* p = nodeMut(parent);
    if (!p) return kNone;
    checkpoint();
    DesignNode n;
    n.props.type = JsonValue::Type::Object;
    n.props.object.emplace_back("class", str(cls));
    std::string base(cls);
    for (int k = 1;; ++k)
        if (findId(base + std::to_string(k)) == kNone) { n.props.object.emplace_back("id", str(base + std::to_string(k))); break; }
    if (cls == "button" || cls == "checkbox" || cls == "label" || cls == "switch" || cls == "radiobutton") n.props.object.emplace_back("text", str(cls == "label" ? "Label" : (cls == "button" ? "Button" : "Option")));
    if (cls == "slider" || cls == "progressbar" || cls == "spinbox") { n.props.object.emplace_back("min", num(0)); n.props.object.emplace_back("max", num(100)); n.props.object.emplace_back("value", num(50)); }
    if (cls == "combobox") { JsonValue items; items.type = JsonValue::Type::Array; items.array = {str("One"), str("Two"), str("Three")}; n.props.object.emplace_back("items", items); }
    const std::string id(n.id());
    if (isContainer(p->cls())) {
        p->children.push_back(std::move(n));
    } else {
        // after the selected leaf, in its parent
        const u32 pp = parentOf(parent);
        DesignNode* gp = nodeMut(pp);
        if (!gp) { p->children.push_back(std::move(n)); return findId(id); }
        auto it = std::find_if(gp->children.begin(), gp->children.end(), [&](const DesignNode& c) { return &c == p; });
        gp->children.insert(it == gp->children.end() ? gp->children.end() : it + 1, std::move(n));
    }
    return findId(id);
}

bool DesignDocument::remove(u32 index) {
    const u32 p = parentOf(index);
    DesignNode* parent = nodeMut(p);
    DesignNode* n = nodeMut(index);
    if (!parent || !n) return false;
    checkpoint();
    parent->children.erase(std::find_if(parent->children.begin(), parent->children.end(), [&](const DesignNode& c) { return &c == n; }));
    return true;
}

u32 DesignDocument::move(u32 index, int delta) {
    DesignNode* parent = nodeMut(parentOf(index));
    DesignNode* n = nodeMut(index);
    if (!parent || !n) return kNone;
    auto& v = parent->children;
    const std::ptrdiff_t at = std::find_if(v.begin(), v.end(), [&](const DesignNode& c) { return &c == n; }) - v.begin();
    const std::ptrdiff_t to = at + delta;
    if (to < 0 || to >= std::ptrdiff_t(v.size())) return kNone;
    checkpoint();
    const std::string id(n->id());
    std::swap(v[std::size_t(at)], v[std::size_t(to)]);
    return id.empty() ? kNone : findId(id);
}

bool DesignDocument::setProp(u32 index, std::string_view key, const JsonValue& value) {
    DesignNode* n = nodeMut(index);
    if (!n || key == "class" || key == "children") return false;
    checkpoint();
    if (key.starts_with("style.")) {
        const std::string_view sk = key.substr(6);
        JsonValue* st = memberMut(n->props, "style");
        if (value.isNull()) {
            if (st) { eraseMember(*st, sk); if (st->object.empty()) eraseMember(n->props, "style"); }
            return true;
        }
        if (!st) { JsonValue o; o.type = JsonValue::Type::Object; setMember(n->props, "style", o); st = memberMut(n->props, "style"); }
        setMember(*st, sk, value);
        return true;
    }
    if (value.isNull()) eraseMember(n->props, key);
    else setMember(n->props, key, value);
    return true;
}

std::string DesignDocument::prop(u32 index, std::string_view key) const {
    const DesignNode* n = node(index);
    if (!n) return {};
    const JsonValue* v = nullptr;
    if (key.starts_with("style.")) {
        if (const JsonValue* st = member(n->props, "style")) v = member(*st, key.substr(6));
    } else {
        v = member(n->props, key);
    }
    if (!v) return {};
    if (v->isString()) return v->string;
    if (v->isNumber()) return numText(v->number);
    if (v->isBool()) return v->boolean ? "true" : "false";
    if (v->isArray()) {
        std::string s;
        for (const JsonValue& e : v->array) { if (!s.empty()) s += ' '; s += e.isString() ? e.string : writeJson(e); }
        return s;
    }
    return writeJson(*v);
}

bool DesignDocument::undo() {
    if (m_undo.empty()) return false;
    std::string cur = save();
    std::string prev = std::move(m_undo.back());
    m_undo.pop_back();
    auto undo = std::move(m_undo);
    auto redo = std::move(m_redo);
    load(prev);
    m_undo = std::move(undo);
    m_redo = std::move(redo);
    m_redo.push_back(std::move(cur));
    return true;
}
bool DesignDocument::redo() {
    if (m_redo.empty()) return false;
    std::string cur = save();
    std::string next = std::move(m_redo.back());
    m_redo.pop_back();
    auto undo = std::move(m_undo);
    auto redo = std::move(m_redo);
    load(next);
    m_undo = std::move(undo);
    m_redo = std::move(redo);
    m_undo.push_back(std::move(cur));
    return true;
}

// ---- Designer UI ------------------------------------------------------------------------------------------------------
namespace {
Designer* designerOf(UiContext& ui, NodeHandle canvas) {
    return reinterpret_cast<Designer*>(static_cast<std::uintptr_t>(ui.userData(canvas)));
}
// The canvas host: in the capture phase it turns presses on the live document into selection and swallows them
// (design mode), so buttons do not click and fields do not take focus.
struct CanvasW {
    static constexpr std::string_view kName = "designcanvas";
    static constexpr A11yRole kRole = A11yRole::Group;
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Capture && ec.phase != Phase::Target) return false;
        switch (e.type) {
        case EventType::PointerDown:
            if (Designer* d = designerOf(ec.ui, ec.node)) d->canvasPick(ec.target);
            return true;
        case EventType::PointerUp: case EventType::Click: case EventType::Wheel: case EventType::KeyDown: case EventType::Text:
            return ec.target != ec.node;
        default: return false;
        }
    }
};

struct PropRow { const char* name; const char* key; };
const PropRow kRows[] = {
    {"Node", nullptr}, {"id", "id"}, {"text", "text"}, {"classes", "classes"}, {"min", "min"}, {"max", "max"}, {"value", "value"},
    {"Layout", nullptr}, {"width", "style.width"}, {"height", "style.height"}, {"padding", "style.padding"}, {"gap", "style.gap"},
    {"flex-grow", "style.flex-grow"}, {"flex-direction", "style.flex-direction"},
};
const PropRow* rowNamed(std::string_view n) {
    for (const PropRow& r : kRows)
        if (r.key && n == r.name) return &r;
    return nullptr;
}
JsonValue parseValue(std::string_view key, std::string_view text) {
    if (text.empty()) return JsonValue{};
    if (key == "classes") {
        JsonValue a;
        a.type = JsonValue::Type::Array;
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && text[i] == ' ') ++i;
            std::size_t j = i;
            while (j < text.size() && text[j] != ' ') ++j;
            if (j > i) a.array.push_back(str(text.substr(i, j - i)));
            i = j;
        }
        return a;
    }
    if (key != "id" && key != "text") {
        double d = 0.0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), d);
        if (r.ec == std::errc{} && r.ptr == text.data() + text.size()) return num(d);
    }
    return str(text);
}
} // namespace

std::string_view Designer::paletteClasses() {
    return "panel\nlabel\nbutton\ncheckbox\nswitch\nradiobutton\nslider\nprogressbar\nspinbox\ncombobox\nlineedit\nseparator";
}

Designer::Designer(UiContext& ui, NodeHandle parent) : m_ui(ui) {
    w::install(ui);
    if (ui.registry().find("designcanvas") == 0xFFFFFFFFu) ui.registry().registerWidget<CanvasW>();
    ui.addStyleSheet(R"(
        .designer { gap: 0px; padding: 0px; flex-grow: 1; }
        .designer-bar { gap: 4px; padding: 6px; background: var(--surface); border: 1px var(--border); }
        .designer-side { width: 220px; gap: 6px; padding: 6px; }
        .designer-props { width: 280px; gap: 6px; padding: 6px; }
        .designer-title { color: var(--text-dim); }
        .designcanvas { flex-grow: 1; padding: 16px; background: #15171c; border: 1px var(--border); }
        .dsel { border: 2px #4aa3ff; }
        .designer-status { padding: 4px 8px; color: var(--text-dim); }
    )");
    if (!ui.alive(parent)) parent = ui.root();
    m_root = w::panel(ui, parent, "designer");
    ui.addClass(m_root, "designer");
    m_toolbar = w::panel(ui, m_root, "designer.toolbar");
    ui.addClass(m_toolbar, "row");
    ui.addClass(m_toolbar, "designer-bar");
    struct Btn { const char* text; const char* id; };
    for (const Btn b : {Btn{"New", "designer.new"}, Btn{"Undo", "designer.undo"}, Btn{"Redo", "designer.redo"}, Btn{"Delete", "designer.delete"},
                        Btn{"Up", "designer.up"}, Btn{"Down", "designer.down"}}) {
        const NodeHandle h = w::toolButton(ui, m_toolbar, b.text, b.id);
        const std::string_view id = b.id; // string literal: static storage
        ui.connect(h, SignalKind::Clicked, [this, id](UiContext&, NodeHandle, const SignalArgs&) {
            if (id == "designer.new") { m_doc.reset(); m_sel = 0; rebuild(); setStatus("New document"); }
            else if (id == "designer.undo") undo();
            else if (id == "designer.redo") redo();
            else if (id == "designer.delete") deleteSelected();
            else if (id == "designer.up") moveSelected(-1);
            else moveSelected(1);
        });
    }
    const NodeHandle body = w::panel(ui, m_root, "designer.body");
    ui.addClass(body, "row");
    ui.setCss(body, "flex-grow", "1");
    ui.setCss(body, "gap", "0px");
    ui.setCss(body, "align-items", "stretch");
    const NodeHandle side = w::panel(ui, body, "designer.side");
    ui.addClass(side, "designer-side");
    ui.addClass(w::label(ui, side, "Widgets"), "designer-title");
    m_palette = w::listView(ui, side, paletteClasses(), -1, "designer.palette");
    ui.setCss(m_palette, "height", "230px");
    ui.connect(m_palette, SignalKind::Submitted, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
        const i32 i = i32(u.value(n));
        if (i >= 0) addWidget(w::listItem(u, n, u32(i)));
    });
    ui.addClass(w::label(ui, side, "Outline"), "designer-title");
    m_outline = w::treeView(ui, side, "", "designer.outline");
    ui.setCss(m_outline, "flex-grow", "1");
    ui.connect(m_outline, SignalKind::ValueChanged, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
        if (!m_syncing) select(u32(std::max(0.f, u.value(n))));
    });
    m_canvas = ui.create("designcanvas", body, "designer.canvas").value_or(NodeHandle{});
    ui.addClass(m_canvas, "designcanvas");
    ui.setUserData(m_canvas, static_cast<u64>(reinterpret_cast<std::uintptr_t>(this)));
    const NodeHandle right = w::panel(ui, body, "designer.right");
    ui.addClass(right, "designer-props");
    ui.addClass(w::label(ui, right, "Properties"), "designer-title");
    m_props = w::propertyGrid(ui, right, "designer.props");
    m_statusLabel = w::label(ui, m_root, "", "designer.status");
    ui.addClass(m_statusLabel, "designer-status");
    m_doc.reset();
    rebuild();
}

Designer::~Designer() {
    m_live.reset();
    if (m_ui.alive(m_root)) m_ui.destroy(m_root);
}

void Designer::setStatus(std::string s) {
    m_status = std::move(s);
    m_ui.setText(m_statusLabel, m_status);
}

bool Designer::open(std::string_view json, std::string* error) {
    std::string err;
    if (!m_doc.load(json, &err)) {
        setStatus("Open failed: " + err);
        if (error) *error = err;
        return false;
    }
    m_doc.ensureIds();
    m_sel = 0;
    rebuild();
    setStatus("Opened (" + std::to_string(m_doc.count()) + " nodes)");
    return true;
}

bool Designer::openFile(const std::string& path, std::string* error) {
    std::string text;
    if (!doc::readTextFile(path.c_str(), text)) {
        if (error) *error = "cannot read " + path;
        setStatus("Cannot read " + path);
        return false;
    }
    return open(text, error);
}

bool Designer::saveFile(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    const std::string t = m_doc.save();
    f.write(t.data(), std::streamsize(t.size()));
    const bool ok = bool(f);
    setStatus(ok ? "Saved " + path : "Cannot write " + path);
    return ok;
}

void Designer::rebuild() {
    m_live.reset();
    m_map.clear();
    // preview model: binding variables resolve to defaults
    auto live = doc::Document::load(m_ui, m_doc.previewText(), &m_model, m_canvas);
    if (live) {
        m_live = std::move(*live);
    } else {
        setStatus("Preview error at " + live.error().path + ": " + live.error().message);
    }
    for (u32 i = 0, n = m_doc.count(); i < n; ++i) {
        const std::string_view id = m_doc.node(i)->id();
        if (id.empty()) continue;
        const NodeHandle h = m_ui.find(id);
        if (m_ui.alive(h)) m_map.emplace_back(h, i);
    }
    if (m_sel >= m_doc.count()) m_sel = 0;
    refreshOutline();
    select(m_sel);
}

void Designer::refreshOutline() {
    std::string lines;
    for (u32 i = 0, n = m_doc.count(); i < n; ++i) {
        const DesignNode* d = m_doc.node(i);
        lines.append(m_doc.depthOf(i), '\t');
        lines += d->cls();
        if (!d->id().empty()) { lines += "  #"; lines += d->id(); }
        if (i + 1 < n) lines += '\n';
    }
    m_ui.setText(m_outline, lines);
    m_ui.invalidate(m_outline);
}

NodeHandle Designer::canvasNode(u32 index) const {
    for (const auto& [h, i] : m_map)
        if (i == index) return h;
    return {};
}

void Designer::select(u32 index) {
    if (index >= m_doc.count()) return;
    if (const NodeHandle old = canvasNode(m_sel); m_ui.alive(old)) m_ui.removeClass(old, "dsel");
    m_sel = index;
    if (const NodeHandle h = canvasNode(m_sel); m_ui.alive(h)) m_ui.addClass(h, "dsel");
    m_syncing = true;
    m_ui.setValue(m_outline, f32(m_sel));
    m_syncing = false;
    refreshProperties();
}

void Designer::refreshProperties() {
    // rebuild the grid for the selection (rows depend on the node)
    const NodeHandle parent = m_ui.parent(m_props);
    m_ui.destroy(m_props);
    m_props = w::propertyGrid(m_ui, parent, "designer.props");
    m_propNames.clear();
    const DesignNode* d = m_doc.node(m_sel);
    if (!d) return;
    const std::string title = std::string(d->cls());
    for (const PropRow& r : kRows) {
        if (!r.key) {
            w::propertyRow(m_ui, m_props, {.name = r.name[0] == 'N' ? std::string_view(title) : std::string_view(r.name), .kind = w::PropKind::Group});
            m_propNames.emplace_back();
            continue;
        }
        w::propertyRow(m_ui, m_props, {.name = r.name, .kind = w::PropKind::Text}, m_doc.prop(m_sel, r.key));
        m_propNames.emplace_back(r.name);
    }
    m_ui.connect(m_props, SignalKind::Submitted, [this](UiContext& u, NodeHandle g, const SignalArgs& a) {
        const u32 row = u32(a.value);
        if (row < m_propNames.size() && !m_propNames[row].empty()) setProperty(m_propNames[row], w::propertyValue(u, g, row));
    });
}

void Designer::canvasPick(NodeHandle target) {
    for (NodeHandle h = target; m_ui.alive(h) && h != m_canvas; h = m_ui.parent(h))
        for (const auto& [n, i] : m_map)
            if (n == h) {
                select(i);
                setStatus("Selected " + std::string(m_doc.node(i)->cls()) + " #" + std::string(m_doc.node(i)->id()));
                return;
            }
}

u32 Designer::addWidget(std::string_view cls) {
    const u32 i = m_doc.insert(m_sel, cls);
    if (i == DesignDocument::kNone) return i;
    m_sel = i;
    rebuild();
    setStatus("Added " + std::string(cls));
    return i;
}

bool Designer::setProperty(std::string_view name, std::string_view value) {
    const PropRow* r = rowNamed(name);
    if (!r) return false;
    if (m_doc.prop(m_sel, r->key) == value) return true;
    const std::string id(m_doc.node(m_sel)->id());
    if (!m_doc.setProp(m_sel, r->key, parseValue(r->key, value))) return false;
    // keep the selection by id across the rebuild (an id edit renames it)
    const u32 again = m_doc.findId(std::string_view(r->key) == "id" ? value : std::string_view(id));
    if (again != DesignDocument::kNone) m_sel = again;
    rebuild();
    setStatus("Set " + std::string(name));
    return true;
}

void Designer::deleteSelected() {
    const u32 p = m_doc.parentOf(m_sel);
    if (p == DesignDocument::kNone || !m_doc.remove(m_sel)) { setStatus("The root cannot be deleted"); return; }
    m_sel = p;
    rebuild();
    setStatus("Deleted");
}

void Designer::moveSelected(int delta) {
    const u32 i = m_doc.move(m_sel, delta);
    if (i == DesignDocument::kNone) return;
    m_sel = i;
    rebuild();
}

void Designer::undo() {
    if (m_doc.undo()) { rebuild(); setStatus("Undo"); }
}
void Designer::redo() {
    if (m_doc.redo()) { rebuild(); setStatus("Redo"); }
}

} // namespace fuse::ui::designer
