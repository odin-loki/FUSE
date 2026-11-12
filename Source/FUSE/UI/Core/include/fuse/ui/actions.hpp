// FUSE UI actions and keyboard shortcuts (QAction / QShortcut; plan §4.7).
//
// An ActionRegistry belongs to one UiContext. An action has a string id, a text, an optional shortcut, enabled /
// checkable / checked state and a triggered callback. The registry installs itself as the context's shortcut hook: a
// KeyDown the focused widget did not consume is matched against the shortcuts with these scope rules:
//   - Subtree scope: the action fires while the focus is inside its scope node (the nearest scope wins);
//   - Window scope: fires wherever the focus is, but not while a modal scope (dialog, popup menu) is open, unless the
//     action names a scope node inside that modal scope;
//   - while a text field has the focus, plain typing keys (letters, digits, space, editing and caret keys, without
//     Ctrl / Alt / Super) are never shortcuts, so a single-letter shortcut does not fire while editing;
//   - disabled actions do not match (the key stays unconsumed).
// bind() keeps a button / tool button / check box / switch in sync with an action (text, enabled, checked) and
// triggers it on click; menus are built from action lists (fuse/ui/widgets/interaction.hpp: actionMenuBarMenu,
// actionPopupMenu), where the items show the shortcut right-aligned.
//
// The core does not depend on the engine's ActionMap (fuse_core): ids are strings and shortcuts round-trip through
// text ("Ctrl+Shift+S"; saveShortcuts / loadShortcuts), which is what an adapter to another binding store needs.
#pragma once

#include "fuse/ui/ui.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::ui {

struct Shortcut {
    Key key = Key::None;
    u8 mods = 0; ///< kMod* bits
    bool valid() const { return key != Key::None && key != Key::Other; }
    friend bool operator==(const Shortcut&, const Shortcut&) = default;
};
/// The platform's primary shortcut modifier: kModSuper (Cmd) on macOS / iOS, kModCtrl elsewhere.
u8 primaryModifier();
/// Overrides primaryModifier() (0 restores the platform default); tests and "use Ctrl on a Mac" preferences.
void setPrimaryModifier(u8 mod);
/// Primary+key, e.g. primaryShortcut(Key::S, kModShift) = Ctrl+Shift+S (Cmd+Shift+S on a Mac).
inline Shortcut primaryShortcut(Key k, u8 extraMods = 0) { return {k, u8(primaryModifier() | extraMods)}; }

/// "S", "F5", "PageUp", "[" ...; empty for Key::None / Key::Other.
std::string_view keyName(Key k);
bool keyFromName(std::string_view name, Key& out); ///< case-insensitive; also "Esc", "Del", "Return", "Ins"-less set
/// "Ctrl+Alt+Shift+S" (modifier order Ctrl, Alt, Shift, Super). Super prints as "Cmd" on Apple platforms, "Super"
/// elsewhere. Empty for an invalid shortcut.
std::string shortcutToString(Shortcut s);
/// Parses what shortcutToString writes, case-insensitively, with the aliases Control, Option, Cmd / Command / Meta /
/// Win / Super, and Primary / CmdOrCtrl (the platform's primary modifier). False when malformed.
bool parseShortcut(std::string_view text, Shortcut& out);

enum class ActionScope : u8 { Window, Subtree };
struct ActionHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
    friend bool operator==(const ActionHandle&, const ActionHandle&) = default;
};
struct ActionDesc {
    std::string_view id;   ///< unique, stable ("file.save")
    std::string_view text; ///< label of bound widgets and menu items
    Shortcut shortcut{};
    bool enabled = true, checkable = false, checked = false;
    ActionScope scope = ActionScope::Window;
    NodeHandle scopeNode{}; ///< Subtree: required. Window: optional owner (a dialog) - see the scope rules above
};
/// Triggered callback: `checked` is the new checked state of a checkable action (false otherwise).
using ActionFn = InlineFn<void(UiContext&, ActionHandle, bool checked)>;

class ActionRegistry {
public:
    explicit ActionRegistry(UiContext& ui); ///< installs the context's shortcut hook
    ~ActionRegistry();
    ActionRegistry(const ActionRegistry&) = delete;
    ActionRegistry& operator=(const ActionRegistry&) = delete;

    ActionHandle add(const ActionDesc& desc, ActionFn onTriggered = {}); ///< invalid when the id is taken or empty
    void remove(ActionHandle a);
    bool alive(ActionHandle a) const { return get(a) != nullptr; }
    ActionHandle find(std::string_view id) const;
    u32 count() const; ///< live actions
    /// Visits every live action in creation order (rebinding screens).
    template <class F>
    void each(F&& f) const {
        for (u32 i = 0; i < m_actions.size(); ++i)
            if (m_actions[i].alive) f(ActionHandle{i, m_actions[i].generation});
    }

    std::string_view id(ActionHandle a) const;
    std::string_view text(ActionHandle a) const;
    Shortcut shortcut(ActionHandle a) const;
    bool enabled(ActionHandle a) const;
    bool checkable(ActionHandle a) const;
    bool checked(ActionHandle a) const;
    void setText(ActionHandle a, std::string_view text);
    void setEnabled(ActionHandle a, bool on);
    void setChecked(ActionHandle a, bool on); ///< no callback (state sync only)
    void setTriggered(ActionHandle a, ActionFn fn);

    /// Runs the action as a click / shortcut does: toggles a checkable action, calls the callback. False when the
    /// action is disabled or gone.
    bool trigger(ActionHandle a);
    /// Shortcut matching (what the context's hook calls). True when an action fired.
    bool handleKey(Key k, u8 mods);
    /// The action a KeyDown would fire right now (focus, modal scope and the text-field rule applied); invalid: none.
    ActionHandle match(Key k, u8 mods) const;

    /// The live action that already owns `s` in the same scope (same kind and scope node) as `a`; invalid: none.
    ActionHandle conflict(ActionHandle a, Shortcut s) const;
    /// Rebinds `a`. On a conflict nothing changes and the other action is returned, unless `steal`: then the other
    /// action loses its shortcut. An invalid shortcut ({}) unbinds.
    std::expected<void, ActionHandle> rebind(ActionHandle a, Shortcut s, bool steal = false);
    /// "id=Ctrl+S" lines for every action (an empty value = unbound); loadShortcuts applies such lines to the actions
    /// it knows (unknown ids and malformed lines are skipped, conflicts steal) and returns how many it applied.
    std::string saveShortcuts() const;
    u32 loadShortcuts(std::string_view text);

    /// Keeps `widget` (button, tool button, check box, switch, radio) in sync with `a`: text, disabled, checked; a
    /// click triggers the action. One action can drive several widgets.
    void bind(ActionHandle a, NodeHandle widget);
    void unbind(NodeHandle widget);

    /// Ordered action lists (menus, tool bars). An invalid handle in a list is a separator.
    u32 addList(std::initializer_list<ActionHandle> actions);
    u32 addList(const ActionHandle* actions, u32 n);
    const std::vector<ActionHandle>& list(u32 listId) const;
    /// `fn(registry, node, user)` runs after any action changes while `node` is alive (menus rebuild their text).
    using ObserverFn = void (*)(ActionRegistry&, NodeHandle node, u64 user);
    void observe(NodeHandle node, ObserverFn fn, u64 user);

    UiContext& ui() { return m_ui; }

private:
    struct Action {
        std::string id, text;
        Shortcut shortcut;
        bool enabled = true, checkable = false, checked = false, alive = false;
        ActionScope scope = ActionScope::Window;
        NodeHandle scopeNode;
        ActionFn fn;
        u32 generation = 0;
    };
    struct Binding {
        NodeHandle node;
        ActionHandle action;
        ConnectionHandle conn;
        ObserverFn observer = nullptr;
        u64 user = 0;
    };
    const Action* get(ActionHandle a) const;
    Action* get(ActionHandle a) { return const_cast<Action*>(std::as_const(*this).get(a)); }
    void changed(ActionHandle a);
    void sync(const Binding& b);

    UiContext& m_ui;
    std::vector<Action> m_actions;
    std::vector<Binding> m_bindings;
    std::vector<std::vector<ActionHandle>> m_lists;
};

} // namespace fuse::ui
