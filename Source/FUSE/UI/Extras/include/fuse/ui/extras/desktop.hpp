// Desktop integration (Qt QSystemTrayIcon::showMessage / QDesktopServices / QPrinter dispatch parity): desktop
// notifications, opening URLs and files with the default handler, printer enumeration and print-job submission.
// All OS work goes through the fuse::ui::os seam (helper programs run with os::runProcess). Each call has a *Command() twin that
// returns the argv it would run, so behaviour is testable without a desktop.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::desktop {

enum class Urgency : std::uint8_t { Low, Normal, Critical };
struct Notification {
    std::string appName = "FUSE";
    std::string title, body;
    std::string icon;          ///< icon name or path (Linux/BSD)
    Urgency urgency = Urgency::Normal;
    int timeoutMs = -1;        ///< -1: server default
    /// Buttons, as (key, label) pairs; the key "default" is the click on the notification itself. Native service
    /// only (Notifier); the helper programs ignore them.
    std::vector<std::pair<std::string, std::string>> actions{};
    std::uint32_t replacesId = 0; ///< Notifier: update the notification with this id instead of adding one
};
std::vector<std::string> notifyCommand(const Notification& n, std::string_view platform = {});
/// Shows a desktop notification: through the native service where there is one (Linux / BSD: notifyNative), else by
/// the platform's helper program (notifyCommand). False when neither exists (mobile backends post their own).
bool notify(const Notification& n);

enum class NotifyResult : std::uint8_t {
    Shown,       ///< handed to the OS notification service
    Unsupported, ///< no notification service on this platform / session
    Failed,      ///< the service exists but refused the request
};
/// The desktop's notification service without a helper process: org.freedesktop.Notifications on the session D-Bus
/// (extras/dbus.hpp, libdbus-1 loaded at run time), with action buttons, click / close callbacks, replacing and
/// closing. Unsupported where there is no session bus or no service on it (Windows, macOS, mobile, a bare X session):
/// desktop::notify falls back to the helper programs there.
class Notifier {
public:
    Notifier();
    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;
    /// A service owns org.freedesktop.Notifications now. (show() may still work without: the bus can start one.)
    bool supported();
    /// Posts `n`; `id` receives the service's id for close() / replacesId / the callbacks.
    NotifyResult show(const Notification& n, std::uint32_t* id = nullptr);
    bool close(std::uint32_t id);
    /// The user chose an action (key "default": clicked the notification) / the notification went away (reason:
    /// 1 expired, 2 dismissed by the user, 3 closed by close(), 4 other). Called from poll().
    std::function<void(std::uint32_t id, std::string_view action)> onAction;
    std::function<void(std::uint32_t id, std::uint32_t reason)> onClosed;
    /// Dispatches pending signals (call from the event loop while callbacks are wanted).
    void poll();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
/// One notification through a short-lived Notifier (no callbacks).
NotifyResult notifyNative(const Notification& n);
/// True when notify() can reach a notification service or helper here.
bool notificationsSupported();

std::vector<std::string> openUrlCommand(std::string_view url, std::string_view platform = {});
/// Opens an http(s)/mailto/file URL or a path with the user's default application.
bool openUrl(std::string_view url);

struct PrintJob {
    std::string file;      ///< PDF (from fuse::ui::pdf) or any printable file
    std::string printer;   ///< empty: default printer
    std::string title;
    int copies = 1;
    bool duplex = false;
    bool landscape = false;
};
std::vector<std::string> printCommand(const PrintJob& job, std::string_view platform = {});
/// Submits the job to the OS spooler (CUPS lp / Windows shell print verb). False when no spooler.
bool print(const PrintJob& job);
std::vector<std::string> listPrintersCommand(std::string_view platform = {});
/// Installed printer queue names (empty when none / no spooler).
std::vector<std::string> printers();

/// Shell-quotes for the platform's helper language (AppleScript string / PowerShell single-quoted string).
std::string appleScriptString(std::string_view s);
std::string powerShellString(std::string_view s);

} // namespace fuse::ui::desktop
