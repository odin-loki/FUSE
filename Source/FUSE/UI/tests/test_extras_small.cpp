// Small self-contained Extras modules. `--gate=<name>`:
//   audio_meter  the audio level meter widget: peak / RMS / hold ballistics as exact functions of the pushed sample
//                time, the latched clip lamp and its click reset, bar colours on screen, theme tokens
//   gesture      the stand-alone recogniser (fuse::ui::gesture): tap, double tap, long press, pan -> swipe with
//                direction, two-finger pinch with scale and rotation
#include "test_util.hpp"

#include "fuse/ui/extras/audio_meter.hpp"
#include "fuse/ui/extras/gesture.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
void step(UiContext& ui) { ui.update(1.f / 60.f); }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }

void gateAudioMeter() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "align-items", "flex-start");
    // ---- audio level meter: ballistics are exact functions of the pushed sample time
    w::AudioMeterOptions mo;
    const NodeHandle meter = w::audioLevelMeter(ui, ui.root(), 2, mo, "meter");
    ui.setCss(meter, "height", "200px");
    UI_CHECK(w::audioLevelMeterChannels(ui, meter) == 2);
    std::vector<f32> blk(2 * 4800); // 0.1 s at 48 kHz: L = 0.5 sine-ish square, R silent
    for (std::size_t i = 0; i < 4800; ++i) blk[i * 2] = (i & 1) ? 0.5f : -0.5f;
    w::audioLevelMeterPush(ui, meter, blk, 2, 48000.f);
    w::AudioLevels L = w::audioLevelMeterLevels(ui, meter, 0), R = w::audioLevelMeterLevels(ui, meter, 1);
    UI_CHECK(std::abs(L.peakDb - (-6.0206f)) < 0.01f && std::abs(L.rmsDb - (-6.0206f)) < 0.01f);
    UI_CHECK(std::abs(L.holdDb - L.peakDb) < 1e-4f && !L.clipped);
    UI_CHECK(R.peakDb <= -119.f);
    UI_CHECK(std::abs(ui.value(meter) - L.peakDb) < 1e-3f);
    // 1 s of silence: peak falls 24 dB, hold stays for 1.5 s then falls
    w::audioLevelMeterAdvance(ui, meter, 1.f);
    L = w::audioLevelMeterLevels(ui, meter, 0);
    UI_CHECK(std::abs(L.peakDb - (-30.0206f)) < 0.01f && std::abs(L.holdDb - (-6.0206f)) < 0.01f);
    w::audioLevelMeterAdvance(ui, meter, 1.f); // 0.5 s more hold, then 0.5 s * 24 = 12 dB fall
    L = w::audioLevelMeterLevels(ui, meter, 0);
    UI_CHECK(std::abs(L.holdDb - (-18.0206f)) < 0.01f);
    // a full-scale sample latches the clip lamp; a click clears it
    std::vector<f32> hot(2 * 480, 0.f);
    hot[100] = 1.f, hot[101] = 0.25f;
    w::audioLevelMeterPush(ui, meter, hot, 2, 48000.f);
    UI_CHECK(w::audioLevelMeterLevels(ui, meter, 0).clipped && !w::audioLevelMeterLevels(ui, meter, 1).clipped);
    UI_CHECK(std::abs(w::audioLevelMeterLevels(ui, meter, 1).peakDb - (-12.041f)) < 0.01f);
    UI_CHECK(std::abs(ui.value(meter)) < 1e-3f);
    w::audioLevelMeterAdvance(ui, meter, 0.25f);
    step(ui);
    Image img;
    render(ui, img);
    {   // the left bar's lamp is red, its lower body green; the right bar has its -12 dB peak region lit (yellow band)
        const Rect c = ui.contentRect(meter);
        const u32 lamp = img.at(int(c.x + 4.f), int(c.y + 3.f));
        UI_CHECK((lamp & 0xFF) > 200 && ((lamp >> 8) & 0xFF) < 120);
        const u32 low = img.at(int(c.x + 4.f), int(c.bottom() - 6.f));
        UI_CHECK(((low >> 8) & 0xFF) > 150 && (low & 0xFF) < 120);
    }
    const Rect mc = ui.contentRect(meter);
    click(ui, {mc.x + 4.f, mc.y + 40.f});
    UI_CHECK(!w::audioLevelMeterLevels(ui, meter, 0).clipped);
    w::audioLevelMeterReset(ui, meter);
    UI_CHECK(w::audioLevelMeterLevels(ui, meter, 0).peakDb <= -119.f && ui.value(meter) <= -119.f);
    w::audioLevelMeterPush(ui, meter, blk, 2, 48000.f);
    UI_CHECK(std::abs(w::audioLevelMeterLevels(ui, meter, 0).peakDb - (-6.0206f)) < 0.01f);
    // a horizontal meter without a scale measures wide and flat
    w::AudioMeterOptions ho;
    ho.vertical = false;
    ho.scale = false;
    const NodeHandle hm = w::audioLevelMeter(ui, ui.root(), 1, ho, "hmeter");
    step(ui);
    UI_CHECK(ui.rect(hm).w > ui.rect(hm).h * 4.f && w::audioLevelMeterChannels(ui, hm) == 1);
    // the bar colours are theme tokens: an override repaints the low (ok) band
    UI_CHECK(ui.addStyleSheet(":root { --meter-ok: #0000ff; }").has_value());
    step(ui);
    render(ui, img);
    {
        const Rect c = ui.contentRect(meter);
        const u32 low = img.at(int(c.x + 4.f), int(c.bottom() - 6.f));
        UI_CHECK(((low >> 16) & 0xFF) > 150 && ((low >> 8) & 0xFF) < 120);
    }
}

using namespace fuse::ui::gesture;
void gateGesture() {
    GestureRecognizer r;
    auto has = [](const std::vector<Gesture>& v, GestureType t, GestureState s) {
        for (auto& g : v) if (g.type == t && g.state == s) return true;
        return false;
    };
    // tap, then double tap
    r.feed({1, {100, 100}, TouchPhase::Down}, 0.0);
    auto o = r.feed({1, {102, 101}, TouchPhase::Up}, 0.1);
    UI_CHECK(has(o, GestureType::Tap, GestureState::Finished) && !has(o, GestureType::DoubleTap, GestureState::Finished));
    r.feed({1, {104, 100}, TouchPhase::Down}, 0.2);
    o = r.feed({1, {104, 100}, TouchPhase::Up}, 0.25);
    UI_CHECK(has(o, GestureType::DoubleTap, GestureState::Finished));
    // long press
    r.feed({1, {50, 50}, TouchPhase::Down}, 5.0);
    o = r.tick(5.6);
    UI_CHECK(has(o, GestureType::LongPress, GestureState::Started));
    o = r.feed({1, {50, 50}, TouchPhase::Up}, 5.8);
    UI_CHECK(has(o, GestureType::LongPress, GestureState::Finished) && !has(o, GestureType::Tap, GestureState::Finished));
    // pan -> swipe right
    r.feed({1, {0, 0}, TouchPhase::Down}, 10.0);
    bool started = false;
    Vec2 off;
    for (int i = 1; i <= 10; ++i) {
        o = r.feed({1, {f32(i * 20), 2}, TouchPhase::Move}, 10.0 + i * 0.01);
        started |= has(o, GestureType::Pan, GestureState::Started);
        for (auto& g : o) if (g.type == GestureType::Pan) off = g.offset;
    }
    UI_CHECK(started && std::abs(off.x - 200.f) < 0.5f);
    o = r.feed({1, {200, 2}, TouchPhase::Up}, 10.11);
    UI_CHECK(has(o, GestureType::Pan, GestureState::Finished));
    bool right = false;
    for (auto& g : o) if (g.type == GestureType::Swipe) right = g.direction == SwipeDirection::Right;
    UI_CHECK(right);
    // pinch: spread x2 and rotate 90deg
    r.feed({1, {100, 100}, TouchPhase::Down}, 20.0);
    o = r.feed({2, {200, 100}, TouchPhase::Down}, 20.01);
    UI_CHECK(has(o, GestureType::Pinch, GestureState::Started));
    r.feed({1, {150, 50}, TouchPhase::Move}, 20.05);
    o = r.feed({2, {150, 250}, TouchPhase::Move}, 20.06);
    f32 scale = 0, rot = 0;
    for (auto& g : o) if (g.type == GestureType::Pinch) { scale = g.scale; rot = g.rotation; }
    UI_CHECK(std::abs(scale - 2.f) < 0.01f);
    UI_CHECK(std::abs(rot - 1.5708f) < 0.01f);
    o = r.feed({2, {150, 250}, TouchPhase::Up}, 20.1);
    UI_CHECK(has(o, GestureType::Pinch, GestureState::Finished));
    r.feed({1, {150, 50}, TouchPhase::Up}, 20.12);
}

} // namespace

int main(int argc, char** argv) {
    std::string gate;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
    if (gate == "audio_meter") gateAudioMeter();
    else if (gate == "gesture") gateGesture();
    else {
        std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
        return 2;
    }
    return finish(("fuse_ui_extras_" + gate).c_str());
}
