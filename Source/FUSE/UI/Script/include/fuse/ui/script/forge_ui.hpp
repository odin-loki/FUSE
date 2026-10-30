// FUSE UI scripting (fuse_ui_script, plan §4.9): the `ui::` functions of FORGE script (forge::script, the C++23-subset
// language in vendor/forge/script, docs/scripting.md) over one UiContext and one binding Model.
//
//   auto h = ui::find("save");                    // node handle (int) or nullptr
//   ui::setText(h, "Saved"); ui::text(h);         // any node argument may also be an id string: ui::text("save")
//   ui::setValue(h, 0.5); ui::setValue(h, 0.5, true /*emit*/); ui::value(h); ui::setChecked(h, true); ui::checked(h)
//   ui::setDisabled(h, true); ui::disabled(h); ui::focus(); ui::focus(h); ui::id(h); ui::alive(h)
//   auto c = ui::on("save", "clicked", [](auto h, auto a) { ... });   // a = {"value":, "flag":, "text":}; ui::off(c)
//   ui::action("save", [](auto h, auto a) { ... });                   // .fuseui "action": handlers
//   ui::set("player.health", 42); ui::get("player.health");           // binding model
//   auto root = ui::load(json_text[, parent]); ui::loadFile("ui/main.fuseui"[, parent]); ui::refresh();
//   ui::create("button"[, parent[, "id"]]); ui::destroy(h); ui::addClass(h, "primary"); ui::removeClass(h, "x");
//   ui::hasClass(h, "x"); ui::css(h, "width", "80px")
//   tree: ui::root() parent firstChild lastChild nextSibling prevSibling childCount childAt childIndex children
//         moveTo(n, parent[, before]) insertBefore(n, ref) insertAfter(n, ref) setChildIndex(n, i) swap(a, b)
//         reorder(parent, {h...}) sortChildren(parent, [](auto a, auto b) { return ...; })
//   animations (Web Animations): auto a = ui::animate(h, "from { opacity: 0 } to { opacity: 1 }",
//         {{"duration", 0.3}, {"easing", "ease-out"}, {"fill", "forwards"}, {"onfinish", [](auto a) { ... }}});
//         ui::animPlay/animPause/animReverse/animFinish/animCancel/animCommitStyles(a), animSetCurrentTime(a, s),
//         animSetPlaybackRate(a, r), animCurrentTime/animPlaybackRate/animPlayState/animProgress/animId/animTarget(a),
//         ui::animations(h[, subtree])
//   signals: clicked toggled valueChanged textChanged submitted focusChanged hoverChanged editingFinished highlighted
//            activated transitionRun/Start/End/Cancel animationStart/Iteration/End/Cancel finish cancel
//
// Errors are script exceptions (HostError, catchable with try / catch), never C++ exceptions out of run(). Handles are
// generation-checked: a stale handle reads as nullptr from every getter and is ignored by setters.
#pragma once

#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/ui.hpp"

#include <forge/script/script.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::script {

class ForgeUi {
public:
    /// Installs the `ui::` functions into `engine`. The ForgeUi must outlive every script call into it.
    ForgeUi(forge::script::engine& engine, UiContext& ui, doc::Model& model);
    ~ForgeUi();
    ForgeUi(const ForgeUi&) = delete;
    ForgeUi& operator=(const ForgeUi&) = delete;

    /// Per-frame: refreshes every loaded document's bindings.
    void update();
    /// Runs a script (top-level statements; globals persist between runs). On error returns false and fills `err`.
    bool run(std::string_view source, std::string* err = nullptr, const std::string& name = "fuseui");
    bool runFile(const std::string& path, std::string* err = nullptr);

    UiContext& context() { return m_ui; }
    doc::Model& model() { return m_model; }
    forge::script::engine& engine() { return m_engine; }
    const std::vector<std::unique_ptr<doc::Document>>& documents() const { return m_docs; }
    /// Script callbacks currently held (signal handlers, actions, animation callbacks).
    std::size_t callbackCount() const { return m_engine.callbacks(); }

    static std::int64_t pack(NodeHandle h) { return std::int64_t((u64(h.generation) << 32) | h.index); }
    static NodeHandle unpack(std::int64_t v) { return {u32(u64(v) & 0xFFFFFFFFu), u32(u64(v) >> 32)}; }

    struct Impl;

private:
    forge::script::engine& m_engine;
    UiContext& m_ui;
    doc::Model& m_model;
    std::vector<std::unique_ptr<doc::Document>> m_docs;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fuse::ui::script
