// fuse_app_host: runs a project.json schema 2 app project through the minimal init path (fuse::app::ProjectHost).
//   fuse_app_host <project dir | project.json> [--headless] [--first-frame] [--screenshot=out.png]
// --first-frame prints the startup times and exits after every window presented once (startup measurements);
// --screenshot writes the main window's CPU raster after the first frame and exits. A "quit" action closes the app.
// fuse_hybrid_host is the same runner with the engine game side linked (hybrid projects).
#if defined(FUSE_APP_HOST_HYBRID)
#include "fuse/app/project_host_hybrid.hpp"
#else
#include "fuse/app/project_host.hpp"
#endif
#include "fuse/ui/doc/document.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const fuse::app::CommandLine args(argc, argv);
    if (args.positional().empty()) {
        std::fprintf(stderr, "usage: fuse_app_host <project dir | project.json> [--headless] [--first-frame] [--screenshot=out.png]\n");
        return 2;
    }
#if defined(FUSE_APP_HOST_HYBRID)
    const fuse::app::GameSideBackend gameSide = fuse::app::hybridGameSide();
#else
    const fuse::app::GameSideBackend gameSide{};
#endif
    auto host = fuse::app::ProjectHost::start({.projectPath = args.positional()[0], .argc = argc, .argv = argv,
                                               .showWindows = !args.has("headless"), .gameSide = gameSide});
    if (!host) {
        std::fprintf(stderr, "fuse_app_host: %s\n", host.error().c_str());
        return 1;
    }
    fuse::app::ProjectHost& h = **host;
    for (fuse::ui::u32 i = 0; i < h.app().windowCount(); ++i)
        if (auto* doc = h.app().window(i)->document())
            doc->onAction([&h](std::string_view action, fuse::ui::NodeHandle, const fuse::ui::SignalArgs&) {
                if (action == "quit") h.app().quit();
            });
    const fuse::app::StartupTimes& t = h.times();
    if (args.has("first-frame") || args.has("screenshot")) {
        std::printf("fuse_app_host: '%s' kind %s, up [%s], %u window(s), presenter %s; manifest %.2f ms, subsystems "
                    "%.2f ms, windows %.2f ms, first frame %.2f ms\n",
                    h.manifest().name.c_str(), std::string(fuse::project::toString(h.manifest().kind)).c_str(),
                    fuse::project::hostFeatureList(h.broughtUp()).c_str(), h.app().windowCount(),
                    h.native() ? "native" : "headless", t.manifest, t.subsystems, t.windows, t.firstFrame);
        if (const std::string_view shot = args.get("screenshot"); !shot.empty()) {
            fuse::app::AppWindow* w = h.app().window(0);
            if (w) w->rasterize({});
            if (!w || !w->writePng(std::string(shot))) return h.app().fail("cannot write screenshot");
        }
        return 0;
    }
    return h.run();
}
