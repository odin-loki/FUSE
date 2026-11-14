#include "fuse/ui/doc/hot_reload.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fuse::ui::doc {

std::expected<std::string, DocError> readTextFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::unexpected(DocError{{}, "cannot open " + path});
    std::string out;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return std::unexpected(DocError{{}, "cannot read " + path});
    return out;
}

namespace {
bool fileStamp(const std::string& path, long long& time, unsigned long long& size) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) return false;
    const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) return false;
    time = static_cast<long long>(t.time_since_epoch().count());
    size = static_cast<unsigned long long>(sz);
    return true;
}
} // namespace

bool DocumentFileWatcher::stamp(long long& time, unsigned long long& size) const { return fileStamp(m_path, time, size); }

void DocumentFileWatcher::setPath(std::string path) {
    m_path = std::move(path);
    m_known = stamp(m_time, m_size);
}

DocumentFileWatcher::Result DocumentFileWatcher::poll(Document& doc, DocError* error) {
    long long time = 0;
    unsigned long long size = 0;
    if (m_path.empty() || !stamp(time, size)) return Result::Missing;
    if (m_known && time == m_time && size == m_size) return Result::Unchanged;
    auto text = readTextFile(m_path);
    if (!text) { // caught mid-write: try again at the next poll
        if (error) *error = text.error();
        return Result::Missing;
    }
    m_known = true; // a broken file is not re-parsed every poll
    m_time = time;
    m_size = size;
    if (auto r = doc.reload(*text); !r) {
        if (error) *error = r.error();
        return Result::Failed;
    }
    ++m_reloads;
    return Result::Reloaded;
}

// ---- style sheets ----------------------------------------------------------------------------------------------------
StyleSheetFileWatcher::Result StyleSheetFileWatcher::load(UiContext& ui, std::string* error) {
    long long time = 0;
    unsigned long long size = 0;
    if (m_path.empty() || !fileStamp(m_path, time, size)) return Result::Missing;
    auto text = readTextFile(m_path);
    if (!text) {
        if (error) *error = text.error().message;
        return Result::Missing;
    }
    m_known = true; // a broken file is not re-parsed every poll
    m_time = time;
    m_size = size;
    auto sheet = StyleSheet::parse(*text);
    if (!sheet) {
        if (error) *error = m_path + ":" + std::to_string(sheet.error().line) + ": " + sheet.error().message;
        return Result::Failed;
    }
    if (ui.styles().hasSheet(m_sheet)) (void)ui.replaceStyleSheet(m_sheet, std::move(*sheet));
    else m_sheet = ui.addStyleSheetTracked(std::move(*sheet));
    ++m_reloads;
    return Result::Reloaded;
}

StyleSheetFileWatcher::Result StyleSheetFileWatcher::poll(UiContext& ui, std::string* error) {
    long long time = 0;
    unsigned long long size = 0;
    if (m_path.empty() || !fileStamp(m_path, time, size)) return Result::Missing;
    if (m_known && time == m_time && size == m_size) return Result::Unchanged;
    return load(ui, error);
}

} // namespace fuse::ui::doc
