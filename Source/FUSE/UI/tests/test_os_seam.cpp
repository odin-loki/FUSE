// fuse_ui_os_seam: the fuse::ui::os seam (docs/gui/PLAN.md §1) on its default FORGE backend, plus Settings and the
// single-instance lock in fuse_app going through it.
#include "fuse/app/app.hpp"
#include "fuse/ui/os.hpp"
#include "test_util.hpp"

#include <string>

namespace os = fuse::ui::os;

int main() {
    const std::string dir = os::runtimeDir() + "/fuse_ui_os_seam_test/nested";
    const std::string file = dir + "/a.txt";
    UI_CHECK(bool(os::writeFileAtomic(file, "hello\nfuse"))); // creates parents
    UI_CHECK(os::isDirectory(dir));
    auto back = os::readFile(file);
    UI_CHECK(back && *back == "hello\nfuse");
    UI_CHECK(!os::readFile(dir + "/missing.txt"));

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
