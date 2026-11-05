// FUSE Remaster W0.2: the content-addressed blob store (see store.hpp).
#include <fuse/remaster/store.hpp>

#include <algorithm>
#include <fstream>

namespace fuse::remaster::store {

namespace fs = std::filesystem;

namespace {
bool setError(std::string* error, const std::string& msg) {
    if (error) {
        *error = msg;
    }
    return false;
}
} // namespace

bool writeFileAtomic(const fs::path& path, std::span<const std::uint8_t> bytes, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return setError(error, "cannot write " + tmp.string());
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec); // Windows: rename onto an existing file can fail on some filesystems
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return setError(error, "cannot rename onto " + path.string());
        }
    }
    return true;
}

bool writeFileAtomic(const fs::path& path, std::string_view text, std::string* error) {
    return writeFileAtomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error);
}

std::optional<std::vector<std::uint8_t>> readFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::optional<std::string> readFileText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

fs::path BlobStore::blobPath(const std::string& sha256) const {
    return m_root / "blobs" / "sha256" / sha256.substr(0, 2) / sha256;
}

std::optional<poco::BlobRef> BlobStore::put(std::span<const std::uint8_t> bytes, const std::string& media, std::string* error) {
    poco::BlobRef ref{sha256Hex(bytes.data(), bytes.size()), bytes.size(), media};
    const fs::path p = blobPath(ref.sha256);
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && fs::file_size(p, ec) == bytes.size()) {
        return ref; // immutable: the address is the content
    }
    if (!writeFileAtomic(p, bytes, error)) {
        return std::nullopt;
    }
    return ref;
}

bool BlobStore::has(const std::string& sha256) const {
    std::error_code ec;
    return isSha256Hex(sha256) && fs::is_regular_file(blobPath(sha256), ec);
}

std::optional<std::vector<std::uint8_t>> BlobStore::get(const std::string& sha256) const {
    if (!isSha256Hex(sha256)) {
        return std::nullopt;
    }
    return readFileBytes(blobPath(sha256));
}

bool BlobStore::verify(const poco::BlobRef& ref, std::string* error) const {
    if (!isSha256Hex(ref.sha256)) {
        return setError(error, "malformed sha256 '" + ref.sha256 + "'");
    }
    const auto bytes = get(ref.sha256);
    if (!bytes) {
        return setError(error, "missing blob " + ref.sha256);
    }
    if (bytes->size() != ref.size) {
        return setError(error, "blob " + ref.sha256 + " has " + std::to_string(bytes->size()) + " bytes, the reference says " +
                                   std::to_string(ref.size));
    }
    if (sha256Hex(bytes->data(), bytes->size()) != ref.sha256) {
        return setError(error, "blob " + ref.sha256 + " does not hash to its address");
    }
    return true;
}

std::vector<std::string> BlobStore::list() const {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = m_root / "blobs" / "sha256";
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file() && isSha256Hex(name)) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool BlobStore::writeRecord(const poco::Record& record, std::string* error) const {
    const auto errs = poco::validate(record);
    if (!errs.empty()) {
        return setError(error, record.id + ": " + errs.front());
    }
    return writeFileAtomic(m_root / poco::recordPath(record), poco::toJsonText(record), error);
}

std::optional<poco::Record> BlobStore::readRecord(const std::string& kind, const std::string& id, std::string* error) const {
    const auto text = readFileText(m_root / "poco" / kind / (id + ".poco.json"));
    if (!text) {
        setError(error, "missing record " + kind + "/" + id);
        return std::nullopt;
    }
    return poco::fromJsonText(*text, error);
}

std::vector<std::string> BlobStore::listRecords() const {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = m_root / "poco";
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().filename().string().ends_with(".poco.json")) {
            out.push_back(fs::relative(e.path(), m_root, ec).generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace fuse::remaster::store
