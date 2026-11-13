// Hot reload of `.fuseui` files (plan §4.12): a file-time poll that feeds Document::reload, which keeps the identity
// of nodes with an id (same NodeHandle: focus, scroll, edit state, value and outside connections survive).
//
//   DocumentFileWatcher watch(path);                 // remembers the file's time stamp
//   ... each frame (or on a timer):
//   if (watch.poll(*doc) == DocumentFileWatcher::Result::Reloaded) { /* the tree was rebuilt in place */ }
//
// The poll is one stat() call; an application loop calls it from its frame or from a UiContext timer. Nothing here
// depends on fuse_app.
//
// Style sheets reload the same way (StyleSheetFileWatcher): the sheet is added once and replaced in place
// (UiContext::replaceStyleSheet: same cascade position, the sheet list does not grow); a sheet that does not parse
// keeps the last good one.
//
//   StyleSheetFileWatcher theme("ui/theme.css");
//   theme.load(ui);                                   // adds the sheet
//   ... each frame: theme.poll(ui);                   // replaced in place when the file changed
#pragma once

#include "fuse/ui/doc/document.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace fuse::ui::doc {

/// Reads a whole text file.
std::expected<std::string, DocError> readTextFile(const std::string& path);

class DocumentFileWatcher {
public:
    enum class Result : u8 {
        Unchanged, ///< same time stamp and size as at the last poll
        Reloaded,  ///< the file changed and the document was rebuilt from it
        Failed,    ///< the file changed but does not parse / build: the old tree stays (see `error`); not retried
                   ///< until the file changes again
        Missing,   ///< the file cannot be read right now (being rewritten, deleted): the old tree stays
    };
    DocumentFileWatcher() = default;
    /// Watches `path`; the file's current state counts as already loaded.
    explicit DocumentFileWatcher(std::string path) { setPath(std::move(path)); }
    void setPath(std::string path);
    const std::string& path() const { return m_path; }
    /// Checks the file and reloads `doc` from it when its time stamp or size changed since the last poll.
    Result poll(Document& doc, DocError* error = nullptr);
    /// Forgets the remembered state: the next poll reloads.
    void invalidate() { m_known = false; }
    u32 reloadCount() const { return m_reloads; }

private:
    bool stamp(long long& time, unsigned long long& size) const;
    std::string m_path;
    long long m_time = 0;
    unsigned long long m_size = 0;
    bool m_known = false;
    u32 m_reloads = 0;
};

/// Hot reload of one CSS-subset style sheet file (the gui-framework branch's HotReload::watchStyle, on the same file
/// poll as DocumentFileWatcher).
class StyleSheetFileWatcher {
public:
    using Result = DocumentFileWatcher::Result;
    StyleSheetFileWatcher() = default;
    explicit StyleSheetFileWatcher(std::string path) : m_path(std::move(path)) {}
    void setPath(std::string path) { m_path = std::move(path); m_known = false; }
    const std::string& path() const { return m_path; }
    /// Reads the file and adds its sheet to `ui` (Reloaded), or replaces the one a previous load added. Failed: the
    /// file does not parse (`error` has line and message; nothing is added / the old sheet stays). Missing: unreadable.
    Result load(UiContext& ui, std::string* error = nullptr);
    /// load() when the file's time stamp or size changed since the last load / poll; Unchanged otherwise.
    Result poll(UiContext& ui, std::string* error = nullptr);
    /// UiContext::addStyleSheetTracked handle of the sheet (0 until a load succeeded).
    u32 sheet() const { return m_sheet; }
    u32 reloadCount() const { return m_reloads; }

private:
    std::string m_path;
    long long m_time = 0;
    unsigned long long m_size = 0;
    bool m_known = false;
    u32 m_sheet = 0, m_reloads = 0;
};

} // namespace fuse::ui::doc
