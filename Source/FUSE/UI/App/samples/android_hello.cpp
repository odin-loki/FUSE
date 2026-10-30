// android_hello (G4 sample): NativeActivity entry point. Logs "fuse_android: clicks=N text=..." to logcat on each
// change so the emulator smoke run (android/run_emulator_smoke.sh) can check input, IME and a11y end to end.
#include "fuse/app/android.hpp"
#include "fuse/ui/doc/document.hpp"
#include <android/log.h>
#include <android_native_app_glue.h>

static const char* kUi = R"({"fuseui":1,
  "style":".root { gap: 12px; padding: 16px; align-items: start; } .big { font-size: 22px; } button { font-size: 18px; } lineedit { font-size: 18px; width: 260px; }",
  "root":{"class":"panel","classes":["root"],"children":[
    {"class":"label","classes":["big"],"text":"Hello, FUSE on Android"},
    {"class":"label","id":"count","text":"Clicked {clicks} times"},
    {"class":"button","id":"click","text":"Click me","on":{"clicked":"click"}},
    {"class":"lineedit","id":"edit","text":""}]}})";

void android_main(android_app* state) {
    __android_log_print(ANDROID_LOG_INFO, "fuse_android", "start");
    fuse::app::androidInstallFonts(state);
    fuse::app::Application app{{.name = "android_hello", .loadSettings = false}};
    fuse::app::AndroidPlatform platform(app, state);
    auto& w = app.createWindow({.title = "FUSE"});
    fuse::ui::doc::Model model;
    model.set("clicks", 0.0);
    if (auto r = w.loadText(kUi, &model); !r) {
        __android_log_print(ANDROID_LOG_ERROR, "fuse_android", "ui: %s", r.error().c_str());
        return;
    }
    w.document()->onAction([&](std::string_view a, fuse::ui::NodeHandle, const fuse::ui::SignalArgs&) {
        if (a == "click") {
            model.set("clicks", model.number("clicks") + 1.0);
            __android_log_print(ANDROID_LOG_INFO, "fuse_android", "clicks=%d", int(model.number("clicks")));
        }
    });
    auto& ui = w.ui();
    const fuse::ui::NodeHandle edit = ui.find("edit");
    ui.connect(edit, fuse::ui::SignalKind::TextChanged, [&](fuse::ui::UiContext&, fuse::ui::NodeHandle, const fuse::ui::SignalArgs&) {
        __android_log_print(ANDROID_LOG_INFO, "fuse_android", "text=%s", std::string(ui.text(edit)).c_str());
    });
    platform.show(w);
    __android_log_print(ANDROID_LOG_INFO, "fuse_android", "run");
    const int rc = app.run();
    __android_log_print(ANDROID_LOG_INFO, "fuse_android", "exit %d", rc);
}
