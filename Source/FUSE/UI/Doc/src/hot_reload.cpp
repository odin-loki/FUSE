#include <fuse/ui/doc/hot_reload.hpp>
#include <fuse/ui/os.hpp>
#include <fuse/ui/ui.hpp>
#include <filesystem>

namespace fuse::ui::doc {
namespace {
std::string canon(const std::string& p) {
    std::string a = os::absolutePath(p);
    return std::filesystem::path(a.empty() ? p : a).lexically_normal().string();
}
bool samePath(const std::string& a, const std::string& b) { return canon(a) == canon(b); }
} // namespace

HotReload::HotReload(UiContext& ui) : m_ui(ui), m_watcher(os::createFileWatcher()) {}
HotReload::~HotReload() = default;

bool HotReload::watchStyle(const std::string& path) {
    std::string css;
    if (!readTextFile(path.c_str(), css)) { fail(path, "cannot read"); return false; }
    auto sheet = StyleSheet::parse(css);
    if (!sheet) { fail(path, sheet.error().message); return false; }
    Entry e{path, nullptr, m_ui.styles().sheetCount()};
    m_ui.styles().addSheet(std::move(*sheet));
    if (m_watcher) m_watcher->add(path);
    m_entries.push_back(std::move(e));
    return m_watcher != nullptr;
}

bool HotReload::watchDocument(const std::string& path, Document& doc) {
    m_entries.push_back({path, &doc, 0});
    return m_watcher && m_watcher->add(path);
}

bool HotReload::apply(Entry& e) {
    std::string text;
    if (!readTextFile(e.path.c_str(), text)) { fail(e.path, "cannot read"); return false; }
    if (e.doc) {
        auto r = e.doc->reload(text);
        if (!r) { fail(e.path, r.error().message); return false; }
        return true;
    }
    auto sheet = StyleSheet::parse(text);
    if (!sheet) { fail(e.path, sheet.error().message); return false; }
    return m_ui.styles().replaceSheet(e.sheet, std::move(*sheet));
}

bool HotReload::reloadNow(const std::string& path) {
    bool any = false;
    for (auto& e : m_entries)
        if (samePath(e.path, path)) any = apply(e) || any;
    return any;
}

static constexpr int kSettleMs = 30;

int HotReload::poll(int timeoutMs) {
    if (!m_watcher) return 0;
    // One save is often several notifications (truncate + write, or temp file + rename; ReadDirectoryChangesW
    // reports each step). Once something changed, keep collecting until the files are quiet for a moment, then
    // reload each changed file once, from its final contents.
    std::vector<std::string> changed = m_watcher->poll(timeoutMs);
    if (changed.empty()) return 0;
    for (int i = 0; i < 20; ++i) {
        auto more = m_watcher->poll(kSettleMs);
        if (more.empty()) break;
        changed.insert(changed.end(), more.begin(), more.end());
    }
    int n = 0;
    for (auto& e : m_entries) {
        bool hit = false;
        for (const auto& c : changed) hit = hit || samePath(e.path, c);
        if (hit && apply(e)) ++n;
    }
    return n;
}

} // namespace fuse::ui::doc
