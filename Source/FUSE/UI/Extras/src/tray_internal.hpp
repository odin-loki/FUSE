// Shared state between the TrayIcon / GlobalMenuBar front ends and the per-platform backends.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "fuse/ui/extras/tray.hpp"

namespace fuse::ui::tray_detail {

enum Change : u32 { kTitle = 1, kIcon = 2, kToolTip = 4, kStatus = 8, kMenu = 16, kId = 32 };

struct TrayState {
    TrayIcon* owner = nullptr;
    std::string id = "fuse-app", title, ttTitle, ttBody, iconName;
    Image icon;
    TrayStatus status = TrayStatus::Active;
    std::vector<MenuItem> menu;
};
struct TrayBackend {
    virtual ~TrayBackend() = default;
    virtual bool show() = 0;
    virtual void hide() = 0;
    virtual bool visible() const = 0;
    virtual void changed(u32 what) = 0;
    virtual void poll() = 0;
    virtual std::string serviceName() const { return {}; }
};
std::unique_ptr<TrayBackend> makeTrayBackend(TrayState& st);
bool trayAvailable();
const char* trayBackendName();

struct MenuBackend {
    virtual ~MenuBackend() = default;
    virtual bool setMenus(std::vector<MenuItem>& menus) = 0; ///< menus owned by the front end (stable address)
    virtual bool active() const = 0;
    virtual void poll() = 0;
    virtual std::string objectPath() const { return {}; }
};
std::unique_ptr<MenuBackend> makeMenuBackend(u64 nativeWindow);
const char* menuBackendName();

/// Label without mnemonic markers ("&&" -> "&"); mnemonicChar receives the marked character (0 if none).
std::string stripMnemonic(std::string_view label, char* mnemonicChar = nullptr);
/// Straight-alpha ARGB32 in network byte order (the SNI / X11 _NET_WM_ICON layout) from a premultiplied Image.
std::string argb32BigEndian(const Image& img);

/// Depth-first flattening of a menu tree: index == DBusMenu id, entry 0 is the root.
struct FlatMenu {
    struct Node { MenuItem* item = nullptr; i32 parent = -1; std::vector<i32> kids; };
    std::vector<Node> nodes;
    void build(std::vector<MenuItem>& menu);
};

} // namespace fuse::ui::tray_detail
