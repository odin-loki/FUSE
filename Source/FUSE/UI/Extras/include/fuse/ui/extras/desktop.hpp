// Desktop integration (Qt QSystemTrayIcon::showMessage / QDesktopServices / QPrinter dispatch parity): desktop
// notifications, opening URLs and files with the default handler, printer enumeration and print-job submission.
// All OS work goes through the fuse::ui::os seam (helpers spawned via FORGE). Each call has a *Command() twin that
// returns the argv it would run, so behaviour is testable without a desktop.
#pragma once

#include <cstdint>
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
};
std::vector<std::string> notifyCommand(const Notification& n, std::string_view platform = {});
/// Shows a desktop notification; false when no notification service exists (mobile backends post their own).
bool notify(const Notification& n);

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
