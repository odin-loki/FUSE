// fuse_ui_designer (plan UI8, prototype): a visual editor for .fuseui documents built with FUSE UI itself.
//   DesignDocument - the editable model: a tree of nodes whose properties are kept verbatim as JSON (so bindings,
//                    "on" actions, items and unknown keys survive a load / save round trip), preorder indexing,
//                    insert / remove / move / set-property edits with snapshot undo / redo, pretty JSON output.
//   Designer       - the UI: toolbar, widget palette, outline tree, a live canvas (the document instantiated through
//                    fuse_ui_doc, so what you see is what loads at runtime) and a property grid for the selection.
//                    Canvas clicks select instead of operating the widgets.
#pragma once

#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/ui.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::designer {

struct DesignNode {
    doc::JsonValue props; ///< object: every member of the node except "children"
    std::vector<DesignNode> children;
    std::string_view cls() const;
    std::string_view id() const;
};

/// JSON text for a value: pretty-printed with 2-space indentation (objects of scalars stay on one line).
std::string writeJson(const doc::JsonValue& v, int indent = 0);

class DesignDocument {
public:
    static constexpr u32 kNone = ~0u;
    bool load(std::string_view json, std::string* error = nullptr);
    std::string save() const;
    /// save() without editor-only "x-*" keys (which fuse_ui_doc rejects): the text the live preview loads.
    std::string previewText() const;
    /// New empty document: a "panel" root.
    void reset();

    u32 count() const; ///< nodes in preorder
    const DesignNode* node(u32 index) const;
    u32 parentOf(u32 index) const;
    u32 depthOf(u32 index) const;
    /// Insert a new `cls` node as the last child of `parent` (or after `parent` when it is not a container class);
    /// returns its preorder index. Gives it a unique id.
    u32 insert(u32 parent, std::string_view cls);
    bool remove(u32 index); ///< not the root
    /// Move among siblings by delta (-1 up, +1 down); returns the new preorder index (or kNone).
    u32 move(u32 index, int delta);
    /// Set (or erase, when `value` is null) a node member; "style.<prop>" addresses the node's style object.
    bool setProp(u32 index, std::string_view key, const doc::JsonValue& value);
    /// Member as display text ("" when missing): strings verbatim, numbers shortest, arrays joined by spaces.
    std::string prop(u32 index, std::string_view key) const;
    /// Index of the node with `id`, else kNone.
    u32 findId(std::string_view id) const;
    /// Give every node without an id a unique one ("<class><n>"). Returns how many were added.
    u32 ensureIds();

    bool undo();
    bool redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    u64 revision() const { return m_rev; } ///< bumps on every edit, undo and redo

    static bool isContainer(std::string_view cls);

private:
    DesignNode* nodeMut(u32 index);
    void checkpoint();
    doc::JsonValue m_header; ///< top-level members except "root" (fuseui version, style, ...)
    DesignNode m_root;
    std::vector<std::string> m_undo, m_redo;
    u64 m_rev = 0;
};

class Designer {
public:
    Designer(UiContext& ui, NodeHandle parent = {});
    ~Designer();
    Designer(const Designer&) = delete;
    Designer& operator=(const Designer&) = delete;

    bool open(std::string_view json, std::string* error = nullptr);
    bool openFile(const std::string& path, std::string* error = nullptr);
    bool saveFile(const std::string& path);
    std::string text() const { return m_doc.save(); }
    DesignDocument& document() { return m_doc; }

    void select(u32 index);
    u32 selected() const { return m_sel; }
    /// The canvas node instantiated for design node `index` (invalid when it has no id or failed to build).
    NodeHandle canvasNode(u32 index) const;
    /// Insert a palette class under the selection (or after it) and select it.
    u32 addWidget(std::string_view cls);
    /// Edit the selection: property grid row names ("id", "text", "classes", "width", ...).
    bool setProperty(std::string_view name, std::string_view value);
    void deleteSelected();
    void moveSelected(int delta);
    void undo();
    void redo();

    NodeHandle canvas() const { return m_canvas; }
    NodeHandle outline() const { return m_outline; }
    NodeHandle palette() const { return m_palette; }
    NodeHandle properties() const { return m_props; }
    const std::string& status() const { return m_status; }
    /// Palette classes ('\n'-separated, the order shown).
    static std::string_view paletteClasses();

    // internal (canvas widget callbacks)
    void canvasPick(NodeHandle target);

private:
    void rebuild();
    void refreshOutline();
    void refreshProperties();
    void setStatus(std::string s);
    UiContext& m_ui;
    DesignDocument m_doc;
    doc::Model m_model;
    std::unique_ptr<doc::Document> m_live;
    std::vector<std::pair<NodeHandle, u32>> m_map; ///< canvas node -> design index
    NodeHandle m_root, m_toolbar, m_palette, m_outline, m_canvas, m_props, m_statusLabel;
    u32 m_sel = 0;
    bool m_syncing = false;
    std::string m_status;
    std::vector<std::string> m_propNames;
};

} // namespace fuse::ui::designer
