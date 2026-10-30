// fuse_ui_os_seam: the fuse::ui::os seam (docs/gui/PLAN.md §1) on its default FORGE backend, plus Settings and the
// single-instance lock in fuse_app going through it.
#include "fuse/app/app.hpp"
#include "fuse/ui/os.hpp"
#include "test_util.hpp"

#include <string>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace os = fuse::ui::os;

#if defined(__unix__) || defined(__APPLE__)
// Native dialog backend with a stand-in `zenity` first on PATH: argv shape + multi-path parsing + cancel.
static void testNativeDialog() {
    namespace fs = std::filesystem;
    if (fuse::ui::os::nativeFileDialogCommand({}).size() && fuse::ui::os::nativeFileDialogCommand({})[0] == "osascript")
        return; // macOS uses osascript
    fs::path dir = fs::temp_directory_path() / "fuse_fake_zenity";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "zenity");
        f << "#!/bin/sh\ncase \"$*\" in *--save*) exit 1;; esac\nprintf '/a/one.png\\n/b/two.png\\n'\n";
    }
    fs::permissions(dir / "zenity", fs::perms::owner_all);
    std::string old = fuse::ui::os::env("PATH");
    ::setenv("PATH", (dir.string() + ":" + old).c_str(), 1);
    fuse::ui::os::NativeDialogSpec s;
    s.title = "Pick";
    s.multiple = true;
    s.filters = {{"Images", {"*.png", "*.jpg"}}};
    auto cmd = fuse::ui::os::nativeFileDialogCommand(s);
    UI_CHECK(!cmd.empty() && cmd[0] == "zenity");
    bool sawFilter = false;
    for (auto& a : cmd) sawFilter |= a == "--file-filter=Images | *.png *.jpg";
    UI_CHECK(sawFilter);
    auto r = fuse::ui::os::nativeFileDialog(s);
    UI_CHECK(!r.unavailable);
    UI_CHECK_EQ(r.paths.size(), std::size_t(2));
    if (r.paths.size() == 2) UI_CHECK(r.paths[1] == "/b/two.png");
    s.kind = fuse::ui::os::NativeDialogSpec::Kind::Save;
    auto c = fuse::ui::os::nativeFileDialog(s);
    UI_CHECK(!c.unavailable && c.paths.empty()); // cancelled
    ::setenv("PATH", old.c_str(), 1);
    fs::remove_all(dir);
}
#else
static void testNativeDialog() {}
#endif

int main() {
    testNativeDialog();
    const std::string dir = os::runtimeDir() + "/fuse_ui_os_seam_test/nested";
    const std::string file = dir + "/a.txt";
    UI_CHECK(bool(os::writeFileAtomic(file, "hello\nfuse"))); // creates parents
    UI_CHECK(os::isDirectory(dir));
    auto back = os::readFile(file);
    UI_CHECK(back && *back == "hello\nfuse");
    UI_CHECK(!os::readFile(dir + "/missing.txt"));

    {
        auto list = os::listDirectory(dir);
        bool found = false;
        if (list)
            for (const auto& e : *list) found = found || (e.name == "a.txt" && !e.isDir);
        UI_CHECK(found);
        UI_CHECK(os::exists(file) && !os::exists(dir + "/nope"));
        UI_CHECK(os::absolutePath(dir + "/../nested") == os::absolutePath(dir));
        auto w = os::createFileWatcher();
        UI_CHECK(w->add(file));
        UI_CHECK(w->poll(0).empty());
        UI_CHECK(bool(os::writeFileAtomic(file, "changed")));
        std::vector<std::string> changed;
        for (int i = 0; i < 20 && changed.empty(); ++i) changed = w->poll(50);
        UI_CHECK(!changed.empty());
    }
    const double t0 = os::monotonicSeconds();
    os::sleepSeconds(0.01);
    UI_CHECK(os::monotonicSeconds() - t0 >= 0.009);

    UI_CHECK(!os::env("PATH").empty());
    UI_CHECK_EQ(os::env("FUSE_UI_SURELY_UNSET_VAR", "dflt"), std::string("dflt"));
    UI_CHECK(!os::configDir("app").empty());

    {
        auto a = os::tryLockInstance("fuse_ui_os_seam");
        UI_CHECK(a != nullptr);
        auto b = os::tryLockInstance("fuse_ui_os_seam"); // flock is per open file description: second fails
        UI_CHECK(b == nullptr);
    }
    UI_CHECK(os::tryLockInstance("fuse_ui_os_seam") != nullptr); // released

#if defined(__linux__) || defined(__FreeBSD__)
    auto libc = os::loadLibrary({"libc.so.6", "libc.so.7", "libc.so"});
    UI_CHECK(libc && libc->symbol("strlen") != nullptr);
#endif
    UI_CHECK(os::loadLibrary({"libfuse_definitely_missing.so"}) == nullptr);

    // fuse_app Settings round-trip through the seam
    {
        fuse::app::Settings s;
        s.setPath(dir + "/cfg/settings.ini");
        s.set("theme", "dark");
        s.set("scale", 1.5);
        UI_CHECK(s.save());
        fuse::app::Settings t;
        t.setPath(dir + "/cfg/settings.ini");
        UI_CHECK(t.load());
        UI_CHECK_EQ(std::string(t.get("theme")), std::string("dark"));
        UI_CHECK_EQ(t.number("scale"), 1.5);
    }
    // single-instance
    {
        fuse::app::Application a{{.name = "fuse_ui_os_seam_app", .singleInstance = true, .loadSettings = false}};
        fuse::app::Application b{{.name = "fuse_ui_os_seam_app", .singleInstance = true, .loadSettings = false}};
        UI_CHECK(a.primary());
        UI_CHECK(!b.primary());
    }
    return fuse::ui::test::failures() ? 1 : 0;
}
