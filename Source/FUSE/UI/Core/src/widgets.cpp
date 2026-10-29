// FUSE UI (UI2): built-in widget classes — Panel, Label, Button, CheckBox, Slider, LineEdit, ScrollView,
// Spacer, ProgressBar. Each is a WidgetClass function table (measure / paint / onEvent).
#include <fuse/ui/context.hpp>
#include <fuse/ui/font5x7.hpp>

#include <algorithm>

namespace fuse::ui {

namespace {

constexpr f32 kButtonPadX = 12.f;
constexpr f32 kButtonPadY = 6.f;
constexpr f32 kFieldPad = 4.f;

Vec2 textSize(const UiContext& ui, NodeId id) { return measureText(ui.text(id), ui.style(id).textScale); }

Vec2 measureLabel(const UiContext& ui, NodeId id, f32) { return textSize(ui, id); }
void paintLabel(const UiContext& ui, NodeId id, DrawList& dl) {
    const Rect r = ui.rect(id).inset(ui.layoutProps(id).padding);
    const Vec2 t = textSize(ui, id);
    dl.text({r.x, r.y + (r.h - t.y) * 0.5f}, ui.text(id), ui.style(id).foreground, ui.style(id).textScale);
}

Vec2 measureButton(const UiContext& ui, NodeId id, f32) {
    const Vec2 t = textSize(ui, id);
    return {t.x + 2.f * kButtonPadX, t.y + 2.f * kButtonPadY};
}
void paintButton(const UiContext& ui, NodeId id, DrawList& dl) {
    const Rect r = ui.rect(id);
    const Vec2 t = textSize(ui, id);
    dl.text({r.x + (r.w - t.x) * 0.5f, r.y + (r.h - t.y) * 0.5f}, ui.text(id), ui.style(id).foreground,
            ui.style(id).textScale);
}
bool activateKey(const Event& e) { return e.type == EventType::KeyDown && (e.key == Key::Space || e.key == Key::Enter); }
bool buttonEvent(UiContext& ui, NodeId id, const Event& e) {
    if (e.type == EventType::PointerDown) return true;
    if (e.type == EventType::PointerUp) {
        if (ui.hasState(id, kStatePressed) && ui.rect(id).contains(e.position)) ui.emitClicked(id);
        return true;
    }
    if (activateKey(e)) {
        ui.emitClicked(id);
        return true;
    }
    return false;
}

Vec2 measureCheck(const UiContext& ui, NodeId id, f32) {
    const Vec2 t = textSize(ui, id);
    const f32 box = f32((kGlyphHeight * int(ui.style(id).textScale)));
    return {box + 6.f + t.x, std::max(box, t.y)};
}
void paintCheck(const UiContext& ui, NodeId id, DrawList& dl) {
    const Rect r = ui.rect(id);
    const Style& s = ui.style(id);
    const f32 box = f32((kGlyphHeight * int(s.textScale)));
    const Rect b{r.x, r.y + (r.h - box) * 0.5f, box, box};
    dl.strokeRect(b, s.foreground, 1.f, 2.f);
    if (ui.checked(id)) dl.fillRect(b.inset(Insets::all(3.f)), s.accent, 1.f);
    const Vec2 t = textSize(ui, id);
    dl.text({b.right() + 6.f, r.y + (r.h - t.y) * 0.5f}, ui.text(id), s.foreground, s.textScale);
}
bool checkEvent(UiContext& ui, NodeId id, const Event& e) {
    if (e.type == EventType::PointerDown) return true;
    if (e.type == EventType::PointerUp) {
        if (ui.hasState(id, kStatePressed) && ui.rect(id).contains(e.position)) {
            ui.setChecked(id, !ui.checked(id));
            ui.emitClicked(id);
        }
        return true;
    }
    if (activateKey(e)) {
        ui.setChecked(id, !ui.checked(id));
        ui.emitClicked(id);
        return true;
    }
    return false;
}

Vec2 measureSlider(const UiContext&, NodeId, f32) { return {160.f, 16.f}; }
f32 fraction(const UiContext& ui, NodeId id) {
    const Vec2 r = ui.range(id);
    return r.y > r.x ? (ui.value(id) - r.x) / (r.y - r.x) : 0.f;
}
void paintSlider(const UiContext& ui, NodeId id, DrawList& dl) {
    const Rect r = ui.rect(id);
    const Style& s = ui.style(id);
    const f32 f = fraction(ui, id);
    dl.fillRect({r.x, r.y, r.w * f, r.h}, s.accent, s.radius);
    const f32 knob = std::min(r.h, 12.f);
    dl.fillRect({r.x + (r.w - knob) * f, r.y + (r.h - knob) * 0.5f, knob, knob}, s.foreground, knob * 0.5f);
}
void sliderFromPointer(UiContext& ui, NodeId id, Vec2 p) {
    const Rect r = ui.rect(id);
    const Vec2 range = ui.range(id);
    const f32 f = r.w > 0.f ? std::clamp((p.x - r.x) / r.w, 0.f, 1.f) : 0.f;
    ui.setValue(id, range.x + (range.y - range.x) * f);
}
bool sliderEvent(UiContext& ui, NodeId id, const Event& e) {
    const Vec2 range = ui.range(id);
    const f32 step = (range.y - range.x) / 20.f;
    switch (e.type) {
    case EventType::PointerDown: sliderFromPointer(ui, id, e.position); return true;
    case EventType::PointerMove:
        if (ui.hasState(id, kStatePressed)) sliderFromPointer(ui, id, e.position);
        return true;
    case EventType::PointerUp: return true;
    case EventType::KeyDown:
        if (e.key == Key::Left || e.key == Key::Down) { ui.setValue(id, ui.value(id) - step); return true; }
        if (e.key == Key::Right || e.key == Key::Up) { ui.setValue(id, ui.value(id) + step); return true; }
        if (e.key == Key::Home) { ui.setValue(id, range.x); return true; }
        if (e.key == Key::End) { ui.setValue(id, range.y); return true; }
        return false;
    default: return false;
    }
}

Vec2 measureLineEdit(const UiContext& ui, NodeId id, f32) {
    const Vec2 t = textSize(ui, id);
    return {std::max(t.x, 120.f) + 2.f * kFieldPad, t.y + 2.f * kFieldPad};
}
void paintLineEdit(const UiContext& ui, NodeId id, DrawList& dl) {
    const Rect r = ui.rect(id);
    const Style& s = ui.style(id);
    const Vec2 t = textSize(ui, id);
    const f32 ty = r.y + (r.h - t.y) * 0.5f;
    dl.pushClip(r.inset(Insets::all(2.f)));
    dl.text({r.x + kFieldPad, ty}, ui.text(id), s.foreground, s.textScale);
    if (ui.hasState(id, kStateFocus)) {
        const f32 cx = r.x + kFieldPad + f32(ui.cursor(id) * kCellWidth * s.textScale);
        dl.fillRect({cx - 1.f, ty - 1.f, 1.f, t.y + 2.f}, s.accent);
    }
    dl.popClip();
}
bool lineEditEvent(UiContext& ui, NodeId id, const Event& e) {
    std::string text = ui.text(id);
    u32 cur = ui.cursor(id);
    if (e.type == EventType::PointerDown) {
        const f32 cell = f32(kCellWidth * ui.style(id).textScale);
        const f32 x = e.position.x - ui.rect(id).x - kFieldPad;
        ui.setCursor(id, u32(std::max(0.f, x / cell + 0.5f)));
        return true;
    }
    if (e.type == EventType::PointerUp || e.type == EventType::PointerMove) return true;
    if (e.type == EventType::Text) {
        if (e.codepoint < 32 || e.codepoint > 126) return true; // UI3 brings UTF-8 / IME
        text.insert(text.begin() + cur, char(e.codepoint));
        ui.setText(id, text);
        ui.setCursor(id, cur + 1);
        ui.emitChanged(id);
        return true;
    }
    if (e.type != EventType::KeyDown) return false;
    switch (e.key) {
    case Key::Backspace:
        if (cur > 0) {
            text.erase(cur - 1, 1);
            ui.setText(id, text);
            ui.setCursor(id, cur - 1);
            ui.emitChanged(id);
        }
        return true;
    case Key::Delete:
        if (cur < text.size()) {
            text.erase(cur, 1);
            ui.setText(id, text);
            ui.emitChanged(id);
        }
        return true;
    case Key::Left: ui.setCursor(id, cur > 0 ? cur - 1 : 0); return true;
    case Key::Right: ui.setCursor(id, cur + 1); return true;
    case Key::Home: ui.setCursor(id, 0); return true;
    case Key::End: ui.setCursor(id, u32(text.size())); return true;
    case Key::Enter: ui.emitClicked(id); return true; // "activated"
    default: return false;
    }
}

bool scrollEvent(UiContext& ui, NodeId id, const Event& e) {
    if (e.type != EventType::Wheel) return false;
    ui.setScrollOffset(id, ui.scrollOffset(id) - e.wheelDelta * 40.f);
    return true;
}

Vec2 measureProgress(const UiContext&, NodeId, f32) { return {120.f, 10.f}; }
void paintProgress(const UiContext& ui, NodeId id, DrawList& dl) {
    const Rect r = ui.rect(id);
    dl.fillRect({r.x, r.y, r.w * fraction(ui, id), r.h}, ui.style(id).accent, ui.style(id).radius);
}

} // namespace

void registerBuiltinWidgets(UiContext& ui) {
    ui.registerClass({"Panel", "group"});
    ui.registerClass({"Label", "label", false, measureLabel, paintLabel});
    ui.registerClass({"Button", "button", true, measureButton, paintButton, buttonEvent});
    ui.registerClass({"CheckBox", "checkbox", true, measureCheck, paintCheck, checkEvent});
    ui.registerClass({"Slider", "slider", true, measureSlider, paintSlider, sliderEvent});
    ui.registerClass({"LineEdit", "textbox", true, measureLineEdit, paintLineEdit, lineEditEvent});
    ui.registerClass({"ScrollView", "scrollarea", false, nullptr, nullptr, scrollEvent});
    ui.registerClass({"Spacer", "none"});
    ui.registerClass({"ProgressBar", "progressbar", false, measureProgress, paintProgress});
    ui.styleSheet().parse(darkThemeSource());
    ui.invalidateStyle();
}

} // namespace fuse::ui
