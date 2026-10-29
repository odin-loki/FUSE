// FUSE UI (UI2): UiTestDriver. See test_driver.hpp.
#include <fuse/ui/test_driver.hpp>

namespace fuse::ui {

Vec2 UiTestDriver::centre(std::string_view name) const {
    const Rect r = m_ui.rect(m_ui.find(name));
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

bool UiTestDriver::move(Vec2 p) {
    Event e{EventType::PointerMove, p};
    return m_ui.dispatch(e);
}

bool UiTestDriver::click(Vec2 p) {
    move(p);
    const bool a = m_ui.dispatch({EventType::PointerDown, p});
    const bool b = m_ui.dispatch({EventType::PointerUp, p});
    return a || b;
}

bool UiTestDriver::drag(Vec2 from, Vec2 to) {
    move(from);
    bool consumed = m_ui.dispatch({EventType::PointerDown, from});
    for (int i = 1; i <= 4; ++i) {
        const f32 t = f32(i) / 4.f;
        consumed |= move({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
    }
    consumed |= m_ui.dispatch({EventType::PointerUp, to});
    return consumed;
}

bool UiTestDriver::wheel(std::string_view name, f32 delta) {
    Event e{EventType::Wheel, centre(name)};
    e.wheelDelta = delta;
    return m_ui.dispatch(e);
}

bool UiTestDriver::key(Key k, u8 modifiers) {
    Event e{EventType::KeyDown};
    e.key = k;
    e.modifiers = modifiers;
    return m_ui.dispatch(e);
}

bool UiTestDriver::type(std::string_view text) {
    bool consumed = false;
    for (char c : text) {
        Event e{EventType::Text};
        e.codepoint = u8(c);
        consumed |= m_ui.dispatch(e);
    }
    return consumed;
}

} // namespace fuse::ui
