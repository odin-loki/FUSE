// Audio level meter widget. See audio_meter.hpp.
#include "fuse/ui/extras/audio_meter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace fuse::ui::widgets {
namespace {
struct ChanState {
    AudioLevels l;
    f32 holdLeft = 0.f;
};
struct MeterState {
    AudioMeterOptions opt;
    std::vector<ChanState> ch;
};
struct MeterSlot {
    NodeHandle node;
    std::unique_ptr<MeterState> s;
};
std::vector<MeterSlot>& meterSlots() { static std::vector<MeterSlot> v; return v; }
MeterState* meterOf(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return nullptr;
    for (auto& s : meterSlots()) if (s.node == n) return s.s.get();
    return nullptr;
}
f32 toDb(f32 a) { return a > 1e-6f ? 20.f * std::log10(a) : -120.f; }

void decay(MeterState& m, f32 dt) {
    const f32 fall = m.opt.releaseDbPerSec * dt;
    for (ChanState& c : m.ch) {
        c.l.peakDb = std::max(-120.f, c.l.peakDb - fall);
        c.l.rmsDb = std::max(-120.f, c.l.rmsDb - fall);
        if (c.holdLeft > 0.f) {
            const f32 used = std::min(c.holdLeft, dt);
            c.holdLeft -= used;
            if (dt > used) c.l.holdDb = std::max(-120.f, c.l.holdDb - m.opt.releaseDbPerSec * (dt - used));
        } else {
            c.l.holdDb = std::max(-120.f, c.l.holdDb - fall);
        }
    }
}
void sync(UiContext& ui, NodeHandle n, MeterState& m) {
    f32 top = -120.f;
    for (const ChanState& c : m.ch) top = std::max(top, c.l.peakDb);
    ui.setValue(n, top, true);
    ui.invalidate(n);
}

struct MeterW {
    static constexpr std::string_view kName = "audiolevelmeter";
    static constexpr A11yRole kRole = A11yRole::ProgressBar;
    static constexpr bool kFocusable = false;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const MeterState* m = meterOf(ui, n);
        const f32 c = m ? f32(m->ch.size()) : 2.f;
        const f32 thick = c * 10.f + (c - 1.f) * 3.f + ((m && m->opt.scale) ? 26.f : 0.f);
        return m && !m->opt.vertical ? Size{220.f, thick} : Size{thick, 180.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        MeterState* m = meterOf(pc.ui, pc.node);
        if (!m || m->ch.empty()) return;
        const AudioMeterOptions& o = m->opt;
        const Rect c = pc.content;
        const bool v = o.vertical;
        const f32 fs = pc.ui.computed(pc.node).fontSize * 0.7f;
        const f32 scaleW = o.scale ? 26.f : 0.f;
        const f32 n = f32(m->ch.size());
        const f32 lamp = 6.f;                          // clip lamp at the loud end
        const f32 len = (v ? c.h : c.w) - lamp - 2.f;  // meter length in px
        const f32 across = (v ? c.w : c.h) - scaleW;
        const f32 gap = 3.f, bw = std::max(2.f, (across - gap * (n - 1.f)) / n);
        auto frac = [&](f32 db) { return std::clamp((db - o.floorDb) / -o.floorDb, 0.f, 1.f); };
        // a segment from fraction a to b along the meter for bar i
        auto seg = [&](u32 i, f32 a, f32 b) -> Rect {
            const f32 x0 = f32(i) * (bw + gap);
            if (v) return {c.x + x0, c.y + lamp + 2.f + len * (1.f - b), bw, len * (b - a)};
            return {c.x + len * a, c.y + x0, len * (b - a), bw};
        };
        // theme tokens; the fallbacks are the classic dark meter (a context without the tokens)
        const auto tok = [&](std::string_view name, u32 rgb) { return pc.tint(pc.ui.themeColor(hashId(name), Color::rgb(rgb))); };
        const Color bg = tok("--meter-bg", 0x15171C), green = tok("--meter-ok", 0x3BD16F), yellow = tok("--meter-warn", 0xF2C94C),
                    red = tok("--meter-danger", 0xEB5757);
        const f32 fw = frac(o.warnDb), fdg = frac(o.dangerDb);
        for (u32 i = 0; i < m->ch.size(); ++i) {
            const AudioLevels& l = m->ch[i].l;
            dl.rect(seg(i, 0.f, 1.f), bg, 2.f);
            // dim zone colours behind, peak bright, RMS as a narrower inner bar
            const f32 pk = frac(l.peakDb), rm = frac(l.rmsDb);
            auto zones = [&](f32 upto, f32 alpha, f32 inset) {
                const f32 bands[3][2] = {{0.f, fw}, {fw, fdg}, {fdg, 1.f}};
                const Color cols[3] = {green, yellow, red};
                for (int z = 0; z < 3; ++z) {
                    const f32 a = bands[z][0], b = std::min(bands[z][1], upto);
                    if (b <= a) continue;
                    Rect r = seg(i, a, b);
                    if (v) r.x += inset, r.w -= 2.f * inset;
                    else r.y += inset, r.h -= 2.f * inset;
                    Color col = cols[z];
                    col.a *= alpha;
                    dl.rect(r, col);
                }
            };
            zones(pk, 0.45f, 0.f);
            zones(rm, 1.f, std::min(2.f, bw * 0.2f));
            if (l.holdDb > o.floorDb) {
                const f32 h = frac(l.holdDb);
                const Color hc = h >= fdg ? red : h >= fw ? yellow : tok("--meter-hold", 0xE8EAEE);
                Rect r = seg(i, h, h);
                if (v) r.y -= 1.f, r.h = 2.f;
                else r.x -= 1.f, r.w = 2.f;
                dl.rect(r, hc);
            }
            const Rect lr = v ? Rect{seg(i, 0, 1).x, c.y, bw, lamp} : Rect{c.x + len + 2.f, seg(i, 0, 1).y, lamp, bw};
            dl.rect(lr, l.clipped ? red : tok("--meter-lamp-off", 0x3A1E22), 1.f);
        }
        if (o.scale) {
            const Color tc = pc.tint(pc.style.color);
            for (f32 db = 0.f; db >= o.floorDb - 0.01f; db -= (db > -12.f ? 3.f : 12.f)) {
                if (db < 0.f && db > -12.f && std::fmod(-db, 6.f) != 0.f) continue; // 0, -6, -12, -24, ...
                const f32 t = frac(db);
                char s[8];
                std::snprintf(s, sizeof s, "%d", int(db));
                if (v) {
                    const f32 y = c.y + lamp + 2.f + len * (1.f - t);
                    const f32 x = c.x + across + 2.f;
                    dl.line({x, y}, {x + 4.f, y}, tc, 1.f);
                    pc.ui.paintString(dl, s, {x + 5.f, y - fs * 0.7f, scaleW - 5.f, fs * 1.4f}, tc, fs, TextAlignment::Start, pc.node.index);
                } else {
                    const f32 x = c.x + len * t, y = c.y + across + 2.f;
                    dl.line({x, y}, {x, y + 4.f}, tc, 1.f);
                    pc.ui.paintString(dl, s, {x - 12.f, y + 5.f, 24.f, fs * 1.4f}, tc, fs, TextAlignment::Center, pc.node.index);
                }
            }
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target || e.type != EventType::Click) return false;
        MeterState* m = meterOf(ec.ui, ec.node);
        if (!m) return false;
        for (ChanState& c : m->ch) c.l.clipped = false; // click clears the clip lamps
        ec.ui.invalidate(ec.node);
        return true;
    }
};
} // namespace

NodeHandle audioLevelMeter(UiContext& ui, NodeHandle parent, u32 channels, const AudioMeterOptions& opt, std::string_view id) {
    WidgetRegistry& r = ui.registry();
    if (r.find("audiolevelmeter") == 0xFFFFFFFFu) r.registerWidget<MeterW>();
    auto h = ui.create("audiolevelmeter", parent, id);
    if (!h) return {};
    auto& slots = meterSlots();
    std::erase_if(slots, [&](const MeterSlot& s) { return !ui.alive(s.node); });
    auto st = std::make_unique<MeterState>();
    st->opt = opt;
    st->ch.resize(std::max(1u, channels));
    slots.push_back({*h, std::move(st)});
    ui.setRange(*h, -120.f, 0.f);
    ui.setValue(*h, -120.f, false);
    ui.setA11y(*h, A11yRole::ProgressBar, "audio level");
    return *h;
}
void audioLevelMeterPush(UiContext& ui, NodeHandle w, std::span<const f32> s, u32 channels, f32 sampleRate) {
    MeterState* m = meterOf(ui, w);
    if (!m || channels == 0 || sampleRate <= 0.f) return;
    const std::size_t frames = s.size() / channels;
    if (frames == 0) return;
    decay(*m, f32(frames) / sampleRate);
    for (u32 c = 0; c < std::min<u32>(channels, u32(m->ch.size())); ++c) {
        f32 pk = 0.f;
        double sum = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            const f32 x = std::fabs(s[i * channels + c]);
            pk = std::max(pk, x), sum += double(x) * x;
        }
        ChanState& ch = m->ch[c];
        const f32 pdb = toDb(pk), rdb = toDb(f32(std::sqrt(sum / double(frames))));
        ch.l.peakDb = std::max(ch.l.peakDb, pdb);
        ch.l.rmsDb = std::max(ch.l.rmsDb, rdb);
        if (pdb >= ch.l.holdDb) ch.l.holdDb = pdb, ch.holdLeft = m->opt.holdSec;
        if (pk >= 1.f) ch.l.clipped = true;
    }
    sync(ui, w, *m);
}
void audioLevelMeterAdvance(UiContext& ui, NodeHandle w, f32 seconds) {
    if (MeterState* m = meterOf(ui, w); m && seconds > 0.f) decay(*m, seconds), sync(ui, w, *m);
}
void audioLevelMeterReset(UiContext& ui, NodeHandle w) {
    if (MeterState* m = meterOf(ui, w)) {
        for (ChanState& c : m->ch) c = {};
        sync(ui, w, *m);
    }
}
AudioLevels audioLevelMeterLevels(const UiContext& ui, NodeHandle w, u32 ch) {
    const MeterState* m = meterOf(ui, w);
    return m && ch < m->ch.size() ? m->ch[ch].l : AudioLevels{};
}
u32 audioLevelMeterChannels(const UiContext& ui, NodeHandle w) {
    const MeterState* m = meterOf(ui, w);
    return m ? u32(m->ch.size()) : 0u;
}
} // namespace fuse::ui::widgets
