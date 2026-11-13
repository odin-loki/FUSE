// FUSE UI declarative documents (plan §4.9, decision D4): `.fuseui` JSON trees with properties, style, bindings and
// event -> action names, plus the binding model and its expression bytecode.
//
//   {
//     "fuseui": 1,
//     "style": ".hud { gap: 4px; }",                       // optional sheet text added to the context
//     "root": {
//       "class": "panel", "id": "hud", "classes": ["hud"], "style": { "padding": "8px" },
//       "children": [
//         { "class": "label",  "text": "HP {player.health / player.maxHealth * 100:0}%" },
//         { "class": "slider", "id": "vol", "min": 0, "max": 1, "value": "{=audio.volume}" },  // two-way
//         { "class": "button", "text": "Save", "disabled": "{busy}", "on": { "clicked": "save" } }
//       ]
//     }
//   }
//
// Bindings are `{expr}` (optionally `{expr:N}` = N decimals) inside strings; `{=name}` is a two-way binding to one
// model variable. Expressions: numbers, dotted names, + - * / %, unary -, comparisons (< <= > >= == !=), && || !,
// parentheses. They compile to a small stack bytecode, so simple bindings need no Lua. Model changes are picked up by
// Document::refresh() (only bindings whose variables changed re-evaluate; no allocation when nothing changed).
#pragma once

#include "fuse/ui/doc/json.hpp"
#include "fuse/ui/ui.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::doc {

struct CookAccess; // cook.cpp: (de)serialises compiled templates / expressions

/// Named values (numbers or strings) that bindings read and two-way bindings write.
class Model {
public:
    u32 slot(std::string_view name); ///< creates the variable (number 0) when missing
    u32 find(std::string_view name) const; ///< 0xFFFFFFFF when missing
    void set(std::string_view name, double v) { setNumber(slot(name), v); }
    void set(std::string_view name, std::string_view v) { setString(slot(name), v); }
    void setNumber(u32 slot, double v);
    void setString(u32 slot, std::string_view v);
    double number(std::string_view name) const;
    std::string_view string(std::string_view name) const;
    double numberAt(u32 slot) const { return m_vars[slot].number; }
    bool isString(u32 slot) const { return m_vars[slot].isString; }
    std::string_view stringAt(u32 slot) const { return m_vars[slot].text; }
    u64 versionAt(u32 slot) const { return m_vars[slot].version; }
    u64 version() const { return m_version; }
    u32 size() const { return u32(m_vars.size()); }
    std::string_view nameAt(u32 slot) const { return m_vars[slot].name; }

private:
    struct Var {
        std::string name;
        double number = 0.0;
        std::string text;
        bool isString = false;
        u64 version = 0;
    };
    std::vector<Var> m_vars;
    u64 m_version = 0;
};

/// Compiled binding expression (stack bytecode).
class Expr {
public:
    static std::expected<Expr, std::string> compile(std::string_view src, Model& model);
    double eval(const Model& m) const;
    /// When the expression is a single variable: its slot (string-valued variables render as text), else ~0u.
    u32 singleVar() const { return m_single; }
    const std::vector<u32>& deps() const { return m_deps; }
    /// Number of Expr::compile calls so far in the process (cooked loading must not compile).
    static u64 compileCount();

    enum class Op : u8 { Num, Var, Add, Sub, Mul, Div, Mod, Neg, Not, Lt, Le, Gt, Ge, Eq, Ne, And, Or };
    struct Ins {
        Op op;
        u32 slot = 0;
        double num = 0.0;
    };

private:
    friend struct CookAccess;
    std::vector<Ins> m_code;
    std::vector<u32> m_deps;
    u32 m_single = ~0u;
    u32 m_maxStack = 0;
};

/// A string with `{expr}` holes.
class Template {
public:
    static std::expected<Template, std::string> compile(std::string_view src, Model& model);
    bool constant() const { return m_parts.size() <= 1 && (m_parts.empty() || m_parts[0].lit); }
    void render(const Model& m, std::string& out) const;
    void deps(std::vector<u32>& out) const;
    /// `{=name}` alone: the two-way slot, else ~0u.
    u32 twoWay() const { return m_twoWay; }
    /// Single `{expr}` with no literal text (number-valued properties).
    const Expr* onlyExpr() const { return m_parts.size() == 1 && !m_parts[0].lit ? &m_parts[0].expr : nullptr; }

private:
    friend struct CookAccess;
    struct Part {
        bool lit = true;
        std::string text;
        Expr expr;
        int decimals = -1;
    };
    std::vector<Part> m_parts;
    u32 m_twoWay = ~0u;
};

/// Document classes: the names a node's "class" may use besides the widget classes registered with the context.
/// Built in (doc_classes.cpp), with their extra properties:
///   doublespinbox, plaintextedit, scrollarea, toolbar, mainwindow (children go to the central region)
///   listview / treeview / tabbar / header / combobox  "items": [text...] (tree lines start with tabs for depth)
///   menubutton (in a menubar)   "text": title, "items": [entry...]
///   modellistview, gridview     "items": [text...]; gridview also "tileWidth", "tileHeight"
///   tableview                   "columns": [title...], "rows": [[cell...]...]   (the document owns the model)
///   treemodelview               "columns": [title...], "items": [line...] - the tabs at the start of a line are its
///                               depth, the tabs after them separate the cells; "editable": bool, "expanded": bool
///                               (the document owns a StringTreeModel)
///   svgimage                    "icon": a built-in icon name (fuse/ui/icons.hpp: "save", "folder", ...) or "svg":
///                               inline SVG source; sized by the style
///   button / toolbutton         "icon": a built-in icon name (drawn left of the text)
///   tabwidget                   "tabs": [title...]; child k is the content of page k; "value" = current tab
///   stack                       every child is a page; "value" = current page (bindable)
///   splitter                    "stacked": bool; every child gets a pane (a child's "paneSize" px fixes it)
///   dockarea / dock             dock children ("text" title, "side": left|right|bottom|center, "float": [x,y,w,h])
///                               become dock widgets, other children go to the centre
///   groupbox                    "text" title, "checkable", "checked"; children go to the body
///   statusbar                   "text" = the message; children are permanent widgets
///   dialog                      "text" title; children go to the body; "visible" (bindable) opens / closes it.
///                               The node with the id is the dialog's popup scope (its first child is the card)
///   propertygrid                "rows": [{ "name", "kind": group|bool|number|text|enum|color, "value", "min", "max",
///                               "step", "options": [...] }]
///   codeedit "language": lua|cpp|css|plain;  colorpicker "color": "#RRGGBB[AA]";  calendar "date": "YYYY-MM-DD";
///   datetimeedit "date": "YYYY-MM-DD HH:MM";  markdown "text";  fontpicker "families": [...], "size"
///   chart "kind": line|bar|area|scatter|flame, "series": [{ "name", "color", "values": [...] }] or "spans": [{ "name",
///   "start", "duration", "depth", "color" }];  nodegraph "nodes": [{ "title", "x", "y", "inputs", "outputs" }],
///   "links": [[fromNode, outPin, toNode, inPin]];  curveeditor "range": [t0, t1, v0, v1], "keys": [[t, v]...];
///   timeline "duration", "tracks": [{ "name", "clips": [{ "start", "end", "text", "color" }] }];
///   viewport "image", "imageWidth", "imageHeight";  video "source": "testpattern";  kineticscroll "profile"
///   lineedit                    "placeholder", "echo": normal|password|noecho|passwordEchoOnEdit, "maxLength", "readOnly",
///                               "inputMask" (QLineEdit mask syntax)
///   spinbox / doublespinbox     "prefix", "suffix", "specialValueText", "wrapping", "decimals"
///   tabwidget / tabbar          also "closable", "movable", "scrollButtons" (tab bar options)
///   editcombo                   "items": [text...], "insertPolicy": none|top|bottom|alphabetical, "placeholder";
///                               "value" = current index
///   lcdnumber                   "digits", "value" or "text", "base": 2|8|10|16
///   keysequenceedit             "text": a shortcut ("Ctrl+Shift+S")
///   toolbox                     "pages": [title...]; child k is the content of page k; "value" = the open page
///   mdiarea / mdiwindow         mdiwindow children ("text" title, "rect": [x, y, w, h]) are sub-windows of the area;
///                               their children go to the body
///   graphicsview                "items": [{ "shape": rect|ellipse|line|text|polygon, "rect": [x, y, w, h], "points":
///                               [[x, y]...], "fill", "stroke", "strokeWidth", "z", "text", "movable", "selectable" }],
///                               "zoom" (the document owns the scene)
///   vkeyboard                   the on-screen keyboard
///   dialogbuttonbox             "buttons": ["ok", "cancel", "apply", "help", ...], "layout": platform|windows|mac|kde|gnome
/// Any node: "tooltip": text, "contextMenu": [entry...] (Submitted on the node).
/// Not document nodes (they open at once, from code): messageBox, inputDialog, wizard, popover, toast, popupMenu,
/// fileDialog.
/// Apps and other modules add their own with registerDocumentClass (e.g. the game HUD widgets, whose library the
/// document module does not link).
struct DocumentClass {
    std::string name;
    /// Creates the node under `parent`; `node` is its JSON object (creation-time properties).
    NodeHandle (*make)(UiContext&, NodeHandle parent, std::string_view id, const JsonValue& node) = nullptr;
    /// Optional: applies a class-specific property; returns false for keys it does not know.
    bool (*prop)(UiContext&, NodeHandle, std::string_view key, const JsonValue& value) = nullptr;
    /// Optional: the parent for the document's child `index` of the node (default: the node).
    NodeHandle (*child)(UiContext&, NodeHandle, std::size_t index, const JsonValue& child) = nullptr;
};
/// Process-wide; a class registered again replaces the earlier one, and a custom class overrides a built-in name.
void registerDocumentClass(DocumentClass c);
bool isDocumentClass(std::string_view name);
std::vector<std::string> documentClassNames(); ///< sorted

struct DocError {
    std::string path; ///< node path, e.g. root/children[2]
    std::string message;
    u32 line = 0, column = 0; ///< JSON syntax errors
};

/// Event -> action callback: (action name, node, signal args).
using ActionFn = InlineFn<void(std::string_view, NodeHandle, const SignalArgs&), 48>;

/// Load-time localisation: maps a source string to its translation; an empty result (or the key itself) keeps the
/// source. Documents translate exactly the strings fuse_ui_extract_strings extracts, keyed the same way (the source
/// string): constant `text` (not of line / text edits), each `items` entry and `a11yName`; strings with a `{` (bindings)
/// are left alone. With a StringTable (fuse_ui_game): `[&st](std::string_view k) { return st.tr(k); }`.
using Translator = InlineFn<std::string_view(std::string_view), 32>;

class Document {
public:
    /// Parses `json` and builds the tree under `parent` (the context root when invalid). `model` may be null (bindings
    /// then fail to compile) and must outlive the document.
    static std::expected<std::unique_ptr<Document>, DocError> load(UiContext& ui, std::string_view json,
                                                                   Model* model = nullptr, NodeHandle parent = {},
                                                                   Translator translate = {});
    /// Builds from a cooked document (cookDocument() in fuse/ui/doc/cook.hpp): no JSON text, CSS or expression
    /// parsing; binding bytecode is loaded and its variables resolved against `model`. The blob is validated.
    static std::expected<std::unique_ptr<Document>, DocError> loadCooked(UiContext& ui, std::span<const u8> cooked,
                                                                         Model* model = nullptr, NodeHandle parent = {},
                                                                         Translator translate = {});
    ~Document(); ///< destroys the tree it built
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    NodeHandle root() const { return m_root; }
    void onAction(ActionFn fn) { m_action = std::move(fn); }
    /// Re-evaluates bindings whose model variables changed since the last refresh. Returns how many were applied.
    u32 refresh();
    /// Hot reload: rebuilds from new text. Nodes with an id whose class did not change are kept (same NodeHandle,
    /// so outside connections, focus, scroll, animations and edit state survive) and moved to their new place;
    /// their unbound value / checked state and edited text (line / text edits) are kept too. Other nodes are
    /// rebuilt; nodes no longer in the document are destroyed. On a JSON syntax error the old tree stays as it was.
    std::expected<void, DocError> reload(std::string_view json);
    /// Nodes reused by the last reload (identity kept).
    u32 reusedCount() const { return m_reused; }
    u32 bindingCount() const { return u32(m_bindings.size()); }
    /// Replaces the translator (used by reload() and retranslate()); an empty one shows the source strings.
    void setTranslator(Translator translate) { m_translate = translate; }
    /// Re-applies the translator to every translatable string the document set (after a locale switch). Returns how
    /// many nodes changed.
    u32 retranslate();
    /// Translatable strings the document set (text / items / a11yName properties).
    u32 translatableCount() const { return u32(m_trEntries.size()); }

private:
    Document(UiContext& ui, Model* model, NodeHandle parent) : m_ui(ui), m_model(model), m_parent(parent) {}
    std::expected<void, DocError> build(const JsonValue& doc);
    std::expected<NodeHandle, DocError> buildNode(const JsonValue& n, NodeHandle parent, std::string& path);
    std::expected<void, DocError> bindProp(NodeHandle node, std::string_view prop, const JsonValue& v,
                                           const std::string& path);
    void apply(u32 binding);

    enum class Target : u8 { Text, Value, Checked, Disabled, Visible, Min, Max };
    struct Binding {
        NodeHandle node;
        Target target;
        Template tmpl;
        std::vector<u32> deps;
    };
    friend struct CookAccess;
    std::expected<NodeHandle, DocError> makeOrReuse(std::string_view cls, NodeHandle parent, std::string_view id,
                                                    const JsonValue& n);
    std::vector<NodeHandle> m_detached; ///< nodes built outside the document's tree (dialog scopes in the overlay)
    UiContext& m_ui;
    Model* m_model;
    std::vector<std::pair<std::string, Template>> m_cooked; ///< cooked load: binding source -> compiled template
    std::optional<StyleSheet> m_cookedStyle;
    std::vector<NodeHandle> m_reuse; ///< reload: kept id nodes waiting for their new place
    std::vector<std::pair<NodeHandle, std::string>> m_nodeClasses; ///< classes the document added (reset on reuse)
    u32 m_reused = 0;
    NodeHandle m_parent, m_root;
    std::vector<Binding> m_bindings;
    std::vector<std::string> m_actions;
    std::vector<ConnectionHandle> m_connections;
    ActionFn m_action;
    u64 m_seen = 0;
    std::string m_scratch;
    enum class TrKind : u8 { Text, Items, A11yName };
    struct TrEntry {
        NodeHandle node;
        TrKind kind;
        std::string source; ///< items: the entries joined with '\n'
        std::string shown;  ///< what was last applied
    };
    Translator m_translate;
    std::vector<TrEntry> m_trEntries;
    std::string m_trScratch;
    std::string_view translated(std::string_view source) const;
    bool applyTranslated(TrEntry& e, bool force);
    void addTranslatable(NodeHandle node, TrKind kind, std::string_view source);
    bool m_applying = false;
};

/// Reads a whole file (e.g. a .fuseui) into `out`.
bool readTextFile(const char* path, std::string& out);

} // namespace fuse::ui::doc
