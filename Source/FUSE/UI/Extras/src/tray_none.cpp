// Android has no tray / global menu: inert backends (show() == false, so apps keep their in-window UI).
#if defined(__ANDROID__)
#include "tray_internal.hpp"
namespace fuse::ui::tray_detail {
namespace {
struct NoTray final : TrayBackend {
    bool show() override { return false; }
    void hide() override {}
    bool visible() const override { return false; }
    void changed(u32) override {}
    void poll() override {}
};
struct NoMenu final : MenuBackend {
    bool setMenus(std::vector<MenuItem>&) override { return false; }
    bool active() const override { return false; }
    void poll() override {}
};
} // namespace
std::unique_ptr<TrayBackend> makeTrayBackend(TrayState&) { return std::make_unique<NoTray>(); }
bool trayAvailable() { return false; }
const char* trayBackendName() { return "none"; }
std::unique_ptr<MenuBackend> makeMenuBackend(u64) { return std::make_unique<NoMenu>(); }
const char* menuBackendName() { return "none"; }
} // namespace fuse::ui::tray_detail
#endif
