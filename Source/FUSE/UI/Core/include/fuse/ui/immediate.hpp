// FUSE UI (UI2): immediate-mode front-end on the retained core (FUSE_UI_PLAN.md §4.4, decision D3 "both on one
// core"). Calls between begin() and end() are reconciled against retained nodes by a stable id (FNV-1a of the
// label, or an explicit "label##id" suffix), so layout, style, events and drawing are the core's. Interaction
// that happened since the previous frame (clicks, edits) is reported by the next frame's call.
#pragma once

#include <fuse/ui/context.hpp>

#include <string_view>
#include <vector>

namespace fuse::ui::im {

class ImmediateUi {
public:
    /// Widgets are created under `container` (a Column panel created under `parent` when invalid).
    ImmediateUi(UiContext& ui, NodeId parent = {});
    ~ImmediateUi();
    ImmediateUi(const ImmediateUi&) = delete;
    ImmediateUi& operator=(const ImmediateUi&) = delete;

    NodeId container() const { return m_container; }
    void begin();
    void end();

    void label(std::string_view text);
    bool button(std::string_view label);
    bool checkbox(std::string_view label, bool& value);
    bool slider(std::string_view label, f32& value, f32 min, f32 max);
    bool inputText(std::string_view label, std::string& value);
    void progress(std::string_view label, f32 fraction);

    /// Node for an id used this frame (tests); invalid when absent.
    NodeId nodeFor(std::string_view label) const;
    u32 widgetCount() const { return u32(m_entries.size()); }
    u32 rebuilds() const { return m_rebuilds; }

private:
    struct Entry {
        u64 key = 0;
        NodeId node;
        bool touched = false;
        u32 clicks = 0;
        bool changed = false;
    };
    Entry& touch(std::string_view label, std::string_view cls);
    Entry* entry(u64 key);

    UiContext& m_ui;
    NodeId m_container;
    bool m_ownsContainer = false;
    std::vector<Entry> m_entries;
    std::vector<u64> m_sequence;
    std::vector<u64> m_lastSequence;
    bool m_rebuild = false;
    bool m_syncing = false;
    u32 m_rebuilds = 0;
};

u64 idOf(std::string_view label);
std::string_view visibleLabel(std::string_view label); ///< strips a trailing "##id"

} // namespace fuse::ui::im
