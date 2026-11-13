// Live reload of .fuseui documents and CSS style sheets on file change (docs/gui/PLAN.md G2).
// File watching is FORGE's (forge::os::file_watcher via fuse::ui::os::createFileWatcher); this class only maps
// changed paths onto Document::reload / StyleEngine::replaceSheet. A bad edit keeps the last good tree / sheet and
// is reported through the error callback, so a typo never blanks the UI.
//
//   fuse::ui::doc::HotReload hot(ui);
//   hot.watchStyle("ui/theme.css");            // appended to ui.styles(), replaced in place on save
//   hot.watchDocument("ui/main.fuseui", *doc); // Document::reload keeps id'd nodes (focus, scroll, state)
//   ... each frame: hot.poll();
#pragma once
#include <fuse/ui/doc/document.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::os { class FileWatcher; }

namespace fuse::ui::doc {

class HotReload {
public:
    using ErrorFn = std::function<void(const std::string& path, const std::string& message)>;
    explicit HotReload(UiContext& ui);
    ~HotReload();
    HotReload(const HotReload&) = delete;
    HotReload& operator=(const HotReload&) = delete;

    /// Loads `path` as a style sheet into ui.styles() and watches it. False when the file can't be read or parsed
    /// (nothing added) or watching isn't available.
    bool watchStyle(const std::string& path);
    /// Watches `path` and reloads `doc` (owned by the caller, must outlive this) when it changes.
    bool watchDocument(const std::string& path, Document& doc);
    void onError(ErrorFn fn) { m_error = std::move(fn); }

    /// Applies pending changes; returns the number of files reloaded. Non-blocking unless `timeoutMs` > 0.
    int poll(int timeoutMs = 0);
    /// Re-reads a watched file now, as if it had changed (also used when the watcher is unavailable).
    bool reloadNow(const std::string& path);
    bool watching() const { return m_watcher != nullptr; }

private:
    struct Entry {
        std::string path;
        Document* doc = nullptr; ///< null: style sheet
        u32 sheet = 0;
    };
    bool apply(Entry& e);
    void fail(const std::string& path, const std::string& msg) { if (m_error) m_error(path, msg); }
    UiContext& m_ui;
    std::unique_ptr<os::FileWatcher> m_watcher;
    std::vector<Entry> m_entries;
    ErrorFn m_error;
};

} // namespace fuse::ui::doc
