// fuse_ui_script: `ui::` natives for FORGE script. Values cross forge::script::engine::define as JSON (forge::json);
// script functions arrive as {"$callback": id} and are invoked later with engine::call.
#include "fuse/ui/script/forge_ui.hpp"

#include "fuse/ui/style.hpp"

#include <forge/json/json.hpp>

#if FUSE_UI_SCRIPT_CHARTS
#include "fuse/ui/extras/charts.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace fuse::ui::script {
namespace json = forge::json;

namespace {

using Args = json::array;
[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

std::string out(const json::value& v) { return json::dump(v); }
std::string none() { return "null"; }
json::value node(NodeHandle h) { return h ? json::value(double(ForgeUi::pack(h))) : json::value(nullptr); }

const json::value& arg(const Args& a, std::size_t i) {
    static const json::value null;
    return i < a.size() ? a[i] : null;
}
bool has(const Args& a, std::size_t i) { return i < a.size() && !a[i].is_null(); }
std::string str(const Args& a, std::size_t i, const char* fn) {
    const auto& v = arg(a, i);
    if (v.is_str()) return v.str();
    if (v.is_num()) {
        const double d = v.num();
        char buf[64];
        std::snprintf(buf, sizeof buf, d == std::floor(d) && std::fabs(d) < 1e15 ? "%.0f" : "%g", d);
        return buf;
    }
    fail(std::string(fn) + ": argument " + std::to_string(i + 1) + " must be a string");
}
double num(const Args& a, std::size_t i, const char* fn) {
    const auto& v = arg(a, i);
    if (v.is_num()) return v.num();
    if (v.is_bool()) return v.b() ? 1.0 : 0.0;
    fail(std::string(fn) + ": argument " + std::to_string(i + 1) + " must be a number");
}
bool truthy(const json::value& v) {
    if (v.is_null()) return false;
    if (v.is_bool()) return v.b();
    if (v.is_num()) return v.num() != 0.0;
    return true;
}
std::uint64_t callback(const Args& a, std::size_t i, const char* fn) {
    const auto& v = arg(a, i);
    if (v.is_obj())
        if (const auto* id = v.find("$callback"); id && id->is_num()) return std::uint64_t(id->num());
    fail(std::string(fn) + ": argument " + std::to_string(i + 1) + " must be a function");
}
json::value signalArgs(const SignalArgs& s) {
    json::object o;
    o.emplace_back("value", double(s.value));
    o.emplace_back("flag", s.flag);
    o.emplace_back("text", std::string(s.text));
    return o;
}
AnimationHandle unpackAnim(const json::value& v) {
    if (!v.is_num()) return {};
    const auto u = u64(v.num());
    return {u32(u & 0xFFFFFFFFu), u32(u >> 32)};
}
json::value anim(AnimationHandle a) {
    return a ? json::value(double((u64(a.generation) << 32) | a.index)) : json::value(nullptr);
}

struct SignalName {
    const char* name;
    SignalKind kind;
};
constexpr SignalName kSignals[] = {
    {"clicked", SignalKind::Clicked},
    {"toggled", SignalKind::Toggled},
    {"valueChanged", SignalKind::ValueChanged},
    {"textChanged", SignalKind::TextChanged},
    {"submitted", SignalKind::Submitted},
    {"focusChanged", SignalKind::FocusChanged},
    {"hoverChanged", SignalKind::HoverChanged},
    {"editingFinished", SignalKind::EditingFinished},
    {"highlighted", SignalKind::Highlighted},
    {"activated", SignalKind::Activated},
    {"transitionRun", SignalKind::TransitionRun},
    {"transitionStart", SignalKind::TransitionStart},
    {"transitionEnd", SignalKind::TransitionEnd},
    {"transitionCancel", SignalKind::TransitionCancel},
    {"animationStart", SignalKind::AnimationStart},
    {"animationIteration", SignalKind::AnimationIteration},
    {"animationEnd", SignalKind::AnimationEnd},
    {"animationCancel", SignalKind::AnimationCancel},
    {"finish", SignalKind::ScriptFinish},
    {"cancel", SignalKind::ScriptCancel},
    {"remove", SignalKind::ScriptRemove},
    {"selectionChanged", SignalKind::SelectionChanged},
};

std::string_view trimView(std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    return t;
}
bool parseLen(std::string_view w, Length& out) {
    const bool pct = w.ends_with('%');
    if (pct) w.remove_suffix(1);
    else if (w.ends_with("px")) w.remove_suffix(2);
    if (w.empty()) return false;
    char* end = nullptr;
    const std::string t(w);
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return false;
    out = pct ? Length::pct(f32(v)) : Length::px(f32(v));
    return true;
}
/// "normal" | "<name> [<length-percentage>]" | "<length-percentage>" (animation-range-start / -end).
AnimRange parseRange(std::string_view t, bool isEnd, const char* fn) {
    AnimRange r;
    static constexpr std::pair<const char*, TimelineRange> names[] = {
        {"normal", TimelineRange::Normal}, {"cover", TimelineRange::Cover}, {"contain", TimelineRange::Contain},
        {"entry", TimelineRange::Entry}, {"exit", TimelineRange::Exit}, {"entry-crossing", TimelineRange::EntryCrossing},
        {"exit-crossing", TimelineRange::ExitCrossing}};
    t = trimView(t);
    const std::size_t sp = t.find(' ');
    const std::string_view w = t.substr(0, sp);
    bool named = false;
    for (auto& [n, k] : names)
        if (w == n) r.name = k, named = true;
    std::string_view rest = named ? (sp == std::string_view::npos ? std::string_view{} : trimView(t.substr(sp + 1))) : t;
    if (!rest.empty()) {
        if (!parseLen(rest, r.offset)) fail(std::string(fn) + ": bad range '" + std::string(t) + "'");
    } else if (named && r.name != TimelineRange::Normal) r.offset = Length::pct(isEnd ? 100.f : 0.f);
    return r;
}
} // namespace

namespace {


} // namespace

struct ForgeUi::Impl {
    ForgeUi& s;
    std::unordered_map<std::string, std::uint64_t> actions;          // .fuseui action name -> callback
    std::unordered_map<std::uint64_t, ConnectionHandle> connections; // callback -> connection
    std::vector<std::uint64_t> owned;                                // every callback id we hold

    explicit Impl(ForgeUi& o) : s(o) {}
    UiContext& ui() { return s.m_ui; }

    NodeHandle nodeArg(const Args& a, std::size_t i) {
        const auto& v = arg(a, i);
        if (v.is_str()) return ui().find(v.str());
        if (!v.is_num()) return {};
        const NodeHandle h = unpack(std::int64_t(v.num()));
        return ui().alive(h) ? h : NodeHandle{};
    }
    NodeHandle needNode(const Args& a, std::size_t i, const char* fn) {
        const NodeHandle h = nodeArg(a, i);
        if (!h) fail(std::string(fn) + ": no such node");
        return h;
    }
    void invoke(std::uint64_t cb, const json::array& args, const char* what) {
        auto r = s.m_engine.call(cb, json::dump(json::value(args)));
        if (!r.ok) std::fprintf(stderr, "fuse_ui_script: %s: %s\n", what, r.error.c_str());
    }
    void hold(std::uint64_t cb) { owned.push_back(cb); }
    void drop(std::uint64_t cb) {
        s.m_engine.release(cb);
        std::erase(owned, cb);
    }
    void action(std::string_view name, NodeHandle h, const SignalArgs& a) {
        auto it = actions.find(std::string(name));
        if (it == actions.end()) return;
        const std::string what = "action '" + std::string(name) + "'";
        invoke(it->second, {node(h), signalArgs(a)}, what.c_str());
    }
    std::string load(std::string_view text, const Args& a, std::size_t parentIdx) {
        const NodeHandle parent = has(a, parentIdx) ? nodeArg(a, parentIdx) : ui().root();
        if (!parent) fail("ui::load: bad parent");
        auto d = doc::Document::load(ui(), text, &s.m_model, parent);
        if (!d) {
            const auto& e = d.error();
            fail("fuseui: " + e.path + (e.path.empty() ? "" : ": ") + e.message + " (line " + std::to_string(e.line) +
                 " col " + std::to_string(e.column) + ")");
        }
        Impl* self = this;
        (*d)->onAction([self](std::string_view n, NodeHandle h, const SignalArgs& args) { self->action(n, h, args); });
        const NodeHandle root = (*d)->root();
        s.m_docs.push_back(std::move(*d));
        return out(node(root));
    }
    /// "document" | "scroll([#id] [nearest|root|self] [block|inline|x|y])" | "view([#id] [axis] [inset-start [inset-end]])"
    bool parseTimeline(std::string_view t, AnimTimeline& out) {
        t = trimView(t);
        out = {};
        if (t == "document" || t.empty()) return true;
        const bool scroll = t.starts_with("scroll("), view = t.starts_with("view(");
        if ((!scroll && !view) || t.back() != ')') return false;
        out.kind = scroll ? AnimTimeline::Kind::Scroll : AnimTimeline::Kind::View;
        std::string_view args = t.substr(scroll ? 7 : 5);
        args.remove_suffix(1);
        int insets = 0;
        while (!(args = trimView(args)).empty()) {
            const std::size_t sp = args.find(' ');
            const std::string_view w = args.substr(0, sp);
            args = sp == std::string_view::npos ? std::string_view{} : args.substr(sp + 1);
            if (w == "block" || w == "y") out.axis = 1;
            else if (w == "inline" || w == "x") out.axis = 0;
            else if (w == "nearest" || w == "self") {}
            else if (w == "root") out.source = ui().root();
            else if (w.starts_with('#')) {
                if (!(out.source = ui().find(w.substr(1)))) return false;
            } else if (view && insets < 2) {
                Length l;
                if (w == "auto") l = Length::px(0.f);
                else if (!parseLen(w, l)) return false;
                const bool pct = l.unit == Unit::Percent;
                f32& px = insets ? out.insetEnd : out.insetStart;
                f32& pc = insets ? out.insetEndPct : out.insetStartPct;
                (pct ? pc : px) = l.value;
                if (insets++ == 0) (pct ? out.insetEndPct : out.insetEnd) = l.value; // one inset: both ends
            } else return false;
        }
        return true;
    }
    AnimationOptions animOptions(const json::value& in) {
        AnimationOptions opt;
        // `{{"duration", 0.3}, ...}` arrives as a list of [key, value] pairs; std::map literals arrive as objects.
        json::value o = in;
        if (in.is_arr()) {
            json::object m;
            for (const auto& p : in.arr())
                if (p.is_arr() && p.arr().size() == 2 && p.arr()[0].is_str()) m.emplace_back(p.arr()[0].str(), p.arr()[1]);
                else fail("ui::animate: options must be {{\"key\", value}, ...} pairs");
            o = json::value(std::move(m));
        }
        if (o.is_num()) {
            opt.duration = f32(o.num());
            return opt;
        }
        if (!o.is_obj()) return opt;
        opt.duration = f32(o.num_or("duration", 0));
        opt.delay = f32(o.num_or("delay", 0));
        opt.endDelay = f32(o.num_or("endDelay", 0));
        if (const auto* it = o.find("iterations"))
            opt.iterations = it->is_num() ? f32(it->num()) : std::numeric_limits<f32>::infinity(); // "infinite"
        opt.iterationStart = f32(o.num_or("iterationStart", 0));
        opt.playbackRate = f32(o.num_or("playbackRate", 1));
        const std::string dir = o.str_or("direction", "normal");
        opt.direction = dir == "reverse" ? AnimDirection::Reverse
                        : dir == "alternate" ? AnimDirection::Alternate
                        : dir == "alternate-reverse" ? AnimDirection::AlternateReverse
                                                     : AnimDirection::Normal;
        const std::string fill = o.str_or("fill", "none");
        opt.fill = fill == "forwards" ? AnimFill::Forwards
                   : fill == "backwards" ? AnimFill::Backwards
                   : fill == "both" ? AnimFill::Both
                                    : AnimFill::None;
        const std::string comp = o.str_or("composite", "replace");
        opt.composite = comp == "add" ? AnimComposite::Add : comp == "accumulate" ? AnimComposite::Accumulate : AnimComposite::Replace;
        if (const auto* e = o.find("easing"); e && e->is_str())
            if (!parseEasing(e->str(), opt.easing)) fail("ui::animate: bad easing '" + e->str() + "'");
        if (const auto* ap = o.find("autoplay"); ap && ap->is_bool()) opt.autoplay = ap->b();
        opt.id = o.str_or("id", "");
        if (const auto* t = o.find("timeline"); t && t->is_str())
            if (!parseTimeline(t->str(), opt.timeline)) fail("ui::animate: bad timeline '" + t->str() + "'");
        if (const auto* r = o.find("rangeStart"); r && r->is_str()) opt.rangeStart = parseRange(r->str(), false, "ui::animate");
        if (const auto* r = o.find("rangeEnd"); r && r->is_str()) opt.rangeEnd = parseRange(r->str(), true, "ui::animate");
        if (const auto* r = o.find("range"); r && r->is_str()) { // "entry 10% exit 90%" / "cover"
            const std::string v = r->str();
            std::size_t cut = std::string::npos;
            for (const char* n : {" normal", " cover", " contain", " entry", " exit"}) {
                const auto p = v.find(n, 1);
                if (p != std::string::npos && p < cut) cut = p;
            }
            opt.rangeStart = parseRange(v.substr(0, cut), false, "ui::animate");
            opt.rangeEnd = parseRange(cut == std::string::npos ? v.substr(0, v.find(' ')) : v.substr(cut + 1), true, "ui::animate");
        }
        if (const std::uint64_t rem = [&]() -> std::uint64_t {
                const auto* f = o.find("onremove");
                const auto* id = f && f->is_obj() ? f->find("$callback") : nullptr;
                return id && id->is_num() ? std::uint64_t(id->num()) : 0;
            }()) {
            hold(rem);
            Impl* self = this;
            opt.onRemove = [self, rem](UiContext&, AnimationHandle a) {
                if (std::find(self->owned.begin(), self->owned.end(), rem) == self->owned.end()) return;
                self->invoke(rem, {anim(a)}, "onremove");
            };
        }
        auto cb = [&](const char* key) -> std::uint64_t {
            const auto* f = o.find(key);
            if (!f || !f->is_obj()) return 0;
            const auto* id = f->find("$callback");
            return id && id->is_num() ? std::uint64_t(id->num()) : 0;
        };
        const std::uint64_t fin = cb("onfinish"), can = cb("oncancel");
        if (fin) hold(fin);
        if (can) hold(can);
        if (fin || can) {
            Impl* self = this;
            // Each callback runs at most once per animation (finish or cancel); both are released afterwards.
            auto done = [self, fin, can](std::uint64_t which, AnimationHandle a) {
                if (!which || std::find(self->owned.begin(), self->owned.end(), which) == self->owned.end()) return;
                self->invoke(which, {anim(a)}, which == fin ? "onfinish" : "oncancel");
                // finish may be followed by a replay; keep callbacks until the animation is gone or cancelled
                if (which == can || !self->ui().animValid(a)) {
                    if (fin) self->drop(fin);
                    if (can) self->drop(can);
                }
            };
            opt.onFinish = [done, fin](UiContext&, AnimationHandle a) { done(fin, a); };
            opt.onCancel = [done, can](UiContext&, AnimationHandle a) { done(can, a); };
        }
        return opt;
    }
    void install();
    void installCharts();
};

void ForgeUi::Impl::install() {
    auto& e = s.m_engine;
    Impl* I = this;
    auto def = [&](const char* name, int lo, int hi, auto fn) {
        e.define(std::string("ui::") + name, lo, hi, [I, fn](const std::string& js) -> std::string {
            const json::value v = json::parse(js);
            // Callbacks passed in but not kept by the binding (errors, ignored arguments) are released afterwards.
            struct Sweep {
                Impl& I;
                const json::value& v;
                void walk(const json::value& x) {
                    if (x.is_arr()) for (auto& e : x.arr()) walk(e);
                    else if (x.is_obj()) {
                        if (const auto* id = x.find("$callback"); id && id->is_num()) {
                            const auto cb = std::uint64_t(id->num());
                            if (std::find(I.owned.begin(), I.owned.end(), cb) == I.owned.end()) I.s.m_engine.release(cb);
                        } else for (auto& [k, e] : x.obj()) walk(e);
                    }
                }
                ~Sweep() { walk(v); }
            } sweep{*I, v};
            return fn(*I, v.arr());
        });
    };
    // ---- nodes
    def("find", 1, 1, [](Impl& I, const Args& a) { return out(node(I.ui().find(str(a, 0, "ui::find")))); });
    def("root", 0, 0, [](Impl& I, const Args&) { return out(node(I.ui().root())); });
    def("alive", 1, 1, [](Impl& I, const Args& a) { return out(json::value(bool(I.nodeArg(a, 0)))); });
    def("id", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return h ? out(std::string(I.ui().idOf(h))) : none();
    });
    def("text", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return h ? out(std::string(I.ui().text(h))) : none();
    });
    def("setText", 2, 2, [](Impl& I, const Args& a) {
        const std::string t = str(a, 1, "ui::setText");
        if (const NodeHandle h = I.nodeArg(a, 0)) {
            I.ui().setText(h, t);
            if (I.ui().registry().get(I.ui().classOf(h)).name == "lineedit") I.ui().editBuffer(h).setText(t);
        }
        return none();
    });
    def("value", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return h ? out(json::value(double(I.ui().value(h)))) : none();
    });
    def("setValue", 2, 3, [](Impl& I, const Args& a) {
        const double v = num(a, 1, "ui::setValue");
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().setValue(h, f32(v), truthy(arg(a, 2)));
        return none();
    });
    def("checked", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return h ? out(json::value(I.ui().checked(h))) : none();
    });
    def("setChecked", 2, 2, [](Impl& I, const Args& a) {
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().setChecked(h, truthy(arg(a, 1)));
        return none();
    });
    def("disabled", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return h ? out(json::value(I.ui().disabled(h))) : none();
    });
    def("setDisabled", 2, 2, [](Impl& I, const Args& a) {
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().setDisabled(h, truthy(arg(a, 1)));
        return none();
    });
    def("focus", 0, 1, [](Impl& I, const Args& a) {
        if (a.empty()) return out(node(I.ui().focused()));
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().setFocus(h);
        return none();
    });
    def("addClass", 2, 2, [](Impl& I, const Args& a) {
        const std::string c = str(a, 1, "ui::addClass");
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().addClass(h, c);
        return none();
    });
    def("removeClass", 2, 2, [](Impl& I, const Args& a) {
        const std::string c = str(a, 1, "ui::removeClass");
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().removeClass(h, c);
        return none();
    });
    def("hasClass", 2, 2, [](Impl& I, const Args& a) {
        const std::string c = str(a, 1, "ui::hasClass");
        const NodeHandle h = I.nodeArg(a, 0);
        return out(json::value(h && I.ui().hasClass(h, c)));
    });
    def("css", 3, 3, [](Impl& I, const Args& a) {
        const std::string p = str(a, 1, "ui::css"), v = str(a, 2, "ui::css");
        const NodeHandle h = I.nodeArg(a, 0);
        return out(json::value(h && I.ui().setCss(h, p, v)));
    });
    def("create", 1, 3, [](Impl& I, const Args& a) {
        const std::string cls = str(a, 0, "ui::create");
        const NodeHandle parent = has(a, 1) ? I.nodeArg(a, 1) : I.ui().root();
        if (!parent) fail("ui::create: bad parent");
        const std::string id = has(a, 2) ? str(a, 2, "ui::create") : std::string();
        auto n = I.ui().create(cls, parent, id);
        if (!n) fail("ui::create: unknown class '" + cls + "'");
        return out(node(*n));
    });
    def("destroy", 1, 1, [](Impl& I, const Args& a) {
        if (const NodeHandle h = I.nodeArg(a, 0)) I.ui().destroy(h);
        return none();
    });
    // ---- tree and reordering
    auto rel = [&](const char* name, NodeHandle (UiContext::*f)(NodeHandle) const) {
        def(name, 1, 1, [f](Impl& I, const Args& a) {
            const NodeHandle h = I.nodeArg(a, 0);
            return h ? out(node((I.ui().*f)(h))) : none();
        });
    };
    rel("parent", &UiContext::parent);
    rel("firstChild", &UiContext::firstChild);
    rel("lastChild", &UiContext::lastChild);
    rel("nextSibling", &UiContext::nextSibling);
    rel("prevSibling", &UiContext::prevSibling);
    def("childCount", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return h ? out(json::value(double(I.ui().childCount(h)))) : none();
    });
    def("children", 1, 1, [](Impl& I, const Args& a) {
        json::array l;
        if (const NodeHandle h = I.nodeArg(a, 0))
            for (NodeHandle c = I.ui().firstChild(h); c; c = I.ui().nextSibling(c)) l.push_back(node(c));
        return out(json::value(std::move(l)));
    });
    def("childAt", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        const double i = num(a, 1, "ui::childAt");
        return h && i >= 0 ? out(node(I.ui().childAt(h, u32(i)))) : none();
    });
    def("childIndex", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.nodeArg(a, 0);
        return out(json::value(double(h ? I.ui().childIndex(h) : -1)));
    });
    def("moveTo", 2, 3, [](Impl& I, const Args& a) {
        const NodeHandle n = I.needNode(a, 0, "ui::moveTo"), p = I.needNode(a, 1, "ui::moveTo");
        I.ui().moveTo(n, p, has(a, 2) ? I.nodeArg(a, 2) : NodeHandle{});
        return none();
    });
    def("insertBefore", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle n = I.nodeArg(a, 0), r = I.nodeArg(a, 1);
        return out(json::value(n && r && I.ui().insertBefore(n, r)));
    });
    def("insertAfter", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle n = I.nodeArg(a, 0), r = I.nodeArg(a, 1);
        return out(json::value(n && r && I.ui().insertAfter(n, r)));
    });
    def("setChildIndex", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle n = I.nodeArg(a, 0);
        const double i = num(a, 1, "ui::setChildIndex");
        return out(json::value(n && i >= 0 && I.ui().setChildIndex(n, u32(i))));
    });
    def("swap", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle x = I.nodeArg(a, 0), y = I.nodeArg(a, 1);
        return out(json::value(x && y && I.ui().swapNodes(x, y)));
    });
    def("reorder", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle p = I.needNode(a, 0, "ui::reorder");
        if (!arg(a, 1).is_arr()) fail("ui::reorder: argument 2 must be a list of nodes");
        std::vector<NodeHandle> order;
        for (std::size_t i = 0; i < arg(a, 1).arr().size(); ++i) order.push_back(I.nodeArg(arg(a, 1).arr(), i));
        return out(json::value(I.ui().reorderChildren(p, order)));
    });
    def("sortChildren", 2, 2, [](Impl& I, const Args& a) {
        const NodeHandle p = I.needNode(a, 0, "ui::sortChildren");
        const std::uint64_t cb = callback(a, 1, "ui::sortChildren");
        std::string err;
        I.ui().sortChildren(p, [&](NodeHandle x, NodeHandle y) {
            if (!err.empty()) return false;
            auto r = I.s.m_engine.call(cb, json::dump(json::value(json::array{node(x), node(y)})));
            if (!r.ok) err = r.error;
            return r.ok && r.value == "true";
        });
        I.s.m_engine.release(cb);
        if (!err.empty()) fail("ui::sortChildren: " + err);
        return none();
    });
    // ---- signals and actions
    def("on", 3, 3, [](Impl& I, const Args& a) {
        const NodeHandle h = I.needNode(a, 0, "ui::on");
        const std::string sig = str(a, 1, "ui::on");
        const SignalName* k = nullptr;
        for (const auto& s : kSignals)
            if (sig == s.name) k = &s;
        if (!k) fail("ui::on: unknown signal '" + sig + "'");
        const std::uint64_t cb = callback(a, 2, "ui::on");
        I.hold(cb);
        Impl* self = &I;
        const char* name = k->name;
        const ConnectionHandle c = I.ui().connect(h, k->kind, [self, cb, name](UiContext&, NodeHandle n, const SignalArgs& s) {
            self->invoke(cb, {node(n), signalArgs(s)}, name);
        });
        I.connections[cb] = c;
        return out(json::value(double(cb)));
    });
    def("off", 1, 1, [](Impl& I, const Args& a) {
        const auto cb = std::uint64_t(num(a, 0, "ui::off"));
        auto it = I.connections.find(cb);
        if (it == I.connections.end()) return out(json::value(false));
        I.ui().disconnect(it->second);
        I.connections.erase(it);
        I.drop(cb);
        return out(json::value(true));
    });
    def("action", 2, 2, [](Impl& I, const Args& a) {
        const std::string n = str(a, 0, "ui::action");
        if (auto it = I.actions.find(n); it != I.actions.end()) I.drop(it->second);
        if (!has(a, 1)) {
            I.actions.erase(n);
            return none();
        }
        const std::uint64_t cb = callback(a, 1, "ui::action");
        I.hold(cb);
        I.actions[n] = cb;
        return none();
    });
    // ---- binding model and documents
    def("set", 2, 2, [](Impl& I, const Args& a) {
        const std::string n = str(a, 0, "ui::set");
        const auto& v = arg(a, 1);
        if (v.is_str()) I.s.m_model.set(n, std::string_view(v.str()));
        else I.s.m_model.set(n, num(a, 1, "ui::set"));
        return none();
    });
    def("get", 1, 1, [](Impl& I, const Args& a) {
        const u32 slot = I.s.m_model.find(str(a, 0, "ui::get"));
        if (slot == 0xFFFFFFFFu) return none();
        if (I.s.m_model.isString(slot)) return out(std::string(I.s.m_model.stringAt(slot)));
        return out(json::value(I.s.m_model.numberAt(slot)));
    });
    def("load", 1, 2, [](Impl& I, const Args& a) { return I.load(str(a, 0, "ui::load"), a, 1); });
    def("loadFile", 1, 2, [](Impl& I, const Args& a) {
        const std::string path = str(a, 0, "ui::loadFile");
        std::string text;
        if (!doc::readTextFile(path.c_str(), text)) fail("ui::loadFile: cannot read '" + path + "'");
        return I.load(text, a, 1);
    });
    def("refresh", 0, 0, [](Impl& I, const Args&) {
        u32 n = 0;
        for (auto& d : I.s.m_docs) n += d->refresh();
        return out(json::value(double(n)));
    });
    // ---- scripted animations (Element.animate / Animation)
    def("animate", 2, 3, [](Impl& I, const Args& a) {
        const NodeHandle h = I.needNode(a, 0, "ui::animate");
        const std::string css = str(a, 1, "ui::animate");
        AnimationOptions opt = I.animOptions(arg(a, 2));
        const AnimationHandle an = I.ui().animate(h, css, std::move(opt));
        if (!an) fail("ui::animate: bad keyframes");
        return out(anim(an));
    });
    auto animOp = [&](const char* name, bool (UiContext::*f)(AnimationHandle)) {
        def(name, 1, 1, [f](Impl& I, const Args& a) { return out(json::value((I.ui().*f)(unpackAnim(arg(a, 0))))); });
    };
    animOp("animPlay", &UiContext::animPlay);
    animOp("animPause", &UiContext::animPause);
    animOp("animReverse", &UiContext::animReverse);
    animOp("animFinish", &UiContext::animFinish);
    animOp("animCancel", &UiContext::animCancel);
    animOp("animCommitStyles", &UiContext::animCommitStyles);
    def("animSetCurrentTime", 2, 2, [](Impl& I, const Args& a) {
        return out(json::value(I.ui().animSetCurrentTime(unpackAnim(arg(a, 0)), f32(num(a, 1, "ui::animSetCurrentTime")))));
    });
    def("animSetPlaybackRate", 2, 2, [](Impl& I, const Args& a) {
        return out(json::value(I.ui().animSetPlaybackRate(unpackAnim(arg(a, 0)), f32(num(a, 1, "ui::animSetPlaybackRate")))));
    });
    def("animCurrentTime", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        return I.ui().animValid(h) ? out(json::value(double(I.ui().animCurrentTime(h)))) : none();
    });
    def("animPlaybackRate", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        return I.ui().animValid(h) ? out(json::value(double(I.ui().animPlaybackRate(h)))) : none();
    });
    def("animProgress", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        const f32 p = I.ui().animValid(h) ? I.ui().animProgress(h) : std::numeric_limits<f32>::quiet_NaN();
        return std::isnan(p) ? none() : out(json::value(double(p)));
    });
    def("animPlayState", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        if (!I.ui().animValid(h)) return none();
        static const char* names[] = {"idle", "running", "paused", "finished"};
        return out(std::string(names[int(I.ui().animPlayState(h))]));
    });
    def("animId", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        return I.ui().animValid(h) ? out(std::string(I.ui().animId(h))) : none();
    });
    def("animTarget", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        return I.ui().animValid(h) ? out(node(I.ui().animTarget(h))) : none();
    });
    def("animations", 1, 2, [](Impl& I, const Args& a) {
        json::array l;
        if (const NodeHandle h = I.nodeArg(a, 0))
            for (AnimationHandle x : I.ui().animations(h, truthy(arg(a, 1)))) l.push_back(anim(x));
        return out(json::value(std::move(l)));
    });
    def("animSetKeyframes", 2, 2, [](Impl& I, const Args& a) {
        return out(json::value(I.ui().animSetKeyframes(unpackAnim(arg(a, 0)), str(a, 1, "ui::animSetKeyframes"))));
    });
    def("animSetTarget", 2, 2, [](Impl& I, const Args& a) {
        return out(json::value(I.ui().animSetTarget(unpackAnim(arg(a, 0)), I.nodeArg(a, 1))));
    });
    animOp("animPersist", &UiContext::animPersist);
    def("animReplaceState", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        if (!I.ui().animValid(h)) return none();
        static const char* names[] = {"active", "removed", "persisted"};
        return out(std::string(names[int(I.ui().animReplaceState(h))]));
    });
    def("animStartTime", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        const double t = I.ui().animValid(h) ? I.ui().animStartTime(h) : std::numeric_limits<double>::quiet_NaN();
        return std::isnan(t) ? none() : out(json::value(t));
    });
    def("animTimelineProgress", 1, 1, [](Impl& I, const Args& a) {
        const AnimationHandle h = unpackAnim(arg(a, 0));
        const f32 p = I.ui().animValid(h) ? I.ui().animTimelineProgress(h) : std::numeric_limits<f32>::quiet_NaN();
        return std::isnan(p) ? none() : out(json::value(double(p)));
    });
    def("documentTime", 0, 0, [](Impl& I, const Args&) { return out(json::value(I.ui().documentTime())); });
    // timelineProgress(node, "view()" [, "entry 0%", "exit 100%"]): nullptr while the timeline is inactive
    def("timelineProgress", 2, 4, [](Impl& I, const Args& a) {
        const NodeHandle h = I.needNode(a, 0, "ui::timelineProgress");
        AnimTimeline t;
        if (!I.parseTimeline(str(a, 1, "ui::timelineProgress"), t)) fail("ui::timelineProgress: bad timeline");
        f32 p;
        if (has(a, 2) || has(a, 3))
            p = I.ui().timelineProgress(h, t, has(a, 2) ? parseRange(str(a, 2, "ui::timelineProgress"), false, "ui::timelineProgress") : AnimRange{},
                                        has(a, 3) ? parseRange(str(a, 3, "ui::timelineProgress"), true, "ui::timelineProgress") : AnimRange{});
        else p = I.ui().timelineProgress(h, t);
        return std::isnan(p) ? none() : out(json::value(double(p)));
    });
    // ---- coordinates through CSS transforms (2D / 3D): context <-> a node's local (untransformed) space
    auto point = [](Vec2 p) {
        json::array r;
        r.push_back(double(p.x));
        r.push_back(double(p.y));
        return json::value(std::move(r));
    };
    auto mapFn = [&](const char* name, bool (UiContext::*f)(NodeHandle, Vec2, Vec2&) const) {
        def(name, 3, 3, [f, point, name](Impl& I, const Args& a) {
            const std::string fn = std::string("ui::") + name;
            const NodeHandle h = I.needNode(a, 0, fn.c_str());
            Vec2 o{};
            if (!(I.ui().*f)(h, Vec2{f32(num(a, 1, fn.c_str())), f32(num(a, 2, fn.c_str()))}, o)) return none();
            return out(point(o));
        });
    };
    mapFn("toLocal", &UiContext::toLocal);
    mapFn("fromLocal", &UiContext::fromLocal);
    def("screenMap", 1, 1, [](Impl& I, const Args& a) {
        json::array m;
        for (f32 v : I.ui().screenMap(I.needNode(a, 0, "ui::screenMap"))) m.push_back(double(v));
        return out(json::value(std::move(m)));
    });
    installCharts();
}

namespace {
/// `{{"k", v}, ...}` pair lists (FORGE brace literals) -> objects, recursively for nested specs; other values as-is.
json::value objOf(const json::value& in) {
    if (!in.is_arr() || in.arr().empty()) return in;
    for (const auto& p : in.arr())
        if (!p.is_arr() || p.arr().size() != 2 || !p.arr()[0].is_str()) return in;
    json::object m;
    for (const auto& p : in.arr()) m.emplace_back(p.arr()[0].str(), p.arr()[1]);
    return json::value(std::move(m));
}
#if FUSE_UI_SCRIPT_CHARTS
std::vector<f32> floats(const json::value* v, const char* fn) {
    std::vector<f32> r;
    if (!v) return r;
    if (!v->is_arr()) fail(std::string(fn) + ": expected a list of numbers");
    for (const auto& x : v->arr()) r.push_back(x.is_num() ? f32(x.num()) : NAN);
    return r;
}
std::vector<std::string> strings(const json::value* v) {
    std::vector<std::string> r;
    if (v && v->is_arr())
        for (const auto& x : v->arr()) r.push_back(x.is_str() ? x.str() : json::dump(x));
    return r;
}
Color colorOf(const json::value* v, const char* fn) {
    Color c{};
    if (v && v->is_str() && !parseColor(v->str(), c)) fail(std::string(fn) + ": bad colour '" + v->str() + "'");
    return c;
}
template <class E, std::size_t N>
E enumOf(const json::value* v, const std::pair<const char*, E> (&names)[N], E def, const char* fn) {
    if (!v || !v->is_str()) return def;
    for (auto& [n, e] : names)
        if (v->str() == n) return e;
    fail(std::string(fn) + ": unknown value '" + v->str() + "'");
}
using namespace fuse::ui::charts;
constexpr std::pair<const char*, SeriesType> kSeries[] = {
    {"line", SeriesType::Line}, {"spline", SeriesType::Spline}, {"stepLine", SeriesType::StepLine},
    {"scatter", SeriesType::Scatter}, {"area", SeriesType::Area}, {"stackedArea", SeriesType::StackedArea},
    {"percentArea", SeriesType::PercentArea}, {"bar", SeriesType::Bar}, {"stackedBar", SeriesType::StackedBar},
    {"percentBar", SeriesType::PercentBar}, {"horizontalBar", SeriesType::HorizontalBar},
    {"horizontalStackedBar", SeriesType::HorizontalStackedBar}, {"horizontalPercentBar", SeriesType::HorizontalPercentBar},
    {"pie", SeriesType::Pie}, {"donut", SeriesType::Donut}, {"candlestick", SeriesType::Candlestick},
    {"ohlc", SeriesType::Ohlc}, {"boxPlot", SeriesType::BoxPlot}, {"histogram", SeriesType::Histogram},
    {"heatmap", SeriesType::Heatmap}, {"bubble", SeriesType::Bubble}, {"waterfall", SeriesType::Waterfall},
    {"contour", SeriesType::Contour}, {"radar", SeriesType::Radar}, {"treemap", SeriesType::Treemap},
    {"sankey", SeriesType::Sankey}, {"gauge", SeriesType::Gauge}, {"funnel", SeriesType::Funnel}};
constexpr std::pair<const char*, AxisType> kAxisType[] = {
    {"value", AxisType::Value}, {"log", AxisType::Log}, {"category", AxisType::Category}, {"datetime", AxisType::DateTime}};
constexpr std::pair<const char*, AxisEdge> kEdge[] = {
    {"bottom", AxisEdge::Bottom}, {"left", AxisEdge::Left}, {"top", AxisEdge::Top}, {"right", AxisEdge::Right}};
constexpr std::pair<const char*, LegendPos> kLegend[] = {
    {"none", LegendPos::None}, {"top", LegendPos::Top}, {"bottom", LegendPos::Bottom}, {"left", LegendPos::Left}, {"right", LegendPos::Right}};
constexpr std::pair<const char*, Decimation> kDecim[] = {
    {"auto", Decimation::Auto}, {"none", Decimation::None}, {"minmax", Decimation::MinMax}, {"lttb", Decimation::Lttb}};
constexpr std::pair<const char*, Kind3D> kKind3D[] = {
    {"surface", Kind3D::Surface}, {"scatter", Kind3D::Scatter}, {"bars", Kind3D::Bars}, {"line", Kind3D::Line}, {"area", Kind3D::Area}, {"volume", Kind3D::Volume}};
f32 numOr(const json::value& o, const char* k, f32 d) {
    const auto* v = o.find(k);
    return v && v->is_num() ? f32(v->num()) : d;
}
bool boolOr(const json::value& o, const char* k, bool d) {
    const auto* v = o.find(k);
    return v ? truthy(*v) : d;
}
void applyAxis(Axis& ax, const json::value& in) {
    const json::value o = objOf(in);
    if (!o.is_obj()) fail("ui::chartSet: an axis must be a map");
    ax.type = enumOf(o.find("type"), kAxisType, ax.type, "ui::chartSet");
    ax.edge = enumOf(o.find("edge"), kEdge, ax.edge, "ui::chartSet");
    ax.min = numOr(o, "min", ax.min), ax.max = numOr(o, "max", ax.max);
    ax.logBase = numOr(o, "logBase", ax.logBase);
    if (o.find("categories")) ax.categories = strings(o.find("categories"));
    ax.title = o.str_or("title", ax.title);
    ax.labelFormat = o.str_or("labelFormat", ax.labelFormat);
    ax.tickCount = u32(numOr(o, "tickCount", f32(ax.tickCount)));
    ax.grid = boolOr(o, "grid", ax.grid), ax.visible = boolOr(o, "visible", ax.visible);
    ax.reversed = boolOr(o, "reversed", ax.reversed), ax.nice = boolOr(o, "nice", ax.nice);
}
void applySeries(Series& se, const json::value& o) {
    const char* fn = "ui::chartSet";
    se.name = o.str_or("name", se.name);
    if (o.find("color")) se.color = colorOf(o.find("color"), fn);
    se.xAxis = u32(numOr(o, "xAxis", f32(se.xAxis))), se.yAxis = u32(numOr(o, "yAxis", f32(se.yAxis)));
    se.visible = boolOr(o, "visible", se.visible);
    if (o.find("x")) se.x = floats(o.find("x"), fn), se.m_head = 0;
    if (o.find("y")) se.y = floats(o.find("y"), fn), se.m_head = 0;
    if (o.find("y") && !o.find("x") && se.x.size() != se.y.size()) {
        se.x.resize(se.y.size());
        for (std::size_t i = 0; i < se.x.size(); ++i) se.x[i] = f32(i);
    }
    if (o.find("size")) se.size = floats(o.find("size"), fn);
    if (o.find("labels")) se.labels = strings(o.find("labels"));
    if (const auto* c = o.find("candles"); c && c->is_arr()) { // [[x, open, high, low, close], ...]
        se.candles.clear();
        for (const auto& r : c->arr()) {
            const auto v = floats(&r, fn);
            if (v.size() != 5) fail("ui::chartSet: a candle is [x, open, high, low, close]");
            se.candles.push_back({v[0], v[1], v[2], v[3], v[4]});
        }
    }
    if (const auto* b = o.find("boxes"); b && b->is_arr()) { // [[x, lower, q1, median, q3, upper, outliers...], ...]
        se.boxes.clear();
        for (const auto& r : b->arr()) {
            const auto v = floats(&r, fn);
            if (v.size() < 6) fail("ui::chartSet: a box is [x, lower, q1, median, q3, upper, outliers...]");
            se.boxes.push_back({v[0], v[1], v[2], v[3], v[4], v[5], std::vector<f32>(v.begin() + 6, v.end())});
        }
    }
    se.rows = u32(numOr(o, "rows", f32(se.rows))), se.cols = u32(numOr(o, "cols", f32(se.cols)));
    se.bins = u32(numOr(o, "bins", f32(se.bins)));
    if (const auto* l = o.find("links"); l && l->is_arr()) { // sankey: [[source, target, value], ...]
        se.links.clear();
        for (const auto& r : l->arr()) {
            const auto v = floats(&r, fn);
            if (v.size() != 3) fail("ui::chartSet: a link is [source, target, value]");
            se.links.push_back({u32(v[0]), u32(v[1]), v[2]});
        }
    }
    if (const auto* t = o.find("totals"); t && t->is_arr()) {
        se.totals.clear();
        for (f32 v : floats(t, fn)) se.totals.push_back(u32(v));
    }
    if (const auto* cm = o.find("colormap"); cm && cm->is_arr()) { // [[offset, "colour"], ...]
        se.colormap.clear();
        for (const auto& r : cm->arr()) {
            if (!r.is_arr() || r.arr().size() != 2 || !r.arr()[0].is_num()) fail("ui::chartSet: a colormap stop is [offset, colour]");
            se.colormap.push_back({f32(r.arr()[0].num()), colorOf(&r.arr()[1], fn)});
        }
    }
    se.levels = u32(numOr(o, "levels", f32(se.levels)));
    se.contourFill = boolOr(o, "contourFill", se.contourFill);
    se.gaugeMin = numOr(o, "gaugeMin", se.gaugeMin), se.gaugeMax = numOr(o, "gaugeMax", se.gaugeMax);
    se.lineWidth = numOr(o, "lineWidth", se.lineWidth), se.markerSize = numOr(o, "markerSize", se.markerSize);
    se.barWidth = numOr(o, "barWidth", se.barWidth), se.holeSize = numOr(o, "holeSize", se.holeSize);
    se.bubbleScale = numOr(o, "bubbleScale", se.bubbleScale);
    se.decimation = enumOf(o.find("decimation"), kDecim, se.decimation, fn);
    se.capacity = std::size_t(numOr(o, "capacity", f32(se.capacity)));
    if (se.capacity && se.count() > se.capacity) { // keep the newest `capacity` points
        const auto xs = se.xs(), ys = se.ys();
        std::vector<f32> nx(xs.end() - std::ptrdiff_t(std::min(se.capacity, xs.size())), xs.end());
        std::vector<f32> ny(ys.end() - std::ptrdiff_t(se.capacity), ys.end());
        se.x = std::move(nx), se.y = std::move(ny), se.m_head = 0;
    }
}
json::value hitOf(const Hit& h) {
    if (h.series < 0) return json::value(nullptr);
    json::object o;
    o.emplace_back("series", double(h.series));
    o.emplace_back("index", double(h.index));
    o.emplace_back("x", double(h.x));
    o.emplace_back("y", double(h.y));
    return json::value(std::move(o));
}
#endif
} // namespace

void ForgeUi::Impl::installCharts() {
#if FUSE_UI_SCRIPT_CHARTS
    auto& e = s.m_engine;
    Impl* I = this;
    auto def = [&](const char* name, int lo, int hi, auto fn) {
        e.define(std::string("ui::") + name, lo, hi, [I, fn](const std::string& js) -> std::string {
            const json::value v = json::parse(js);
            return fn(*I, v.arr());
        });
    };
    auto chart = [](Impl& I, const Args& a, const char* fn) -> Chart& {
        const NodeHandle h = I.needNode(a, 0, fn);
        Chart* c = chartOf(I.ui(), h);
        if (!c) fail(std::string(fn) + ": not a chart view");
        return *c;
    };
    // chartView(parent [, id]) -> node (QChartView)
    def("chartView", 1, 2, [](Impl& I, const Args& a) {
        const NodeHandle p = I.needNode(a, 0, "ui::chartView");
        return out(node(chartView(I.ui(), p, has(a, 1) ? str(a, 1, "ui::chartView") : std::string())));
    });
    // chartSet(view, {{"title", ..}, {"legend", "bottom"}, {"theme", "light"}, {"polar", b}, {"animate", b},
    //   {"axes", {axis...}}, {"series", {{{"type", "bar"}, {"name", ..}, {"y", {..}}, ...}, ...}}}) replaces what it names
    def("chartSet", 2, 3, [chart](Impl& I, const Args& a) {
        Chart& c = chart(I, a, "ui::chartSet");
        const json::value o = objOf(arg(a, 1));
        if (!o.is_obj()) fail("ui::chartSet: the spec must be a map");
        c.title = o.str_or("title", c.title);
        c.legend = enumOf(o.find("legend"), kLegend, c.legend, "ui::chartSet");
        c.polar = boolOr(o, "polar", c.polar), c.animate = boolOr(o, "animate", c.animate);
        if (const auto* t = o.find("theme"); t && t->is_str()) {
            if (t->str() == "light") c.theme = Theme::light();
            else if (t->str() == "dark") c.theme = Theme::dark();
            else fail("ui::chartSet: theme is \"light\" or \"dark\"");
        }
        if (const auto* ax = o.find("axes"); ax && ax->is_arr()) {
            c.axes.resize(std::max<std::size_t>(ax->arr().size(), 2));
            for (std::size_t i = 0; i < ax->arr().size(); ++i) applyAxis(c.axes[i], ax->arr()[i]);
        }
        if (const auto* sl = o.find("series"); sl && sl->is_arr()) {
            c.series.clear();
            for (const auto& x : sl->arr()) {
                const json::value so = objOf(x);
                if (!so.is_obj()) fail("ui::chartSet: a series must be a map");
                Series& se = c.add(enumOf(so.find("type"), kSeries, SeriesType::Line, "ui::chartSet"));
                applySeries(se, so);
            }
        }
        chartChanged(I.ui(), I.nodeArg(a, 0), truthy(arg(a, 2)));
        return none();
    });
    // chartSeries(view, index, spec): edits one series in place; chartAxis(view, index, spec) likewise
    def("chartSeries", 3, 3, [chart](Impl& I, const Args& a) {
        Chart& c = chart(I, a, "ui::chartSeries");
        const std::size_t i = std::size_t(num(a, 1, "ui::chartSeries"));
        if (i >= c.series.size()) fail("ui::chartSeries: no such series");
        const json::value o = objOf(arg(a, 2));
        if (!o.is_obj()) fail("ui::chartSeries: the spec must be a map");
        if (o.find("type")) c.series[i].type = enumOf(o.find("type"), kSeries, c.series[i].type, "ui::chartSeries");
        applySeries(c.series[i], o);
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartAxis", 3, 3, [chart](Impl& I, const Args& a) {
        Chart& c = chart(I, a, "ui::chartAxis");
        const std::size_t i = std::size_t(num(a, 1, "ui::chartAxis"));
        if (i >= c.axes.size()) c.axes.resize(i + 1);
        applyAxis(c.axes[i], arg(a, 2));
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartSeriesCount", 1, 1, [chart](Impl& I, const Args& a) {
        return out(json::value(double(chart(I, a, "ui::chartSeriesCount").series.size())));
    });
    def("chartPointCount", 2, 2, [chart](Impl& I, const Args& a) {
        Chart& c = chart(I, a, "ui::chartPointCount");
        const std::size_t i = std::size_t(num(a, 1, "ui::chartPointCount"));
        return i < c.series.size() ? out(json::value(double(c.series[i].count()))) : none();
    });
    // chartAppend(view, series, x, y) or (view, series, {xs}, {ys}): streaming (Series::capacity keeps the newest)
    def("chartAppend", 4, 4, [chart](Impl& I, const Args& a) {
        Chart& c = chart(I, a, "ui::chartAppend");
        const std::size_t i = std::size_t(num(a, 1, "ui::chartAppend"));
        if (i >= c.series.size()) fail("ui::chartAppend: no such series");
        if (arg(a, 2).is_arr()) {
            const auto xs = floats(&arg(a, 2), "ui::chartAppend"), ys = floats(&arg(a, 3), "ui::chartAppend");
            if (xs.size() != ys.size()) fail("ui::chartAppend: x and y lengths differ");
            c.series[i].append(xs, ys);
        } else c.series[i].append(f32(num(a, 2, "ui::chartAppend")), f32(num(a, 3, "ui::chartAppend")));
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartClear", 2, 2, [chart](Impl& I, const Args& a) {
        Chart& c = chart(I, a, "ui::chartClear");
        const std::size_t i = std::size_t(num(a, 1, "ui::chartClear"));
        if (i < c.series.size()) c.series[i].clear();
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartZoom", 4, 4, [chart](Impl& I, const Args& a) {
        zoom(chart(I, a, "ui::chartZoom"), u32(num(a, 1, "ui::chartZoom")), f32(num(a, 2, "ui::chartZoom")), f32(num(a, 3, "ui::chartZoom")));
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartPan", 3, 3, [chart](Impl& I, const Args& a) {
        pan(chart(I, a, "ui::chartPan"), u32(num(a, 1, "ui::chartPan")), f32(num(a, 2, "ui::chartPan")));
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartZoomTo", 4, 4, [chart](Impl& I, const Args& a) {
        zoomTo(chart(I, a, "ui::chartZoomTo"), u32(num(a, 1, "ui::chartZoomTo")), f32(num(a, 2, "ui::chartZoomTo")), f32(num(a, 3, "ui::chartZoomTo")));
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chartResetZoom", 1, 1, [chart](Impl& I, const Args& a) {
        resetZoom(chart(I, a, "ui::chartResetZoom"));
        chartChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    // chartRange(view, axis [, full]) -> {lo, hi}
    def("chartRange", 2, 3, [chart](Impl& I, const Args& a) {
        const auto r = axisRanges(chart(I, a, "ui::chartRange"), truthy(arg(a, 2)));
        const std::size_t i = std::size_t(num(a, 1, "ui::chartRange"));
        if (i >= r.size()) return none();
        json::array l;
        l.push_back(double(r[i].first));
        l.push_back(double(r[i].second));
        return out(json::value(std::move(l)));
    });
    def("chartHovered", 1, 1, [](Impl& I, const Args& a) {
        return out(hitOf(chartHovered(I.ui(), I.needNode(a, 0, "ui::chartHovered"))));
    });
    def("chartTooltip", 1, 3, [chart](Impl& I, const Args& a) {
        if (has(a, 1))
            return out(json::value(tooltip(chart(I, a, "ui::chartTooltip"), u32(num(a, 1, "ui::chartTooltip")), u32(num(a, 2, "ui::chartTooltip")))));
        return out(json::value(chartTooltip(I.ui(), I.needNode(a, 0, "ui::chartTooltip"))));
    });
    def("chartSelection", 1, 1, [chart](Impl& I, const Args& a) {
        json::array l;
        for (const Selected& x : chart(I, a, "ui::chartSelection").selection) {
            json::array p;
            p.push_back(double(x.series));
            p.push_back(double(x.index));
            l.push_back(json::value(std::move(p)));
        }
        return out(json::value(std::move(l)));
    });
    def("chartDescribe", 1, 1, [](Impl& I, const Args& a) {
        const NodeHandle h = I.needNode(a, 0, "ui::chartDescribe");
        if (Chart* c = chartOf(I.ui(), h)) return out(json::value(describe(*c)));
        if (Chart3D* c = chart3dOf(I.ui(), h)) return out(json::value(describe(*c)));
        fail("ui::chartDescribe: not a chart view");
    });
    def("chartExport", 2, 4, [](Impl& I, const Args& a) {
        const NodeHandle h = I.needNode(a, 0, "ui::chartExport");
        const std::string path = str(a, 1, "ui::chartExport");
        const int w = has(a, 2) ? int(num(a, 2, "ui::chartExport")) : 0, hh = has(a, 3) ? int(num(a, 3, "ui::chartExport")) : 0;
        if (chartOf(I.ui(), h)) return out(json::value(exportImage(I.ui(), h, path, w, hh)));
        if (chart3dOf(I.ui(), h)) return out(json::value(exportImage3d(I.ui(), h, path, w, hh)));
        fail("ui::chartExport: not a chart view");
    });
    // ---- 3D graphs
    auto chart3 = [](Impl& I, const Args& a, const char* fn) -> Chart3D& {
        Chart3D* c = chart3dOf(I.ui(), I.needNode(a, 0, fn));
        if (!c) fail(std::string(fn) + ": not a 3D chart view");
        return *c;
    };
    def("chart3dView", 2, 3, [](Impl& I, const Args& a) {
        const NodeHandle p = I.needNode(a, 0, "ui::chart3dView");
        const json::value k = arg(a, 1);
        const Kind3D kind = enumOf(&k, kKind3D, Kind3D::Surface, "ui::chart3dView");
        return out(node(chart3dView(I.ui(), p, kind, has(a, 2) ? str(a, 2, "ui::chart3dView") : std::string())));
    });
    // chart3dSet(view, {{"rows", r}, {"cols", c}, {"values", {..}}, {"points", {{x, y, z}, ..}}, {"pointColors", {..}},
    //   {"axes", {{{"title", ..}, {"min", ..}, {"max", ..}, {"ticks", n}}, ..}}, {"camera", {{"yaw", ..}, ..}},
    //   {"wireframe", b}, {"shading", b}, {"grid", b}, {"pointSize", px}, {"lineWidth", px}, {"seriesColors", {..}},
    //   {"maxPoints", n}, {"maxGrid", n}})
    def("chart3dSet", 2, 2, [chart3](Impl& I, const Args& a) {
        const char* fn = "ui::chart3dSet";
        Chart3D& c = chart3(I, a, fn);
        const json::value o = objOf(arg(a, 1));
        if (!o.is_obj()) fail("ui::chart3dSet: the spec must be a map");
        c.rows = u32(numOr(o, "rows", f32(c.rows))), c.cols = u32(numOr(o, "cols", f32(c.cols)));
        if (o.find("values")) c.values = floats(o.find("values"), fn);
        if (const auto* pts = o.find("points"); pts && pts->is_arr()) {
            c.points.clear();
            for (const auto& p : pts->arr()) {
                const auto v = floats(&p, fn);
                if (v.size() != 3) fail("ui::chart3dSet: a point is {x, y, z}");
                c.points.push_back({v[0], v[1], v[2]});
            }
        }
        auto colours = [&](const char* k, std::vector<Color>& dst) {
            if (const auto* l = o.find(k); l && l->is_arr()) {
                dst.clear();
                for (const auto& x : l->arr()) dst.push_back(colorOf(&x, fn));
            }
        };
        colours("pointColors", c.pointColors);
        colours("seriesColors", c.seriesColors);
        c.pointSize = numOr(o, "pointSize", c.pointSize), c.lineWidth = numOr(o, "lineWidth", c.lineWidth);
        c.wireframe = boolOr(o, "wireframe", c.wireframe), c.shading = boolOr(o, "shading", c.shading);
        c.grid = boolOr(o, "grid", c.grid);
        c.maxPoints = u32(numOr(o, "maxPoints", f32(c.maxPoints))), c.maxGrid = u32(numOr(o, "maxGrid", f32(c.maxGrid)));
        c.vx = u32(numOr(o, "vx", f32(c.vx))), c.vy = u32(numOr(o, "vy", f32(c.vy))), c.vz = u32(numOr(o, "vz", f32(c.vz)));
        if (o.find("volume")) c.volume = floats(o.find("volume"), fn);
        if (const auto* sl = o.find("slice"); sl && sl->is_arr())
            for (std::size_t i = 0; i < 3 && i < sl->arr().size(); ++i)
                if (sl->arr()[i].is_num()) c.slice[i] = i32(sl->arr()[i].num());
        c.valueMin = numOr(o, "valueMin", c.valueMin), c.valueMax = numOr(o, "valueMax", c.valueMax);
        c.gpuMesh = boolOr(o, "gpuMesh", c.gpuMesh);
        if (const auto* ax = o.find("axes"); ax && ax->is_arr())
            for (std::size_t i = 0; i < ax->arr().size() && i < 3; ++i) {
                const json::value x = objOf(ax->arr()[i]);
                if (!x.is_obj()) fail("ui::chart3dSet: an axis must be a map");
                c.axis[i].min = numOr(x, "min", c.axis[i].min), c.axis[i].max = numOr(x, "max", c.axis[i].max);
                c.axis[i].title = x.str_or("title", c.axis[i].title);
                c.axis[i].ticks = u32(numOr(x, "ticks", f32(c.axis[i].ticks)));
            }
        if (const auto* cam = o.find("camera")) {
            const json::value x = objOf(*cam);
            if (!x.is_obj()) fail("ui::chart3dSet: camera must be a map");
            c.camera.yaw = numOr(x, "yaw", c.camera.yaw), c.camera.pitch = numOr(x, "pitch", c.camera.pitch);
            c.camera.zoom = numOr(x, "zoom", c.camera.zoom), c.camera.perspective = boolOr(x, "perspective", c.camera.perspective);
        }
        chart3dChanged(I.ui(), I.nodeArg(a, 0));
        return none();
    });
    def("chart3dCamera", 1, 1, [chart3](Impl& I, const Args& a) {
        const Camera3D& cam = chart3(I, a, "ui::chart3dCamera").camera;
        json::object o;
        o.emplace_back("yaw", double(cam.yaw));
        o.emplace_back("pitch", double(cam.pitch));
        o.emplace_back("zoom", double(cam.zoom));
        o.emplace_back("perspective", cam.perspective);
        return out(json::value(std::move(o)));
    });
    def("chart3dHovered", 1, 1, [](Impl& I, const Args& a) {
        const i32 i = chart3dHovered(I.ui(), I.needNode(a, 0, "ui::chart3dHovered"));
        return i < 0 ? none() : out(json::value(double(i)));
    });
    def("chart3dSelection", 1, 1, [](Impl& I, const Args& a) {
        json::array l;
        for (u32 i : chart3dSelection(I.ui(), I.needNode(a, 0, "ui::chart3dSelection"))) l.push_back(double(i));
        return out(json::value(std::move(l)));
    });
    def("chart3dTooltip", 1, 1, [](Impl& I, const Args& a) {
        json::array l;
        for (auto& t : chart3dTooltip(I.ui(), I.needNode(a, 0, "ui::chart3dTooltip"))) l.push_back(t);
        return out(json::value(std::move(l)));
    });
#endif
}

ForgeUi::ForgeUi(forge::script::engine& engine, UiContext& ui, doc::Model& model)
    : m_engine(engine), m_ui(ui), m_model(model), m_impl(std::make_unique<Impl>(*this)) {
    m_impl->install();
}

ForgeUi::~ForgeUi() {
    for (auto& [cb, c] : m_impl->connections) m_ui.disconnect(c);
    m_docs.clear();
    for (std::uint64_t cb : m_impl->owned) m_engine.release(cb);
}

void ForgeUi::update() {
    for (auto& d : m_docs) d->refresh();
}

bool ForgeUi::run(std::string_view source, std::string* err, const std::string& name) {
    auto r = m_engine.run(source, name);
    if (!r.ok && err) *err = r.error;
    return r.ok;
}

bool ForgeUi::runFile(const std::string& path, std::string* err) {
    auto r = m_engine.run_file(path);
    if (!r.ok && err) *err = r.error;
    return r.ok;
}

} // namespace fuse::ui::script
