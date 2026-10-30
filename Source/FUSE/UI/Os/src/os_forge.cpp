// fuse::ui::os on FORGE (forge::os). Everything OS-specific here is FORGE's; this file only adapts signatures.
#include "fuse/ui/os.hpp"

#include <forge/os/clipboard.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

namespace fuse::ui::os {
namespace fo = forge::os;

std::string env(std::string_view name, std::string_view fallback) {
    std::string v = fo::getenv(std::string(name));
    return v.empty() ? std::string(fallback) : v;
}

std::string configDir(std::string_view app) {
    const std::string a(app);
    if constexpr (fo::current() == fo::platform::windows) {
        return env("APPDATA", ".") + "\\" + a;
    } else if constexpr (fo::current() == fo::platform::macos) {
        return env("HOME", ".") + "/Library/Application Support/" + a;
    } else {
        if (std::string x = env("XDG_CONFIG_HOME"); !x.empty()) return x + "/" + a;
        return env("HOME", ".") + "/.config/" + a;
    }
}

std::string runtimeDir() {
    if (std::string r = env("XDG_RUNTIME_DIR"); !r.empty()) return r;
    return fo::temp_dir();
}

std::expected<std::string, std::string> readFile(std::string_view path) {
    auto r = fo::read_file(std::string(path));
    if (!r) return std::unexpected(r.err.what);
    return std::move(r.value);
}

std::expected<void, std::string> writeFileAtomic(std::string_view path, std::string_view data) {
    const std::string p(path);
    if (const std::size_t slash = p.find_last_of("/\\"); slash != std::string::npos && slash > 0)
        if (auto s = fo::fs::make_dirs(p.substr(0, slash)); !s) return std::unexpected(s.err.what);
    if (auto s = fo::fs::write_file_atomic(p, data, 0600); !s) return std::unexpected(s.err.what);
    return {};
}

bool isDirectory(std::string_view path) { return fo::fs::is_dir(std::string(path)); }

double monotonicSeconds() {
    static const std::uint64_t t0 = fo::monotonic_ns();
    return double(fo::monotonic_ns() - t0) * 1e-9;
}
void sleepSeconds(double seconds) {
    if (seconds > 0.0) fo::sleep_ns(std::uint64_t(seconds * 1e9));
}

namespace {
struct ForgeInstanceLock final : InstanceLock {
    fo::fs::file_lock lock;
};
struct ForgeLibrary final : Library {
    fo::library lib;
    void* symbol(const char* name) const override { return lib.symbol(name); }
};
} // namespace

std::unique_ptr<InstanceLock> tryLockInstance(std::string_view name) {
    std::string file = runtimeDir() + "/fuse_app_" + std::string(name);
    if constexpr (fo::current() != fo::platform::windows) file += "_" + env("USER", "user");
    auto r = fo::fs::file_lock::acquire(file + ".lock", /*exclusive=*/true, /*wait=*/false);
    if (!r) return nullptr;
    auto l = std::make_unique<ForgeInstanceLock>();
    l->lock = std::move(r.value);
    return l;
}

std::unique_ptr<Library> loadLibrary(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        auto r = fo::library::open(n);
        if (!r) continue;
        auto l = std::make_unique<ForgeLibrary>();
        l->lib = std::move(r.value);
        return l;
    }
    return nullptr;
}

std::expected<std::string, std::string> clipboardText() {
    auto r = fo::clipboard::get();
    if (!r) return std::unexpected(r.err.what);
    return std::move(r.value);
}
bool setClipboardText(std::string_view text) { return bool(fo::clipboard::set(std::string(text))); }

} // namespace fuse::ui::os
