// FUSE GUI — the public API. Packages include this one header and link one CMake target, `fuse::gui`.
//
//   #include <fuse/gui.hpp>
//   int main(int argc, char** argv) {
//       fuse::gui::Application app{{.name = "demo", .argc = argc, .argv = argv}};
//       auto& win = app.createWindow({.title = "Demo", .width = 640, .height = 400});
//       auto& ui = win.ui();
//       auto col = fuse::gui::widgets::panel(ui, ui.root());
//       fuse::gui::widgets::label(ui, col, "Hello");
//       fuse::gui::NativePlatform native{app};   // OS window where there is a display, headless otherwise
//       native.show(win);
//       return app.run();
//   }
//
// Everything reachable from this header is public API; headers under src/ and *_internal.hpp are not. The Extras
// modules (undo, declarative bindings, rich text editor, PDF, desktop integration, tray, web view, charts, audio level
// meter, gesture recogniser) and the OS seam are included when the tree was built with them (FUSE_BUILD_UI_EXTRAS):
// FUSE_GUI_HAS_EXTRAS says so. The CMake package (find_package(FuseUI)) stays the way to consume single libraries.
#pragma once

#define FUSE_GUI_VERSION_MAJOR 0
#define FUSE_GUI_VERSION_MINOR 1
#define FUSE_GUI_VERSION_PATCH 0
#define FUSE_GUI_VERSION (FUSE_GUI_VERSION_MAJOR * 10000 + FUSE_GUI_VERSION_MINOR * 100 + FUSE_GUI_VERSION_PATCH)

#include "fuse/text/text.hpp"
#include "fuse/ui/style.hpp"
#include "fuse/ui/ui.hpp"
#include "fuse/ui/immediate.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"
#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/doc/hot_reload.hpp"
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#if __has_include("fuse/ui/extras/undo.hpp") && __has_include("fuse/ui/os.hpp")
#define FUSE_GUI_HAS_EXTRAS 1
#include "fuse/ui/os.hpp"
#include "fuse/ui/extras/audio_meter.hpp"
#include "fuse/ui/extras/charts.hpp"
#include "fuse/ui/extras/declarative.hpp"
#include "fuse/ui/extras/desktop.hpp"
#include "fuse/ui/extras/gesture.hpp"
#include "fuse/ui/extras/pdf.hpp"
#include "fuse/ui/extras/richtext.hpp"
#include "fuse/ui/extras/tray.hpp"
#include "fuse/ui/extras/undo.hpp"
#include "fuse/ui/extras/webview.hpp"
#else
#define FUSE_GUI_HAS_EXTRAS 0
#endif

namespace fuse::gui {

// Core retained tree, layout, style, events, draw list.
using namespace fuse::ui;
// Widgets, themes.
namespace widgets = fuse::ui::widgets;
// Declarative .fuseui documents and data binding.
namespace doc = fuse::ui::doc;
// Text: fonts, shaping, layout, editing buffers.
namespace text = fuse::text;
#if FUSE_GUI_HAS_EXTRAS
namespace os = fuse::ui::os;           // OS seam
namespace desktop = fuse::ui::desktop; // notify / openUrl / print
namespace pdf = fuse::ui::pdf;
namespace charts = fuse::ui::charts;
namespace rich = fuse::ui::rich;       // rich text document / editor
namespace gesture = fuse::ui::gesture; // stand-alone gesture recogniser
#endif

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
