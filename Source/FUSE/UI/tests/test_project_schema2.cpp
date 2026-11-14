// fuse_project_schema2 (plan UI5): project.json schema 2 + AppPresent + the minimal init path.
//   * schema 1 files load unchanged (every Samples/**/project.json), write back byte-identical, migrate to schema 2
//     and reload to the same manifest; schema 2 write -> parse -> write is a fixed point
//   * invalid schema 2 files are rejected with a field path
//   * the Application (FUSE UI) template (Templates/FuseUIApp, createAppProject) is a valid app project
//   * ProjectHost brings up only {AppPresent} for an app project (no job scheduler, no game frame / renderer /
//     physics), opens its window with the .fuseui and presents the first frame within 150 ms (headless)
//   * ui.theme dark / light / system; a hybrid project brings its game side up only when a viewport widget shows
//   * a hybrid viewport shows the game side's GPU scene (a red cube probe): its pixels equal the GPU readback
//   --native: the same on a real OS window (Win32 / X11), presented pixels match the CPU raster; the hybrid viewport
//     is sampled in place on a GPU drawn window (zero-copy) and matches the readback; exit 77 without a display
#ifndef FUSE_PROJECT_SCHEMA2_HYBRID
#define FUSE_PROJECT_SCHEMA2_HYBRID 0 // 1: fuse_app_project_hybrid is linked (the engine game side)
#endif
#if FUSE_PROJECT_SCHEMA2_HYBRID
#include "fuse/app/project_host_hybrid.hpp"
#else
#include "fuse/app/project_host.hpp"
#endif
#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <fuse/core/init.hpp>
#if FUSE_PROJECT_SCHEMA2_HYBRID
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world3d/world_3d.hpp>
#endif
#include <fuse/core/track_b.hpp>
#include <fuse/project/loader.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace project = fuse::project;
using namespace fuse::ui;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

#ifndef FUSE_REPO_ROOT
#define FUSE_REPO_ROOT "."
#endif

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::erase(s, '\r'); // CRLF checkouts
    return s;
}

void wipe(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

bool sameManifest(const project::ProjectManifest& a, const project::ProjectManifest& b) {
    const auto sameWindows = [&] {
        if (a.windows.size() != b.windows.size()) return false;
        for (std::size_t i = 0; i < a.windows.size(); ++i) {
            const auto &x = a.windows[i], &y = b.windows[i];
            if (x.id != y.id || x.title != y.title || x.width != y.width || x.height != y.height || x.root != y.root ||
                x.redraw != y.redraw)
                return false;
        }
        return true;
    };
    return a.name == b.name && a.kind == b.kind && a.dimensions.enable3D == b.dimensions.enable3D &&
           a.dimensions.enable2D == b.dimensions.enable2D && a.dimensions.enableUI == b.dimensions.enableUI &&
           a.modules.ai == b.modules.ai && a.modules.cinematics == b.modules.cinematics && a.modules.fx == b.modules.fx &&
           a.modules.mechanics == b.modules.mechanics && a.modules.adventure == b.modules.adventure &&
           a.defaultWorld3D == b.defaultWorld3D && a.defaultWorld2D == b.defaultWorld2D && a.workerCap == b.workerCap &&
           a.requiredFeatures == b.requiredFeatures && a.ui.root == b.ui.root && a.ui.styles == b.ui.styles &&
           a.ui.theme == b.ui.theme && a.ui.redraw == b.ui.redraw && a.ui.profile == b.ui.profile &&
           a.ui.text == b.ui.text && sameWindows();
}

void schema1Unchanged() {
    // every schema 1 project in the repo loads as kind game and writes back byte-identical (modulo key order the
    // writer always produced; files written by hand are compared through a reload instead)
    int count = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(FUSE_REPO_ROOT) / "Samples", ec)) {
        if (!entry.is_regular_file() || entry.path().filename() != "project.json") continue;
        const project::LoadResult r = project::loadFromFile(entry.path().string());
        if (r.manifest.schemaVersion != 1u) continue;
        ++count;
        CHECK(r.status == project::LoadStatus::Ok);
        CHECK(r.manifest.kind == project::ProjectKind::Game);
        CHECK(r.manifest.windows.empty() && r.manifest.requiredFeatures == 0u && r.manifest.ui.root.empty());
        CHECK(!project::planInit(r.manifest).appShell);
        const std::string again = project::writeManifestJson(r.manifest);
        CHECK(again.find("\"schemaVersion\": 1,") != std::string::npos && again.find("\"kind\"") == std::string::npos);
        const project::LoadResult reread = project::parseManifest(again, r.manifest.projectRoot);
        CHECK(reread.status == project::LoadStatus::Ok && sameManifest(reread.manifest, r.manifest));
        // migrate in memory -> schema 2 text -> same manifest, now schema 2
        project::ProjectManifest migrated = r.manifest;
        CHECK(project::migrateToLatest(migrated) && !project::migrateToLatest(migrated));
        const std::string v2 = project::writeManifestJson(migrated);
        CHECK(v2.find("\"schemaVersion\": 2,") != std::string::npos && v2.find("\"kind\": \"game\"") != std::string::npos);
        const project::LoadResult r2 = project::parseManifest(v2, r.manifest.projectRoot);
        if (r2.status != project::LoadStatus::Ok) std::fprintf(stderr, "  %s: %s\n", entry.path().string().c_str(), r2.error.c_str());
        CHECK(r2.status == project::LoadStatus::Ok && r2.manifest.schemaVersion == 2u && sameManifest(r2.manifest, r.manifest));
        CHECK(project::writeManifestJson(r2.manifest) == v2);
    }
    std::printf("  schema 1: %d sample project(s) load unchanged, round trip and migrate\n", count);
    CHECK(count >= 3);

    // a schema 1 file written by the old writer is reproduced byte for byte
    project::ProjectManifest m;
    m.schemaVersion = 1;
    m.name = "Legacy \"quoted\"";
    m.dimensions = {true, false, true};
    m.modules.fx = true;
    m.defaultWorld3D = "worlds/a.fuselevel";
    m.workerCap = 4;
    const std::string text = project::writeManifestJson(m);
    const std::string expected = "{\n  \"schemaVersion\": 1,\n  \"name\": \"Legacy \\\"quoted\\\"\",\n  \"dimensions\": {\n"
                                 "    \"enable3D\": true,\n    \"enable2D\": false,\n    \"enableUI\": true\n  },\n"
                                 "  \"modules\": {\n    \"ai\": false,\n    \"cinematics\": false,\n    \"fx\": true,\n"
                                 "    \"mechanics\": false,\n    \"adventure\": false\n  },\n"
                                 "  \"defaultWorld3D\": \"worlds/a.fuselevel\",\n  \"defaultWorld2D\": \"\",\n"
                                 "  \"workerCap\": 4\n}\n";
    CHECK(text == expected);

    // --migrate on disk: schema 1 file -> schema 2 file, second run is a no-op
    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / "migrate";
    wipe(dir);
    CHECK(project::saveToDirectory(m, dir.string()).ok);
    const project::MigrateResult mr = project::migrateProjectFile(dir.string(), true);
    CHECK(mr.ok && mr.changed && mr.fromVersion == 1u);
    const project::LoadResult after = project::loadFromDirectory(dir.string());
    CHECK(after.status == project::LoadStatus::Ok && after.manifest.schemaVersion == 2u && sameManifest(after.manifest, m));
    const project::MigrateResult again = project::migrateProjectFile(dir.string(), true);
    CHECK(again.ok && !again.changed && again.fromVersion == 2u);
}

void schema2RoundTrip() {
    project::ProjectManifest m;
    m.schemaVersion = 2;
    m.name = "Tool";
    m.kind = project::ProjectKind::Hybrid;
    m.dimensions = {true, false, true};
    m.ui.root = "ui/main.fuseui";
    m.ui.styles = {"ui/theme.fusestyle", "ui/extra.fusestyle"};
    m.ui.theme = "dark";
    m.ui.profile = project::UiProfile::Editor;
    m.ui.text = project::TextProfile::Full;
    m.requiredFeatures = project::hostFeatureBit(project::HostFeature::Jobs) | project::hostFeatureBit(project::HostFeature::Script);
    project::WindowSettings a;
    a.title = "Main \"window\"";
    a.width = 1280;
    a.height = 720;
    project::WindowSettings b;
    b.id = "inspector";
    b.root = "ui/inspector.fuseui";
    b.redraw = project::RedrawMode::Continuous;
    m.windows = {a, b};
    CHECK(project::validateManifest(m).empty());
    const std::string text = project::writeManifestJson(m);
    const project::LoadResult r = project::parseManifest(text, "/root");
    if (r.status != project::LoadStatus::Ok) std::fprintf(stderr, "  %s\n", r.error.c_str());
    CHECK(r.status == project::LoadStatus::Ok && sameManifest(r.manifest, m) && r.manifest.projectRoot == "/root");
    CHECK(project::writeManifestJson(r.manifest) == text);

    // plans: hybrid starts the app shell and defers the game side; game starts everything; app only AppPresent
    const project::InitPlan hp = project::planInit(m);
    CHECK(hp.appShell && hp.has(project::HostFeature::AppPresent) && hp.has(project::HostFeature::Jobs) &&
          hp.has(project::HostFeature::Viewport) && !hp.has(project::HostFeature::GameFrame) &&
          !hp.has(project::HostFeature::Renderer3D) && !hp.has(project::HostFeature::Physics));
    CHECK((hp.deferred & project::hostFeatureBit(project::HostFeature::Renderer3D)) != 0u &&
          (hp.deferred & project::hostFeatureBit(project::HostFeature::GameFrame)) != 0u);
    project::ProjectManifest game;
    game.name = "G";
    const project::InitPlan gp = project::planInit(game);
    CHECK(!gp.appShell && gp.has(project::HostFeature::GameFrame) && gp.has(project::HostFeature::Renderer3D) &&
          gp.has(project::HostFeature::Physics) && !gp.has(project::HostFeature::AppPresent));
    const project::InitPlan ap = project::planInit(project::appTemplateManifest("A"));
    CHECK(ap.appShell && ap.startup == project::hostFeatureBit(project::HostFeature::AppPresent) && ap.deferred == 0u);
    std::printf("  plans: app [%s], hybrid [%s] deferred [%s], game [%s]\n", project::hostFeatureList(ap.startup).c_str(),
                project::hostFeatureList(hp.startup).c_str(), project::hostFeatureList(hp.deferred).c_str(),
                project::hostFeatureList(gp.startup).c_str());
}

void rejectsInvalid() {
    const auto base = [](std::string_view extra) {
        return std::string("{ \"schemaVersion\": 2, \"name\": \"X\", \"kind\": \"app\", "
                           "\"dimensions\": { \"enable3D\": false, \"enable2D\": false, \"enableUI\": true }, "
                           "\"ui\": { \"root\": \"ui/main.fuseui\" }") +
               std::string(extra) + " }";
    };
    CHECK(project::parseManifest(base(""), "").status == project::LoadStatus::Ok);
    CHECK(project::parseManifest(base(", \"x-editor\": { \"any\": 1 }"), "").status == project::LoadStatus::Ok);
    struct Case {
        std::string text;
        project::LoadStatus status;
        const char* error;
    };
    const Case cases[] = {
        {"{ \"schemaVersion\": 3, \"name\": \"X\" }", project::LoadStatus::UnsupportedSchema, "expected 1 or 2"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", }", project::LoadStatus::ParseError, "line 1"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"name\": \"Y\" }", project::LoadStatus::ParseError, "duplicate key"},
        {"{ \"schemaVersion\": 2 }", project::LoadStatus::Invalid, "$.name: required"},
        {base(", \"kindd\": \"app\""), project::LoadStatus::Invalid, "$.kindd: unknown key"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"kind\": \"tool\" }", project::LoadStatus::Invalid, "$.kind: expected one of"},
        {base(", \"requiredFeatures\": [\"Teleport\"]"), project::LoadStatus::Invalid, "$.requiredFeatures[0]"},
        {base(", \"requiredFeatures\": [\"Physics\"]"), project::LoadStatus::Invalid, "need kind \"hybrid\""},
        {base(", \"windows\": [ { \"width\": 0 } ]"), project::LoadStatus::Invalid, "windows[0]: width / height"},
        {base(", \"windows\": [ { \"width\": 99999 } ]"), project::LoadStatus::Invalid, "$.windows[0].width: expected an integer"},
        {base(", \"windows\": [ {}, {} ]"), project::LoadStatus::Invalid, "duplicate window id"},
        {base(", \"windows\": [ { \"id\": \"a b\" } ]"), project::LoadStatus::Invalid, "windows[0].id"},
        {base(", \"windows\": [ { \"root\": \"../outside.fuseui\" } ]"), project::LoadStatus::Invalid, "inside the project"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"kind\": \"hybrid\", \"ui\": { \"root\": \"main.json\" } }",
         project::LoadStatus::Invalid, "must end with .fuseui"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"kind\": \"app\" }", project::LoadStatus::Invalid, "no 3D / 2D worlds"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"kind\": \"app\", \"dimensions\": { \"enable3D\": false, "
         "\"enable2D\": false } }", project::LoadStatus::Invalid, "ui.root: app and hybrid projects need"},
        {base(", \"ui\": { \"root\": \"ui/a.fuseui\" }"), project::LoadStatus::ParseError, "duplicate key \"ui\""},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"ui\": { \"redraw\": \"sometimes\" } }", project::LoadStatus::Invalid,
         "$.ui.redraw: expected one of onDemand, continuous"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"kind\": \"hybrid\", \"ui\": { \"root\": \"a.fuseui\", "
         "\"styles\": [\"/abs.fusestyle\"] } }", project::LoadStatus::Invalid, "ui.styles[0]"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"ui\": { \"text\": \"huge\" } }", project::LoadStatus::Invalid, "$.ui.text"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"requiredFeatures\": [\"AppPresent\"] }", project::LoadStatus::Invalid,
         "need kind \"app\" or \"hybrid\""},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"windows\": [ {} ] }", project::LoadStatus::Invalid, "only app and hybrid"},
        {"{ \"schemaVersion\": 2, \"name\": \"X\", \"dimensions\": { \"enable3D\": 1 } }", project::LoadStatus::Invalid,
         "$.dimensions.enable3D: expected true or false"},
        {"{ \"schemaVersion\": 2, \"name\": \"\" }", project::LoadStatus::Invalid, "name: must not be empty"},
    };
    int n = 0;
    for (const Case& c : cases) {
        const project::LoadResult r = project::parseManifest(c.text, "");
        const bool ok = r.status == c.status && r.error.find(c.error) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  case %d: status %d error '%s' (want '%s')\n", n, int(r.status), r.error.c_str(), c.error);
        CHECK(ok);
        ++n;
    }
    std::printf("  rejects %d invalid manifests with field paths\n", n);
}

void templateMatches() {
    const fs::path tpl = fs::path(FUSE_REPO_ROOT) / "Templates" / "FuseUIApp";
    const project::LoadResult r = project::loadFromDirectory(tpl.string());
    if (r.status != project::LoadStatus::Ok) std::fprintf(stderr, "  template: %s\n", r.error.c_str());
    CHECK(r.status == project::LoadStatus::Ok && r.manifest.kind == project::ProjectKind::App);
    CHECK(sameManifest(r.manifest, project::appTemplateManifest("FuseUIApp")));
    CHECK(readFile(tpl / "project.json") == project::writeManifestJson(project::appTemplateManifest("FuseUIApp")));
    CHECK(readFile(tpl / "ui" / "main.fuseui") == project::appTemplateMainUi());

    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / "NewApp";
    wipe(dir);
    CHECK(project::createAppProject(dir.string(), "").ok);
    CHECK(!project::createAppProject(dir.string(), "").ok); // never overwrites
    const project::LoadResult created = project::loadFromDirectory(dir.string());
    CHECK(created.status == project::LoadStatus::Ok && created.manifest.name == "NewApp" &&
          created.manifest.kind == project::ProjectKind::App && fs::exists(dir / "ui" / "main.fuseui"));
}

int minimalInit(bool native) {
    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / (native ? "HostNative" : "Host");
    wipe(dir);
    CHECK(project::createAppProject(dir.string(), "Schema2 App").ok);
    CHECK(!fuse::core::trackBHostFeatureEnabled(fuse::core::TrackBHostFeature::AppPresent));
    CHECK(!fuse::core::isInitialized());
    auto started = fuse::app::ProjectHost::start({.projectPath = dir.string(), .showWindows = native});
    if (!started) {
        std::fprintf(stderr, "  ProjectHost::start: %s\n", started.error().c_str());
        ++g_fail;
        return 1;
    }
    fuse::app::ProjectHost& host = **started;
    if (native && !host.native()) {
        std::printf("fuse_project_schema2_native: skipped (no display)\n");
        return 77;
    }
    const auto& t = host.times();
    std::printf("  %s: up [%s], first frame %.2f ms (manifest %.2f, subsystems %.2f, windows %.2f)\n",
                native ? "native" : "headless", project::hostFeatureList(host.broughtUp()).c_str(), t.firstFrame,
                t.manifest, t.subsystems, t.windows);
    // only the app shell + AppPresent: no job scheduler, no game frame, renderer, worlds or physics
    CHECK(host.broughtUp() == project::hostFeatureBit(project::HostFeature::AppPresent));
    CHECK(host.plan().startup == host.broughtUp() && host.plan().deferred == 0u);
    CHECK(fuse::core::trackBHostFeatureEnabled(fuse::core::TrackBHostFeature::AppPresent));
    CHECK(!fuse::core::trackBHostFeatureEnabled(fuse::core::TrackBHostFeature::EditorViewportPresent));
    CHECK(!fuse::core::isInitialized());
    CHECK(host.app().windowCount() == 1);
    fuse::app::AppWindow* w = host.window("main");
    CHECK(w != nullptr);
    if (!w) return 1;
    CHECK(w->framesPresented() >= 1 && w->title() == "Schema2 App");
    CHECK(w->ui().find("action").valid() && w->ui().text(w->ui().find("title")) == "Hello from FUSE UI");
    // the .fuseui is what the window shows: the button face is not the clear colour
    const Image& img = w->rasterize({});
    const Rect b = w->ui().rect(w->ui().find("action"));
    CHECK(img.width == 800 && img.height == 600 && img.at(int(b.x + b.w * 0.5f), int(b.y + 2.f)) != 0xFF000000u);
    if (!native) {
        CHECK(t.firstFrame <= 150.0);
    } else {
        Image shown;
        // main's presenter is installed by show() (no asynchronous Vulkan start-up to wait for)
        std::printf("  native: presenter %s\n", host.native()->kind(*w) == fuse::app::PresenterKind::Vulkan ? "vulkan" : "software");
        for (int i = 0; i < 20; ++i) host.app().step(0.005f);
        w->requestRedraw();
        const u64 f0 = w->framesPresented();
        for (int i = 0; i < 400 && w->framesPresented() <= f0; ++i) host.app().step(0.01f);
        CHECK(host.native()->capture(*w, shown));
        const Image& ref = w->rasterize({});
        int maxErr = 0;
        std::size_t differ = 0;
        if (shown.width == ref.width && shown.height == ref.height) {
            for (std::size_t i = 0; i < ref.pixels.size(); ++i) {
                int e = 0;
                for (int c = 0; c < 3; ++c)
                    e = std::max(e, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
                maxErr = std::max(maxErr, e);
                differ += e != 0;
            }
        } else {
            maxErr = 255;
        }
        const bool gpu = host.native()->gpuDraws(*w);
        std::printf("  native window: %dx%d via %s, max channel error %d, %zu pixels differ\n", shown.width, shown.height,
                    gpu ? "GPU draw list" : "CPU raster", maxErr, differ);
        CHECK(maxErr <= 2 && double(differ) < 0.005 * double(ref.pixels.size()));
        // idle: no presents while nothing changes
        const u64 idle0 = w->framesPresented();
        for (int i = 0; i < 40; ++i) host.app().step(0.005f);
        CHECK(w->framesPresented() == idle0);
    }
    started->reset(); // tear down: AppPresent withdrawn
    CHECK(!fuse::core::trackBHostFeatureEnabled(fuse::core::TrackBHostFeature::AppPresent));

    // game projects are not run by the app host; unsupported features are refused by name
    project::ProjectManifest game;
    game.name = "G";
    const fs::path gdir = dir.parent_path() / (native ? "GameN" : "Game");
    CHECK(project::saveToDirectory(game, gdir.string()).ok);
    auto refused = fuse::app::ProjectHost::start({.projectPath = gdir.string(), .showWindows = false});
    CHECK(!refused && refused.error().find("game project") != std::string::npos);
    project::ProjectManifest scripted = project::appTemplateManifest("S");
    scripted.requiredFeatures = project::hostFeatureBit(project::HostFeature::Script);
    const fs::path sdir = dir.parent_path() / (native ? "ScriptedN" : "Scripted");
    wipe(sdir);
    CHECK(project::createAppProject(sdir.string(), "S").ok && project::saveToDirectory(scripted, sdir.string()).ok);
    auto noScript = fuse::app::ProjectHost::start({.projectPath = sdir.string(), .showWindows = false});
    CHECK(!noScript && noScript.error().find("Script") != std::string::npos);
    // a declared Jobs feature starts the job scheduler and only that
    project::ProjectManifest jobs = project::appTemplateManifest("J");
    jobs.requiredFeatures = project::hostFeatureBit(project::HostFeature::Jobs);
    CHECK(project::saveToDirectory(jobs, sdir.string()).ok);
    {
        auto withJobs = fuse::app::ProjectHost::start({.projectPath = sdir.string(), .showWindows = false});
        CHECK(withJobs && (*withJobs)->broughtUp() == (project::hostFeatureBit(project::HostFeature::AppPresent) |
                                                       project::hostFeatureBit(project::HostFeature::Jobs)));
        CHECK(fuse::core::isInitialized());
    }
    CHECK(!fuse::core::isInitialized());
    return 0;
}

/// Mean brightness (0..255) of an image region.
double brightness(const Image& img, int x0, int y0, int w, int h) {
    double sum = 0.0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const u32 p = img.at(x, y);
            sum += ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / 3.0;
        }
    return sum / double(w * h);
}

void themes() {
    // ui.theme: dark / light pin the widget theme, system follows the OS, anything else is refused
    double shade[2] = {};
    const char* names[2] = {"dark", "light"};
    for (int i = 0; i < 2; ++i) {
        const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / (std::string("Theme_") + names[i]);
        wipe(dir);
        CHECK(project::createAppProject(dir.string(), "T").ok);
        project::ProjectManifest m = project::appTemplateManifest("T");
        m.ui.theme = names[i];
        CHECK(project::saveToDirectory(m, dir.string()).ok);
        auto host = fuse::app::ProjectHost::start({.projectPath = dir.string(), .showWindows = false});
        CHECK(host.has_value());
        if (!host) continue;
        fuse::app::AppWindow* w = (*host)->window("main");
        CHECK(!w->followsSystemTheme());
        const Rect b = w->ui().rect(w->ui().find("action"));
        shade[i] = brightness(w->rasterize({}), int(b.x + 4), int(b.y + 4), int(b.w - 8), int(b.h - 8));
    }
    std::printf("  ui.theme: button face brightness dark %.0f, light %.0f\n", shade[0], shade[1]);
    CHECK(shade[1] > shade[0] + 60.0);
    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / "Theme_system";
    wipe(dir);
    CHECK(project::createAppProject(dir.string(), "T").ok); // template theme: "system"
    {
        auto host = fuse::app::ProjectHost::start({.projectPath = dir.string(), .showWindows = false});
        CHECK(host && (*host)->window("main") && (*host)->window("main")->followsSystemTheme());
        // following: a system theme change restyles the project's window
        if (host) {
            fuse::app::AppWindow* w = (*host)->window("main");
            const Color before = w->ui().computed(w->ui().root()).background;
            (*host)->app().setSystemTheme({.scheme = fuse::app::ColorScheme::Light});
            (*host)->app().step(0.016f);
            CHECK(!(w->ui().computed(w->ui().root()).background == before));
        }
    }
    project::ProjectManifest bad = project::appTemplateManifest("T");
    bad.ui.theme = "neon";
    CHECK(project::saveToDirectory(bad, dir.string()).ok);
    auto refused = fuse::app::ProjectHost::start({.projectPath = dir.string(), .showWindows = false});
    CHECK(!refused && refused.error().find("ui.theme") != std::string::npos);
}

#if FUSE_PROJECT_SCHEMA2_HYBRID
void hybridViewport() {
    // a hybrid project starts like an app; its game side (frame, 3D world, physics, job scheduler) comes up only
    // when a viewport widget becomes visible, then feeds the viewport every frame
    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / "Hybrid";
    wipe(dir);
    CHECK(project::createAppProject(dir.string(), "H").ok);
    {
        std::ofstream ui(dir / "ui" / "main.fuseui", std::ios::binary | std::ios::trunc);
        ui << R"({ "fuseui": 1, "root": { "class": "panel", "id": "main", "children": [
            { "class": "label", "id": "t", "text": "Hybrid" }, { "class": "viewport", "id": "vp" } ] } })";
    }
    project::ProjectManifest m = project::appTemplateManifest("H");
    m.kind = project::ProjectKind::Hybrid;
    m.dimensions = {true, false, true};
    CHECK(project::saveToDirectory(m, dir.string()).ok);
    CHECK(!fuse::core::isInitialized());
    // without a game side backend a hybrid project is refused (app-only hosts do not link the engine)
    auto noBackend = fuse::app::ProjectHost::start({.projectPath = dir.string(), .showWindows = false});
    CHECK(!noBackend && noBackend.error().find("hybrid") != std::string::npos);
    auto started = fuse::app::ProjectHost::start(
        {.projectPath = dir.string(), .showWindows = false, .gameSide = fuse::app::hybridGameSide(),
         .gpuViewport = false}); // the deferred bring-up and frame cadence; hybridScenePixels covers the GPU path
    if (!started) {
        std::fprintf(stderr, "  hybrid start: %s\n", started.error().c_str());
        ++g_fail;
        return;
    }
    fuse::app::ProjectHost& host = **started;
    fuse::app::AppWindow* w = host.window("main");
    fuse::ui::UiContext& ui = w->ui();
    const NodeHandle vp = ui.find("vp");
    CHECK(host.viewportCount() == 1 && vp.valid());
    const project::HostFeatureSet startUp = host.broughtUp();
    CHECK(startUp == (project::hostFeatureBit(project::HostFeature::AppPresent) | project::hostFeatureBit(project::HostFeature::Viewport)));
    CHECK(!host.gameSideUp() && !fuse::core::isInitialized());
    // hidden viewport: the game side stays down
    ui.setCss(vp, "display", "none");
    const auto runFor = [&](double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) host.app().step(0.02f);
    };
    runFor(0.6);
    CHECK(!host.gameSideUp() && host.viewportFrames() == 0);
    // shown: up within one poll, then game frames reach the viewport
    ui.setCss(vp, "display", "flex");
    ui.setCss(vp, "width", "320px");
    ui.setCss(vp, "height", "240px");
    const u64 framesBefore = w->framesPresented();
    // the first game frame creates the composer's render context (Vulkan when present): allow for it
    const auto shownAt = std::chrono::steady_clock::now();
    while (host.viewportFrames() < 6 && std::chrono::steady_clock::now() - shownAt < std::chrono::seconds(20))
        host.app().step(0.02f);
    std::printf("  hybrid: 6 game frames %.0f ms after the viewport showed\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shownAt).count());
    runFor(0.3); // steady 60 Hz game frames, each presented
    const project::HostFeatureSet up = host.broughtUp();
    std::printf("  hybrid: start [%s], after the viewport showed [%s], %llu game frames, %llu window frames\n",
                project::hostFeatureList(startUp).c_str(), project::hostFeatureList(up).c_str(),
                static_cast<unsigned long long>(host.viewportFrames()),
                static_cast<unsigned long long>(w->framesPresented() - framesBefore));
    CHECK(host.gameSideUp() && host.gameSideHandle() != nullptr && fuse::core::isInitialized());
    CHECK((up & project::hostFeatureBit(project::HostFeature::GameFrame)) != 0u &&
          (up & project::hostFeatureBit(project::HostFeature::Renderer3D)) != 0u &&
          (up & project::hostFeatureBit(project::HostFeature::Physics)) != 0u &&
          (up & project::hostFeatureBit(project::HostFeature::Jobs)) != 0u);
    CHECK(host.viewportFrames() >= 5 && w->framesPresented() > framesBefore + 3);
    // hidden again: game frames stop
    ui.setCss(vp, "display", "none");
    runFor(0.3);
    const u64 stopped = host.viewportFrames();
    runFor(0.5);
    CHECK(host.viewportFrames() == stopped);
    started->reset();
    CHECK(!fuse::core::isInitialized());
}

/// Mean colour (0..255 per channel) of a w x h region of RGBA8 words.
struct Mean {
    double r = 0, g = 0, b = 0;
};
Mean meanOf(const std::vector<u32>& px, u32 stride, int x0, int y0, int w, int h) {
    Mean m;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const u32 p = px[std::size_t(y) * stride + std::size_t(x)];
            m.r += p & 0xFF;
            m.g += (p >> 8) & 0xFF;
            m.b += (p >> 16) & 0xFF;
        }
    const double n = double(w * h);
    return {m.r / n, m.g / n, m.b / n};
}

int hybridScenePixels(bool native) {
    // the viewport shows the game side's real GPU scene: a red cube lit by a sun in front of the camera. Headless (and
    // CPU-presented windows) get the GPU frame's readback; a GPU drawn window samples it in place (zero-copy) and its
    // presented pixels are compared with the readback
    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / (native ? "HybridGpuN" : "HybridGpu");
    wipe(dir);
    CHECK(project::createAppProject(dir.string(), "HG").ok);
    {
        std::ofstream ui(dir / "ui" / "main.fuseui", std::ios::binary | std::ios::trunc);
        ui << R"({ "fuseui": 1, "root": { "class": "panel", "id": "main", "children": [
            { "class": "viewport", "id": "vp" } ] } })";
    }
    project::ProjectManifest m = project::appTemplateManifest("HG");
    m.kind = project::ProjectKind::Hybrid;
    m.dimensions = {true, false, true};
    CHECK(project::saveToDirectory(m, dir.string()).ok);
    auto started = fuse::app::ProjectHost::start(
        {.projectPath = dir.string(), .showWindows = native, .gameSide = fuse::app::hybridGameSide(), .gpuValidation = true});
    if (!started) {
        std::fprintf(stderr, "  hybrid GPU start: %s\n", started.error().c_str());
        ++g_fail;
        return 1;
    }
    fuse::app::ProjectHost& host = **started;
    const char* tag = native ? "hybrid GPU (native)" : "hybrid GPU (headless)";
    if (native && !host.native()) {
        std::printf("  %s: skipped (no display)\n", tag);
        return 77;
    }
    if (!host.viewportGpu()) {
        std::printf("  %s: no Vulkan device, software viewport (pixel checks skipped)\n", tag);
        return 0;
    }
    fuse::app::AppWindow* w = host.window("main");
    fuse::ui::UiContext& ui = w->ui();
    const NodeHandle vp = ui.find("vp");
    ui.setCss(vp, "width", "320px");
    ui.setCss(vp, "height", "240px");
    const auto stepUntil = [&](auto done, double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!done() && std::chrono::steady_clock::now() < end) host.app().step(0.01f);
    };
    stepUntil([&] { return host.gameSideUp(); }, 120.0); // Lavapipe: the first scene frame builds every pipeline
    CHECK(host.gameSideUp());
    if (!host.gameSideUp()) return 1;
    // the probe scene
    auto* composer = static_cast<fuse::hybrid::HybridComposer*>(host.gameSideHandle());
    fuse::world3d::World3D& world = *composer->world3D();
    fuse::world3d::RenderMaterial3D red;
    red.baseColor[0] = 0.9f, red.baseColor[1] = 0.05f, red.baseColor[2] = 0.05f;
    red.emissive[0] = 1.f, red.emissiveIntensity = 1.f;
    world.setMaterial(7, red);
    const f32 centre[3] = {0.f, 0.f, 0.f}, half[3] = {1.f, 1.f, 1.f};
    (void)world.spawnMesh(fuse::world3d::BuiltinMesh::Cube, 7, centre, half);
    const f32 toSun[3] = {0.3f, 1.f, 0.6f}, white[3] = {1.f, 1.f, 1.f};
    (void)world.spawnDirectionalLight(toSun, white, 3.f);
    fuse::world3d::RenderCamera3D cam{};
    cam.eye[2] = 4.f;
    cam.fovY = 0.9f;
    cam.nearPlane = 0.1f;
    cam.farPlane = 100.f;
    cam.valid = true;
    world.setCamera(cam);
    const u64 f0 = host.viewportFrames();
    // the frame converges (eye adaptation): the window may show the previous frame, so compare a settled image
    stepUntil([&] { return host.viewportFrames() >= f0 + 90; }, 120.0);
    CHECK(host.viewportFrames() >= f0 + 90);

    std::vector<u32> gpu;
    u32 gw = 0, gh = 0;
    CHECK(host.viewportReadback(gpu, gw, gh));
    if (gpu.empty()) return 1;
    // known probe: the cube fills the centre (red), the sky the corners
    const Mean c = meanOf(gpu, gw, int(gw / 2 - 8), int(gh / 2 - 8), 16, 16);
    const Mean k = meanOf(gpu, gw, 2, 2, 12, 12);
    std::printf("  %s: %ux%u frame, centre (%.0f, %.0f, %.0f), corner (%.0f, %.0f, %.0f)\n", tag, gw, gh, c.r, c.g, c.b,
                k.r, k.g, k.b);
    // the frame is rendered at the viewport's content size in device pixels (shown 1:1)
    const f32 s = w->scale();
    const Rect content = ui.contentRect(vp);
    CHECK(gw == u32(std::lround(content.w * s)) && gh == u32(std::lround(content.h * s)));
    CHECK(c.r > 100.0 && c.r > c.g + 60.0 && c.r > c.b + 60.0);
    CHECK(std::abs(k.r - c.r) + std::abs(k.g - c.g) + std::abs(k.b - c.b) > 60.0);

    const fuse::app::NativePlatform* np = host.native();
    const bool gpuDrawn = np && host.native()->gpuDraws(*w);
    Image shown;
    if (!native) {
        shown = w->rasterize({}); // headless: the viewport image is the readback of the same frame
    } else {
        // the window's presented pixels inside the viewport
        w->requestRedraw();
        const u64 p0 = w->framesPresented();
        stepUntil([&] { return w->framesPresented() > p0 + 2; }, 10.0);
        CHECK(host.native()->capture(*w, shown));
    }
    const Rect r = fuse::ui::widgets::viewportImageRect(ui, vp);
    const int x0 = int(std::lround(r.x * s)), y0 = int(std::lround(r.y * s));
    CHECK(std::lround(r.w * s) == long(gw) && std::lround(r.h * s) == long(gh));
    int maxErr = 0;
    double sumErr = 0.0;
    std::size_t compared = 0;
    for (u32 y = 2; y + 2 < gh; ++y)
        for (u32 x = 2; x + 2 < gw; ++x) {
            const int sx = x0 + int(x), sy = y0 + int(y);
            if (sx >= shown.width || sy >= shown.height) continue;
            const u32 a = shown.at(sx, sy), b = gpu[std::size_t(y) * gw + x];
            int e = 0;
            for (int ch = 0; ch < 3; ++ch) e = std::max(e, std::abs(int((a >> (8 * ch)) & 0xFF) - int((b >> (8 * ch)) & 0xFF)));
            maxErr = std::max(maxErr, e);
            sumErr += e;
            ++compared;
        }
    if (shown.width > x0 + int(gw / 2) && shown.height > y0 + int(gh / 2))
        std::printf("  %s: shown %dx%d, image rect (%.2f, %.2f) %.2fx%.2f for %ux%u, centre shown 0x%08X vs GPU 0x%08X\n",
                    tag, shown.width, shown.height, double(r.x), double(r.y), double(r.w), double(r.h), gw, gh,
                    shown.at(x0 + int(gw / 2), y0 + int(gh / 2)),
                    gpu[std::size_t(gh / 2) * gw + gw / 2]);
    const double meanErr = compared ? sumErr / double(compared) : 255.0;
    std::printf("  %s: viewport via %s, %llu zero-copy frames, %zu pixels vs the GPU readback: max channel error %d, "
                "mean %.3f\n",
                tag, gpuDrawn ? "GPU draw list" : "CPU image", static_cast<unsigned long long>(host.viewportZeroCopyFrames()),
                compared, maxErr, meanErr);
    CHECK(compared > std::size_t(gw) * gh / 2);
    if (gpuDrawn) CHECK(host.viewportZeroCopyFrames() > 0);
    else CHECK(host.viewportZeroCopyFrames() == 0);
    // headless: the same bytes; on screen: the UI kernel's 1:1 sample + the swapchain conversion. On NVIDIA the
    // +-1 rounding share varies run to run (mean 0.22..0.28 over 5 runs); a misplaced or rescaled image gives far more.
    CHECK(native ? (maxErr <= 2 && meanErr <= 0.5) : maxErr == 0);
    started->reset();
    CHECK(!fuse::core::isInitialized());
    return 0;
}
#else
/// An app-only host (no engine game side linked) refuses a hybrid project with a message that says why.
void hybridViewport() {
    const fs::path dir = fs::temp_directory_path() / "fuse_project_schema2" / "HybridRefused";
    wipe(dir);
    CHECK(project::createAppProject(dir.string(), "H").ok);
    project::ProjectManifest m = project::appTemplateManifest("H");
    m.kind = project::ProjectKind::Hybrid;
    m.dimensions.enable3D = true;
    CHECK(project::saveToDirectory(m, dir.string()).ok);
    auto noBackend = fuse::app::ProjectHost::start({.projectPath = dir.string(), .showWindows = false});
    CHECK(!noBackend && noBackend.error().find("hybrid") != std::string::npos);
    std::printf("  hybrid game side not linked (fuse_app_project_hybrid): deferred bring-up and GPU viewport not run\n");
}
int hybridScenePixels(bool) { return 0; }
#endif

} // namespace

int main(int argc, char** argv) {
    const bool native = argc > 1 && std::string(argv[1]) == "--native";
    if (!native) {
        schema1Unchanged();
        schema2RoundTrip();
        rejectsInvalid();
        templateMatches();
        themes();
        hybridViewport();
        hybridScenePixels(false);
    }
    const int rc = minimalInit(native);
    if (rc == 77) return 77;
    if (native && hybridScenePixels(true) == 77) return 77;
    const char* name = native ? "fuse_project_schema2_native" : "fuse_project_schema2";
    std::printf("%s: %s\n", name, g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
