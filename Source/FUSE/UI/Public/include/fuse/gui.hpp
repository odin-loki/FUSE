// FUSE GUI — the public API. Packages include this one header and link one CMake target, `fuse::gui`.
//
//   #include <fuse/gui.hpp>
//   int main(int argc, char** argv) {
//       fuse::gui::Application app{{.name = "demo", .argc = argc, .argv = argv}};
//       auto& win = app.createWindow({.title = "Demo", .width = 640, .height = 400});
//       auto& ui = win.ui();
//       auto col = fuse::gui::widgets::column(ui, ui.root());
//       fuse::gui::widgets::label(ui, col, "Hello");
//       fuse::gui::NativePlatform native{app};   // OS window where there is a display, headless otherwise
//       return app.run();
//   }
//
// Stability (docs/gui/PLAN.md §4): everything reachable from this header is public API, source-compatible within
// a major version. Headers under src/ and *_internal.hpp are not. OS services come from FORGE via fuse::ui::os.
#pragma once

#define FUSE_GUI_VERSION_MAJOR 0
#define FUSE_GUI_VERSION_MINOR 1
#define FUSE_GUI_VERSION_PATCH 0
#define FUSE_GUI_VERSION (FUSE_GUI_VERSION_MAJOR * 10000 + FUSE_GUI_VERSION_MINOR * 100 + FUSE_GUI_VERSION_PATCH)

#include "fuse/text/text.hpp"
#include "fuse/ui/os.hpp"
#include "fuse/ui/style.hpp"
#include "fuse/ui/ui.hpp"
#include "fuse/ui/immediate.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"
#include "fuse/ui/doc/document.hpp"
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"

namespace fuse::gui {

// Core retained tree, layout, style, events, draw list.
using namespace fuse::ui;
// Widgets (Tier A–D), themes.
namespace widgets = fuse::ui::widgets;
// Declarative .fuseui documents and data binding.
namespace doc = fuse::ui::doc;
// Text: fonts, shaping, layout, editing buffers.
namespace text = fuse::text;
// OS seam (FORGE-backed).
namespace os = fuse::ui::os;

// Application shell.
using fuse::app::Application;
using fuse::app::AppDesc;
using fuse::app::AppWindow;
using fuse::app::WindowDesc;
using fuse::app::Settings;
using fuse::app::NativePlatform;
using fuse::app::NativeOptions;

struct Version {
    int major, minor, patch;
};
constexpr Version version() { return {FUSE_GUI_VERSION_MAJOR, FUSE_GUI_VERSION_MINOR, FUSE_GUI_VERSION_PATCH}; }

} // namespace fuse::gui
