#include "fuse/ui/extras/desktop.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>

namespace fuse::ui::desktop {
namespace {
std::string_view plat(std::string_view p) { return p.empty() ? os::platformName() : p; }
bool unixLike(std::string_view p) { return p == "linux" || p == "freebsd"; }
bool run(std::vector<std::string> argv) {
    if (argv.empty()) return false;
    auto r = os::runProcess(std::move(argv), {}, 15000);
    return r && r->exitCode == 0;
}
} // namespace

std::string appleScriptString(std::string_view s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}
std::string powerShellString(std::string_view s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += '\''; o += c; }
    return o + "'";
}
namespace {
std::string xmlEscape(std::string_view s) {
    std::string o;
    for (char c : s) {
        switch (c) { case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '&': o += "&amp;"; break;
                     case '"': o += "&quot;"; break; default: o += c; }
    }
    return o;
}
} // namespace

std::vector<std::string> notifyCommand(const Notification& n, std::string_view p) {
    p = plat(p);
    if (unixLike(p)) {
        std::vector<std::string> a{"notify-send", "-a", n.appName,
                                   "-u", n.urgency == Urgency::Low ? "low" : n.urgency == Urgency::Critical ? "critical" : "normal"};
        if (!n.icon.empty()) { a.push_back("-i"); a.push_back(n.icon); }
        if (n.timeoutMs >= 0) { a.push_back("-t"); a.push_back(std::to_string(n.timeoutMs)); }
        a.push_back("--");
        a.push_back(n.title);
        if (!n.body.empty()) a.push_back(n.body);
        return a;
    }
    if (p == "macos")
        return {"osascript", "-e", "display notification " + appleScriptString(n.body) + " with title " + appleScriptString(n.title)};
    if (p == "windows") {
        const std::string xml = "<toast><visual><binding template='ToastGeneric'><text>" + xmlEscape(n.title) + "</text><text>" +
                                xmlEscape(n.body) + "</text></binding></visual></toast>";
        const std::string script =
            "[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]>$null;"
            "[Windows.Data.Xml.Dom.XmlDocument,Windows.Data.Xml.Dom.XmlDocument,ContentType=WindowsRuntime]>$null;"
            "$x=New-Object Windows.Data.Xml.Dom.XmlDocument;$x.LoadXml(" + powerShellString(xml) + ");"
            "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier(" + powerShellString(n.appName) +
            ").Show([Windows.UI.Notifications.ToastNotification]::new($x))";
        return {"powershell", "-NoProfile", "-NonInteractive", "-Command", script};
    }
    return {};
}
bool notify(const Notification& n) {
    auto c = notifyCommand(n);
    if (c.empty() || os::findExecutable(c[0]).empty()) return false;
    return run(std::move(c));
}

std::vector<std::string> openUrlCommand(std::string_view url, std::string_view p) {
    p = plat(p);
    const std::string u(url);
    if (unixLike(p)) return {"xdg-open", u};
    if (p == "macos") return {"open", u};
    if (p == "windows") return {"rundll32", "url.dll,FileProtocolHandler", u};
    return {};
}
bool openUrl(std::string_view url) {
    auto c = openUrlCommand(url);
    if (c.empty() || os::findExecutable(c[0]).empty()) return false;
    return run(std::move(c));
}

std::vector<std::string> printCommand(const PrintJob& j, std::string_view p) {
    p = plat(p);
    if (unixLike(p) || p == "macos") {
        std::vector<std::string> a{"lp"};
        if (!j.printer.empty()) { a.push_back("-d"); a.push_back(j.printer); }
        if (j.copies > 1) { a.push_back("-n"); a.push_back(std::to_string(j.copies)); }
        if (!j.title.empty()) { a.push_back("-t"); a.push_back(j.title); }
        if (j.duplex) { a.push_back("-o"); a.push_back(j.landscape ? "sides=two-sided-short-edge" : "sides=two-sided-long-edge"); }
        if (j.landscape) { a.push_back("-o"); a.push_back("landscape"); }
        a.push_back("--");
        a.push_back(j.file);
        return a;
    }
    if (p == "windows") {
        std::string s = "Start-Process -FilePath " + powerShellString(j.file) + " -WindowStyle Hidden ";
        s += j.printer.empty() ? "-Verb Print" : "-Verb PrintTo -ArgumentList " + powerShellString("\"" + j.printer + "\"");
        return {"powershell", "-NoProfile", "-NonInteractive", "-Command", s};
    }
    return {};
}
bool print(const PrintJob& j) {
    auto c = printCommand(j);
    if (c.empty() || os::findExecutable(c[0]).empty() || !os::exists(j.file)) return false;
    bool ok = true;
    const int copies = os::platformName() == "windows" ? std::max(1, j.copies) : 1; // shell verb has no copies option
    for (int i = 0; i < copies && ok; ++i) ok = run(c);
    return ok;
}
std::vector<std::string> listPrintersCommand(std::string_view p) {
    p = plat(p);
    if (unixLike(p) || p == "macos") return {"lpstat", "-e"};
    if (p == "windows") return {"powershell", "-NoProfile", "-NonInteractive", "-Command", "Get-Printer | ForEach-Object Name"};
    return {};
}
std::vector<std::string> printers() {
    auto c = listPrintersCommand();
    std::vector<std::string> out;
    if (c.empty() || os::findExecutable(c[0]).empty()) return out;
    auto r = os::runProcess(std::move(c), {}, 10000);
    if (!r || r->exitCode != 0) return out;
    std::size_t a = 0;
    while (a < r->out.size()) {
        auto b = r->out.find('\n', a);
        if (b == std::string::npos) b = r->out.size();
        std::string l = r->out.substr(a, b - a);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) out.push_back(l);
        a = b + 1;
    }
    return out;
}

} // namespace fuse::ui::desktop
