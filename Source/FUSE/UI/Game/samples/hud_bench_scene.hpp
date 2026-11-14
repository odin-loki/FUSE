// A typical shooter HUD (~300 nodes) for the §6 HUD budget: fuse_ui_bench_hud (desktop gate) and the hud_bench
// Android app (fuse_ui_bench_hud_mobile) build and animate the same scene. tick() is one game frame of HUD work:
// bars, cooldowns, compass, minimap blips, ammo, timer, markers, floating text and the kill feed change as in play.
#pragma once

#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace fuse::ui::game::bench {

constexpr Size kBenchViewport{1920.f, 1080.f};

struct HudBench {
    std::unique_ptr<GameUi> game;
    NodeHandle hp, shield, stamina, hpText, compass, ammo, reserve, timer, score;
    std::array<NodeHandle, 6> dials{};
    std::array<NodeHandle, 12> blips{};
    std::array<NodeHandle, 4> mateBars{};
    std::array<NodeHandle, 5> objBars{};
    std::array<NodeHandle, 14> buffs{};
    std::array<NodeHandle, 10> slotCounts{};
    std::array<NodeHandle, 8> ring{}; ///< damage direction indicators
    std::array<u32, 8> markers{};
    u64 frame = 0;
    int ammoLeft = 30;
};

/// A transparent HUD group. Fixed-size groups (w, h > 0) are layout boundaries: a changing number inside re-lays out
/// that group only, as HUD layouts are usually authored.
inline NodeHandle hudPanel(UiContext& ui, NodeHandle parent, std::string_view id = {}, f32 w = 0.f, f32 h = 0.f) {
    const NodeHandle p = widgets::panel(ui, parent, id);
    ui.setCss(p, "background", "transparent");
    ui.setCss(p, "pointer-events", "none");
    if (w > 0.f && h > 0.f) {
        ui.set(p, Prop::Width, PropValue::ofLength(Length::px(w)));
        ui.set(p, Prop::Height, PropValue::ofLength(Length::px(h)));
    }
    return p;
}
inline void anchor(UiContext& ui, NodeHandle n, const char* side, f32 v, const char* side2, f32 v2) {
    char b[32];
    std::snprintf(b, sizeof b, "%dpx", int(v));
    ui.setCss(n, side, b);
    std::snprintf(b, sizeof b, "%dpx", int(v2));
    ui.setCss(n, side2, b);
}

inline Mat4 benchCamera(Vec3 eye, f32 yawDeg) {
    const f32 y = yawDeg * 3.14159265f / 180.f;
    const Vec3 fwd{std::sin(y), 0.f, -std::cos(y)};
    return mul(perspective(1.0f, kBenchViewport.w / kBenchViewport.h, 0.1f, 500.f),
               lookAt(eye, {eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z}, {0, 1, 0}));
}

/// Builds the HUD on `ui` (widgets::install and game::install done by the caller).
inline HudBench buildHudBench(UiContext& ui) {
    using namespace widgets;
    HudBench s;
    ui.setCss(ui.root(), "background", "#2E4A5C");
    ui.setCss(ui.root(), "layout", "anchor");
    ui.setCss(ui.root(), "padding", "0px");
    s.game = std::make_unique<GameUi>(ui);
    GameUi& g = *s.game;
    g.setSafeArea({32.f, 24.f, 32.f, 24.f});
    const NodeHandle safe = g.safeArea();
    char buf[64];

    // vitals (bottom left): three bars, numbers, buffs
    const NodeHandle vitals = hudPanel(ui, safe, "vitals", 380, 170);
    anchor(ui, vitals, "left", 0, "bottom", 0);
    ui.setCss(vitals, "gap", "4px");
    s.hpText = label(ui, vitals, "100 / 100", "hp.text");
    s.hp = hudBar(ui, vitals, 1.f, "hp");
    s.shield = hudBar(ui, vitals, 0.6f, "shield");
    ui.setCss(s.shield, "background-end", "#3D8BFD");
    s.stamina = hudBar(ui, vitals, 0.8f, "stamina");
    ui.setCss(s.stamina, "height", "6px");
    const NodeHandle buffRow = hudPanel(ui, vitals, "buffs");
    ui.setCss(buffRow, "flex-direction", "row");
    ui.setCss(buffRow, "gap", "3px");
    for (std::size_t i = 0; i < s.buffs.size(); ++i) {
        const NodeHandle b = panel(ui, buffRow);
        ui.setCss(b, "width", "26px");
        ui.setCss(b, "height", "26px");
        ui.setCss(b, "border-radius", "4px");
        ui.setCss(b, "background", i % 3 ? "#304060" : "#603040");
        std::snprintf(buf, sizeof buf, "%zu", 5 + i * 3);
        s.buffs[i] = label(ui, b, buf);
    }

    // abilities (bottom centre)
    const NodeHandle abil = hudPanel(ui, safe, "abilities", 400, 70);
    anchor(ui, abil, "left", 700, "bottom", 0);
    ui.setCss(abil, "flex-direction", "row");
    ui.setCss(abil, "gap", "10px");
    static const char* const kKeys[] = {"Q", "E", "R", "F", "1", "2"};
    for (std::size_t i = 0; i < s.dials.size(); ++i) s.dials[i] = cooldownDial(ui, abil, kKeys[i]);

    // weapon (bottom right): ammo, reserve, four weapon slots, grenades
    const NodeHandle weapon = hudPanel(ui, safe, "weapon", 260, 150);
    anchor(ui, weapon, "right", 0, "bottom", 0);
    ui.setCss(weapon, "gap", "4px");
    s.ammo = label(ui, weapon, "30", "ammo");
    s.reserve = label(ui, weapon, "/ 120", "reserve");
    label(ui, weapon, "AR-7 Carbine");
    const NodeHandle slots = hudPanel(ui, weapon);
    ui.setCss(slots, "flex-direction", "row");
    ui.setCss(slots, "gap", "4px");
    for (int i = 0; i < 4; ++i) {
        const NodeHandle sl = panel(ui, slots);
        ui.setCss(sl, "padding", "3px");
        std::snprintf(buf, sizeof buf, "%d", i + 1);
        label(ui, sl, buf);
        label(ui, sl, i == 0 ? "AR" : i == 1 ? "SMG" : i == 2 ? "Pistol" : "Knife");
    }
    label(ui, weapon, "Grenades x2");

    // compass + timer + score (top centre)
    s.compass = compass(ui, safe, "compass");
    anchor(ui, s.compass, "left", 760, "top", 0);
    const NodeHandle top = hudPanel(ui, safe, "top", 320, 30);
    anchor(ui, top, "left", 860, "top", 40);
    ui.setCss(top, "flex-direction", "row");
    ui.setCss(top, "gap", "16px");
    s.timer = label(ui, top, "12:00", "timer");
    s.score = label(ui, top, "BLUE 0 - 0 RED", "score");

    // minimap (top right): frame, grid, blips, labels
    const NodeHandle map = panel(ui, safe, "minimap");
    anchor(ui, map, "right", 0, "top", 0);
    ui.setCss(map, "width", "240px");
    ui.setCss(map, "height", "240px");
    ui.setCss(map, "layout", "absolute");
    ui.setCss(map, "border-width", "2px");
    ui.setCss(map, "border-color", "#C0C8D0");
    ui.setCss(map, "overflow", "clip");
    for (int i = 0; i < 6; ++i) { // grid lines
        const NodeHandle ln = panel(ui, map);
        ui.setCss(ln, "background", "#405060");
        anchor(ui, ln, "left", f32(i * 40), "top", 0);
        ui.setCss(ln, "width", "1px");
        ui.setCss(ln, "height", "240px");
        const NodeHandle lh = panel(ui, map);
        ui.setCss(lh, "background", "#405060");
        anchor(ui, lh, "left", 0, "top", f32(i * 40));
        ui.setCss(lh, "width", "240px");
        ui.setCss(lh, "height", "1px");
    }
    for (std::size_t i = 0; i < s.blips.size(); ++i) {
        s.blips[i] = panel(ui, map);
        ui.setCss(s.blips[i], "width", "8px");
        ui.setCss(s.blips[i], "height", "8px");
        ui.setCss(s.blips[i], "border-radius", "4px");
        ui.setCss(s.blips[i], "background", i < 4 ? "#3D8BFD" : "#FF5C5C");
    }
    label(ui, map, "N");

    // squad (left): name, bar, status per mate
    const NodeHandle squad = hudPanel(ui, safe, "squad", 280, 150);
    anchor(ui, squad, "left", 0, "top", 260);
    ui.setCss(squad, "gap", "6px");
    static const char* const kMates[] = {"Mira", "Odin", "Kade", "Juno"};
    for (std::size_t i = 0; i < s.mateBars.size(); ++i) {
        const NodeHandle m = hudPanel(ui, squad);
        ui.setCss(m, "flex-direction", "row");
        ui.setCss(m, "gap", "6px");
        label(ui, m, kMates[i]);
        s.mateBars[i] = hudBar(ui, m, 0.9f - 0.15f * f32(i));
        ui.setCss(s.mateBars[i], "width", "120px");
        label(ui, m, i == 2 ? "DOWN" : "OK");
    }

    // objectives (left, below the squad): text + progress
    const NodeHandle objs = hudPanel(ui, safe, "objectives", 260, 230);
    anchor(ui, objs, "left", 0, "top", 420);
    ui.setCss(objs, "gap", "4px");
    label(ui, objs, "OBJECTIVES");
    for (std::size_t i = 0; i < s.objBars.size(); ++i) {
        std::snprintf(buf, sizeof buf, "Secure relay %c", char('A' + i));
        label(ui, objs, buf);
        s.objBars[i] = progressBar(ui, objs, 0.2f * f32(i));
    }

    // chat (left, lower)
    const NodeHandle chat = hudPanel(ui, safe, "chat", 440, 310);
    anchor(ui, chat, "left", 0, "top", 640);
    for (int i = 0; i < 16; ++i) {
        std::snprintf(buf, sizeof buf, "[Team] Player%d: holding position %d", i, i * 7);
        label(ui, chat, buf);
    }

    // hotbar (bottom centre, above the abilities): ten slots with key, item and count
    const NodeHandle hotbar = hudPanel(ui, safe, "hotbar", 570, 56);
    anchor(ui, hotbar, "left", 640, "bottom", 80);
    ui.setCss(hotbar, "flex-direction", "row");
    ui.setCss(hotbar, "gap", "4px");
    for (std::size_t i = 0; i < s.slotCounts.size(); ++i) {
        const NodeHandle sl = panel(ui, hotbar);
        ui.setCss(sl, "width", "52px");
        ui.setCss(sl, "height", "52px");
        ui.setCss(sl, "border-width", "1px");
        ui.setCss(sl, "border-color", "#8090A0");
        std::snprintf(buf, sizeof buf, "%zu", (i + 1) % 10);
        label(ui, sl, buf);
        std::snprintf(buf, sizeof buf, "x%zu", i * 2 + 1);
        s.slotCounts[i] = label(ui, sl, buf);
    }

    // roster (top left): ten player chips with name and score
    const NodeHandle roster = hudPanel(ui, safe, "roster", 760, 40);
    anchor(ui, roster, "left", 0, "top", 0);
    ui.setCss(roster, "flex-direction", "row");
    ui.setCss(roster, "gap", "4px");
    for (int i = 0; i < 10; ++i) {
        const NodeHandle chip = panel(ui, roster);
        ui.setCss(chip, "padding", "2px");
        ui.setCss(chip, "background", i < 5 ? "#203A60" : "#602030");
        std::snprintf(buf, sizeof buf, "P%d", i + 1);
        label(ui, chip, buf);
        std::snprintf(buf, sizeof buf, "%d", i * 3);
        label(ui, chip, buf);
    }

    // crosshair (centre), hit marker and damage direction ring
    const NodeHandle cross = panel(ui, safe, "crosshair");
    anchor(ui, cross, "left", 908, "top", 500);
    ui.setCss(cross, "width", "40px");
    ui.setCss(cross, "height", "40px");
    ui.setCss(cross, "layout", "absolute");
    ui.setCss(cross, "background", "transparent");
    ui.setCss(cross, "pointer-events", "none");
    for (int i = 0; i < 4; ++i) {
        const NodeHandle tick = panel(ui, cross);
        ui.setCss(tick, "background", "#F0F0F0");
        anchor(ui, tick, "left", i < 2 ? f32(i * 26) : 19.f, "top", i < 2 ? 19.f : f32((i - 2) * 26));
        ui.setCss(tick, "width", i < 2 ? "14px" : "2px");
        ui.setCss(tick, "height", i < 2 ? "2px" : "14px");
    }
    for (std::size_t i = 0; i < s.ring.size(); ++i) {
        s.ring[i] = panel(ui, safe);
        const f32 a = f32(i) * 0.785398f;
        anchor(ui, s.ring[i], "left", 922.f + std::cos(a) * 120.f, "top", 514.f + std::sin(a) * 120.f);
        ui.setCss(s.ring[i], "width", "12px");
        ui.setCss(s.ring[i], "height", "12px");
        ui.setCss(s.ring[i], "border-radius", "6px");
        ui.setCss(s.ring[i], "background", "#FF3030");
        ui.setCss(s.ring[i], "pointer-events", "none");
    }

    // notifications (right, middle): five toasts with title and body
    const NodeHandle toasts = hudPanel(ui, safe, "toasts", 260, 360);
    anchor(ui, toasts, "right", 0, "top", 300);
    ui.setCss(toasts, "gap", "4px");
    for (int i = 0; i < 8; ++i) {
        const NodeHandle t = panel(ui, toasts);
        ui.setCss(t, "padding", "4px");
        label(ui, t, i % 2 ? "Achievement" : "Squad");
        label(ui, t, i % 2 ? "Ten headshots in a row" : "Kade revived Juno");
    }

    // prompts (right, above the weapon)
    const NodeHandle prompts = hudPanel(ui, safe, "prompts", 260, 120);
    anchor(ui, prompts, "right", 0, "bottom", 140);
    ui.setCss(prompts, "gap", "6px");
    g.prompt(prompts, Action::Interact, "Open crate");
    g.prompt(prompts, Action::Back, "Cancel");
    g.prompt(prompts, Action::Menu, "Map");

    const Vec3 eye{0.f, 1.7f, 0.f};
    g.setCamera(benchCamera(eye, 0.f), eye);
    for (std::size_t i = 0; i < s.markers.size(); ++i) {
        const f32 a = f32(i) * 0.8f;
        std::snprintf(buf, sizeof buf, "Target %zu", i);
        s.markers[i] = g.addMarker({std::sin(a) * 40.f, 1.5f, -std::cos(a) * 40.f}, i % 2 ? buf : "");
    }
    for (int i = 0; i < 5; ++i) g.feed("Odin  >  Raider", 30.f);
    g.subtitle("Mira", "The relay is just past the ridge.", 1000.f);
    ui.update(0.f);
    return s;
}

/// One frame of HUD work at 60 Hz: values tick, the camera turns, combat text and feed lines arrive now and then.
inline void tickHudBench(UiContext& ui, HudBench& s) {
    constexpr f32 dt = 1.f / 60.f;
    GameUi& g = *s.game;
    const u64 f = ++s.frame;
    const f32 t = f32(f) * dt;
    char buf[48];
    const f32 hp = 0.55f + 0.45f * std::cos(t * 0.7f);
    setHudBar(ui, s.hp, hp);
    setHudBar(ui, s.shield, 0.5f + 0.5f * std::sin(t * 0.3f));
    setHudBar(ui, s.stamina, 0.5f + 0.5f * std::sin(t * 1.7f));
    if (f % 6 == 0) {
        std::snprintf(buf, sizeof buf, "%d / 100", int(hp * 100.f));
        ui.setText(s.hpText, buf);
    }
    for (std::size_t i = 0; i < s.dials.size(); ++i) setCooldown(ui, s.dials[i], std::fmod(t * 0.2f + f32(i) * 0.17f, 1.f));
    const f32 yaw = std::fmod(t * 20.f, 360.f);
    setCompass(ui, s.compass, yaw, 65.f);
    for (std::size_t i = 0; i < s.blips.size(); ++i) {
        const f32 a = t * (0.3f + 0.05f * f32(i)) + f32(i);
        ui.set(s.blips[i], Prop::Left, PropValue::ofLength(Length::px(std::round(116.f + std::cos(a) * (30.f + 6.f * f32(i))))));
        ui.set(s.blips[i], Prop::Top, PropValue::ofLength(Length::px(std::round(116.f + std::sin(a) * (30.f + 6.f * f32(i))))));
    }
    if (f % 8 == 0) {
        s.ammoLeft = s.ammoLeft > 0 ? s.ammoLeft - 1 : 30;
        std::snprintf(buf, sizeof buf, "%d", s.ammoLeft);
        ui.setText(s.ammo, buf);
    }
    if (f % 6 == 0) {
        const int left = 720 - int(t);
        std::snprintf(buf, sizeof buf, "%02d:%02d.%d", left / 60, left % 60, 9 - int(std::fmod(t * 10.f, 10.f)));
        ui.setText(s.timer, buf);
    }
    for (std::size_t i = 0; i < s.mateBars.size(); ++i) setHudBar(ui, s.mateBars[i], 0.5f + 0.4f * std::sin(t + f32(i)));
    if (f % 30 == 0)
        for (std::size_t i = 0; i < s.objBars.size(); ++i) ui.setValue(s.objBars[i], std::fmod(t * 0.05f + 0.2f * f32(i), 1.f));
    if (f % 60 == 0)
        for (std::size_t i = 0; i < s.buffs.size(); ++i) {
            std::snprintf(buf, sizeof buf, "%d", int(std::fmod(40.f - t + f32(i) * 3.f, 40.f)));
            ui.setText(s.buffs[i], buf);
        }
    for (std::size_t i = 0; i < s.ring.size(); ++i) { // damage direction: one indicator flares, the others fade
        const f32 o = std::max(0.f, 1.f - std::fmod(t * 0.9f + f32(i) * 0.37f, 1.f) * 1.5f);
        ui.set(s.ring[i], Prop::Opacity, PropValue::ofNumber(std::round(o * 16.f) / 16.f));
    }
    if (f % 45 == 0) {
        const std::size_t k = std::size_t(f / 45) % s.slotCounts.size();
        std::snprintf(buf, sizeof buf, "x%d", int(f / 45) % 9);
        ui.setText(s.slotCounts[k], buf);
    }
    if (f % 20 == 0) {
        std::snprintf(buf, sizeof buf, "-%d", int(f % 97) + 3);
        g.floatText({900.f + f32(f % 200), 500.f}, buf, Color::rgb(0xFF5C5C));
    }
    if (f % 120 == 0) {
        g.feed("Kade  >  Drone", 4.f);
        std::snprintf(buf, sizeof buf, "BLUE %d - %d RED", int(f / 120), int(f / 300));
        ui.setText(s.score, buf);
    }
    const Vec3 eye{0.f, 1.7f, 0.f};
    g.setCamera(benchCamera(eye, yaw), eye);
    g.update(dt);
    ui.update(dt);
}

struct HudBenchResult {
    double medianMs = 0.0, p95Ms = 0.0, rasterMs = 0.0;
    u32 nodes = 0, drawCmds = 0;
};

/// Builds the HUD, warms up, then times `rounds` x `frames` ticks (tickHudBench: HUD state + UiContext::update = style,
/// layout and paint into the draw list). Result: the round with the lowest median (other processes only add time).
/// The CPU raster of the last frame is timed separately (reported, not gated).
template <class Rasterize>
HudBenchResult runHudBench(UiContext& ui, int frames, int rounds, Rasterize&& rasterize) {
    HudBench s = buildHudBench(ui);
    for (int i = 0; i < 120; ++i) tickHudBench(ui, s);
    HudBenchResult r;
    r.medianMs = 1e9;
    std::vector<double> t(std::size_t(frames), 0.0);
    for (int round = 0; round < rounds; ++round) {
        for (int i = 0; i < frames; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            tickHudBench(ui, s);
            t[std::size_t(i)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        std::sort(t.begin(), t.end());
        if (t[t.size() / 2] < r.medianMs) {
            r.medianMs = t[t.size() / 2];
            r.p95Ms = t[t.size() * 95 / 100];
        }
    }
    r.nodes = ui.nodeCount();
    r.drawCmds = u32(ui.drawList().size());
    const auto r0 = std::chrono::steady_clock::now();
    rasterize();
    r.rasterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
    return r;
}

} // namespace fuse::ui::game::bench
