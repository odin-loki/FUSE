// FUSE UI (UI2): UiTestDriver — scripted input by widget name for tests (FUSE_UI_PLAN.md §4.16).
#pragma once

#include <fuse/ui/context.hpp>

#include <string_view>

namespace fuse::ui {

class UiTestDriver {
public:
    explicit UiTestDriver(UiContext& ui) : m_ui(ui) {}

    UpdateStats frame(f32 dt = 1.f / 60.f) { return m_ui.update(dt); }
    NodeId node(std::string_view name) const { return m_ui.find(name); }
    Vec2 centre(std::string_view name) const;
    bool move(Vec2 p);
    bool click(Vec2 p);
    bool click(std::string_view name) { return click(centre(name)); }
    bool drag(Vec2 from, Vec2 to);
    bool wheel(std::string_view name, f32 delta);
    bool key(Key k, u8 modifiers = 0);
    bool type(std::string_view text); ///< Text events to the focused widget
    bool tab(bool backwards = false) { return key(Key::Tab, backwards ? u8(kModShift) : u8(0)); }

private:
    UiContext& m_ui;
};

} // namespace fuse::ui
