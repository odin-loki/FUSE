// fuse_ui_script: `ui::` natives for FORGE script. Values cross forge::script::engine::define as JSON (forge::json);
// script functions arrive as {"$callback": id} and are invoked later with engine::call.
#include "fuse/ui/script/forge_ui.hpp"

#include "fuse/ui/style.hpp"

#include <forge/json/json.hpp>

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
};

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
