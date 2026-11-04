// fuse_ui_a11y: platform accessibility bridge (plan D7). The core keeps an accessible tree (roles, names, states,
// values); this module snapshots it, diffs successive snapshots, maps platform actions back onto the UiContext and,
// when AccessKit is linked (FUSE_UI_ACCESSKIT_DIR or FUSE_UI_ACCESSKIT_BUILD), feeds it: AT-SPI on Linux
// (UnixAdapter), UI Automation on Windows (WindowsAdapter; fuse::app::WindowAccessibility wires it to a native window).
#pragma once

#include "fuse/ui/ui.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::ui::a11y {

/// Stable platform id for a node: generation in the high half, slot in the low half (never 0).
inline u64 idOf(NodeHandle n) { return (u64(n.generation) << 32) | u64(n.index + 1u); }
inline NodeHandle nodeOf(u64 id) { return {u32(id & 0xFFFFFFFFu) - 1u, u32(id >> 32)}; }

enum class Action : u8 { Focus, Click, Increment, Decrement, SetValue, SetText, Expand, Collapse, ScrollIntoView };
struct ActionRequest {
    Action action = Action::Focus;
    u64 target = 0;
    f32 value = 0.f;  ///< SetValue
    std::string text{}; ///< SetText
};

/// Widget kinds the core role set folds together (menus are lists, the tree view is a list), refined from the widget
/// class so adapters can report menu / tree semantics.
enum class Kind : u8 { Plain, Menu, MenuBar, MenuItem, Tree };
/// Per-node details beyond A11ySnapshotNode (one per Bridge::nodes() entry, see Bridge::extras()).
struct NodeExtra {
    Kind kind = Kind::Plain;
    std::string text;          ///< editable text of a text field
    bool labelled = true;      ///< false when the name is only the field's text (no a11y name set)
    std::int8_t expanded = -1; ///< -1 not expandable, 0 collapsed, 1 open (menu buttons, submenu items, combo boxes)
    bool selected = false;
    bool operator==(const NodeExtra&) const = default;
};

class Bridge {
public:
    explicit Bridge(UiContext& ui) : m_ui(ui) {}
    /// Snapshot the tree; returns the number of nodes added, removed or changed since the last sync (0 = no update
    /// needed). The first sync reports every node.
    u32 sync();
    const std::vector<A11ySnapshotNode>& nodes() const { return m_cur; }
    const std::vector<NodeExtra>& extras() const { return m_curExtra; }
    u64 focusId() const;
    /// Children of snapshot entry i (indices into nodes()).
    std::vector<u32> children(u32 i) const;
    /// Apply an action on the UI thread. Click synthesises a pointer press at the node centre; Increment /
    /// Decrement step the value by its step (or 1 % of the range); SetValue clamps; SetText replaces a text field's
    /// text (TextChanged); Expand clicks a collapsed popup owner, Collapse sends Escape to its open popup;
    /// ScrollIntoView scrolls the scroll containers above the node. Returns false for dead nodes.
    bool perform(const ActionRequest& r);
    /// Thread-safe queue for adapters whose callbacks run on another thread; drain() applies them on the UI thread.
    void enqueue(const ActionRequest& r);
    u32 drain();
    /// Called (on the enqueuing thread) after every enqueue: wakes the UI thread's event pump so it drains soon.
    void setWakeHandler(void (*fn)(void* user), void* user);
    UiContext& ui() { return m_ui; }
    /// Logical px -> platform px for bounds (window DPI scale); a change makes the next sync report the root.
    void setScale(f32 s) { m_scale = s; }
    f32 scale() const { return m_scale; }
    /// Held while the snapshot is rebuilt (sync) or read from an adapter thread (activation callbacks).
    std::unique_lock<std::mutex> lockSnapshot() { return std::unique_lock<std::mutex>(m_snapMutex); }

private:
    UiContext& m_ui;
    void refine();
    std::vector<A11ySnapshotNode> m_cur, m_prev;
    std::vector<NodeExtra> m_curExtra, m_prevExtra;
    f32 m_scale = 1.f, m_syncedScale = 1.f;
    std::mutex m_mutex, m_snapMutex;
    std::vector<ActionRequest> m_queue;
    void (*m_wake)(void*) = nullptr;
    void* m_wakeUser = nullptr;
};

/// True when this build links AccessKit.
bool accessKitAvailable();

#if FUSE_UI_HAVE_ACCESSKIT
/// Build an AccessKit tree update (caller owns; pass to an adapter or accesskit_tree_update_free). Returned as void*
/// so this header does not pull in accesskit.h.
/// The root carries Bridge::scale() as its transform.
void* buildAccessKitUpdate(const Bridge& b, bool includeTree = true);
std::string accessKitDebug(const void* update); ///< AccessKit's debug dump of an update
void freeAccessKitUpdate(void* update);
/// AccessKit's Linux AT-SPI adapter driven by a Bridge: activation builds the full tree, actions are queued on the
/// bridge (drain them each frame), update() pushes a fresh tree when sync() saw changes and an AT is listening.
class UnixAdapter {
public:
    explicit UnixAdapter(Bridge& b);
    ~UnixAdapter();
    UnixAdapter(const UnixAdapter&) = delete;
    UnixAdapter& operator=(const UnixAdapter&) = delete;
    void update();                 ///< call once per frame after ui.update()
    void setWindowFocused(bool focused);
    /// X11: the window's frame (outer) and client (inner) rects in screen px; AT-SPI screen coordinates (extents,
    /// GetAccessibleAtPoint) are the node bounds offset by the inner origin.
    void setRootWindowBounds(Rect outer, Rect inner);
    bool valid() const { return m_adapter != nullptr; }

private:
    Bridge& m_bridge;
    void* m_adapter = nullptr;
};
#endif

#if FUSE_UI_HAVE_ACCESSKIT && defined(_WIN32)
/// AccessKit's Windows UI Automation adapter driven by a Bridge. Feed the window's messages to handleMessage first
/// (it answers WM_GETOBJECT and tracks WM_SETFOCUS / WM_KILLFOCUS); call update() on the window's thread once per
/// loop iteration. UIA calls actions on its own threads: they are queued on the bridge and the window is woken
/// (WM_NULL) so the UI thread drains them. Bounds are scaled from logical px to client px (client width / viewport).
class WindowsAdapter {
public:
    WindowsAdapter(Bridge& b, void* hwnd);
    ~WindowsAdapter();
    WindowsAdapter(const WindowsAdapter&) = delete;
    WindowsAdapter& operator=(const WindowsAdapter&) = delete;
    /// True when the message was consumed; `result` is then the window procedure's return value.
    bool handleMessage(u32 msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result);
    void update(); ///< drain queued actions, sync, push a tree update when something changed and UIA is listening
    void setWindowFocused(bool focused);
    bool windowFocused() const { return m_focused; }
    bool valid() const { return m_adapter != nullptr; }
    Bridge& bridge() { return m_bridge; }
    void* hwnd() const { return m_hwnd; }

private:
    Bridge& m_bridge;
    void* m_hwnd = nullptr;
    void* m_adapter = nullptr;
    bool m_focused = false;
};
#endif

} // namespace fuse::ui::a11y
