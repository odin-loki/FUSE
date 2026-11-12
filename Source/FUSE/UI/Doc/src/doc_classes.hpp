// Internal glue between Document (document.cpp), the cooker (cook.cpp) and the document-class table (doc_classes.cpp).
#pragma once

#include "fuse/ui/doc/document.hpp"

namespace fuse::ui::doc::detail {

/// A registered widget class or a document class.
bool creatable(const UiContext& ui, std::string_view cls);
/// A document class that is not itself a registered class (group box, tab widget, ...): built from several nodes.
bool compound(const UiContext& ui, std::string_view cls);
/// Creates a node of `cls` under `parent` (`n` = its JSON object, for creation-time properties). Invalid on failure.
NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id, const JsonValue& n);
/// Class-specific (or any-node: tooltip, contextMenu) property; false = not handled here.
bool prop(UiContext& ui, NodeHandle node, std::string_view cls, std::string_view key, const JsonValue& v);
/// The parent of the document's child `index` of `node` (a tab page, a splitter pane, a group box body, ...).
NodeHandle childParent(UiContext& ui, NodeHandle node, std::string_view cls, std::size_t index, const JsonValue& child);
/// "visible" on nodes that are not shown by `display` (dialogs open / close); false = use display.
bool setVisible(UiContext& ui, NodeHandle node, bool on);
/// "value" on compound nodes whose value is not the node value alone (stack page, tab widget); false = setValue.
bool setValue(UiContext& ui, NodeHandle node, f32 v);

} // namespace fuse::ui::doc::detail
