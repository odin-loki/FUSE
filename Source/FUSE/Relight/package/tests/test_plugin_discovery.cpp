// FUSE Relight RL-6.4: plugin discovery gate (rl_package_plugin_discovery).
//
// Builds mock plugin directories (fake PE DLLs with a VS_FIXEDFILEINFO resource, or soname files) in a
// scratch directory and checks discovery for present / absent / incomplete / wrong-version / unknown-version
// plugins, the search order (per-plugin env dir > FUSE_RELIGHT_PLUGIN_DIR > <module>/fuse_relight_plugins),
// case-insensitive Windows names, the report lines, and that the PE parser survives every truncation and
// byte corruption of a valid image. Optional argv[2]: a real PE whose version resource must parse.
//
//   fuse_relight_plugin_discovery_tests <scratch dir> [<real PE with a version resource>]
#include <fuse/relight/package/plugin_discovery.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace fuse::relight::package;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
        }                                                                                                              \
    } while (0)

void put16(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    b[o] = std::uint8_t(v);
    b[o + 1] = std::uint8_t(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[o + std::size_t(i)] = std::uint8_t(v >> (8 * i));
    }
}

// A minimal PE32+ (or PE32) image: one .rsrc section holding RT_VERSION / id 1 / lang 0x409 -> VS_VERSIONINFO.
std::vector<std::uint8_t> makePe(FileVersion v, bool pe32 = false, bool withVersion = true) {
    const std::size_t optSize = pe32 ? 224 : 240;
    const std::size_t peOff = 0x40;
    const std::size_t opt = peOff + 24;
    const std::size_t secTable = opt + optSize;
    const std::size_t raw = 0x200;
    const std::uint32_t va = 0x1000;
    const std::size_t rsrcSize = 0x100;
    std::vector<std::uint8_t> b(raw + rsrcSize, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, std::uint32_t(peOff));
    put32(b, peOff, 0x00004550u);
    put16(b, peOff + 4, pe32 ? 0x14C : 0x8664);
    put16(b, peOff + 6, 1);
    put16(b, peOff + 20, std::uint32_t(optSize));
    put16(b, opt, pe32 ? 0x10B : 0x20B);
    const std::size_t numRva = opt + (pe32 ? 92 : 108);
    put32(b, numRva, 16);
    if (withVersion) {
        put32(b, numRva + 4 + 16, va);
        put32(b, numRva + 4 + 20, std::uint32_t(rsrcSize));
    }
    std::memcpy(&b[secTable], ".rsrc", 5);
    put32(b, secTable + 8, std::uint32_t(rsrcSize));
    put32(b, secTable + 12, va);
    put32(b, secTable + 16, std::uint32_t(rsrcSize));
    put32(b, secTable + 20, std::uint32_t(raw));
    // Resource tree (offsets relative to the section start).
    put16(b, raw + 14, 1);                     // root: 1 id entry
    put32(b, raw + 16, 16);                    //   RT_VERSION
    put32(b, raw + 20, 0x80000000u | 0x18);    //   -> dir at 0x18
    put16(b, raw + 0x18 + 14, 1);              // level 2
    put32(b, raw + 0x18 + 16, 1);              //   id 1
    put32(b, raw + 0x18 + 20, 0x80000000u | 0x30);
    put16(b, raw + 0x30 + 14, 1);              // level 3
    put32(b, raw + 0x30 + 16, 0x409);          //   lang
    put32(b, raw + 0x30 + 20, 0x48);           //   -> data entry
    put32(b, raw + 0x48, va + 0x58);           // data entry: rva, size
    put32(b, raw + 0x48 + 4, 40 + 52);
    const std::size_t vi = raw + 0x58;         // VS_VERSIONINFO
    put16(b, vi, 40 + 52);
    put16(b, vi + 2, 52);
    const char* key = "VS_VERSION_INFO";
    for (std::size_t i = 0; key[i]; ++i) {
        b[vi + 6 + 2 * i] = std::uint8_t(key[i]);
    }
    const std::size_t ffi = vi + 40;
    put32(b, ffi, 0xFEEF04BDu);
    put32(b, ffi + 4, 0x00010000u);
    put32(b, ffi + 8, (v.major << 16) | v.minor);
    put32(b, ffi + 12, (v.patch << 16) | v.build);
    return b;
}

void writeFile(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
void writeText(const fs::path& p, const std::string& text) {
    writeFile(p, std::vector<std::uint8_t>(text.begin(), text.end()));
}

// Installs a complete Windows DLSS SR set into dir with the given DLSS version.
void installDlss(const fs::path& dir, FileVersion dlss, FileVersion sl = {2, 14, 1, 0}) {
    writeText(dir / "fuse_nvplugin_streamline.dll", "provider (unversioned mock)");
    writeFile(dir / "sl.interposer.dll", makePe(sl));
    writeText(dir / "sl.common.dll", "mock");
    writeText(dir / "sl.dlss.dll", "mock");
    writeFile(dir / "nvngx_dlss.dll", makePe(dlss));
}

const PluginResult& find(const std::vector<PluginResult>& rs, const std::string& id) {
    for (const PluginResult& r : rs) {
        if (r.id == id) {
            return r;
        }
    }
    static PluginResult none;
    std::fprintf(stderr, "no result for %s\n", id.c_str());
    ++g_failures;
    return none;
}

struct Env {
    std::map<std::string, std::string> vars;
    DiscoveryConfig config(PluginPlatform platform, const fs::path& moduleDir) const {
        DiscoveryConfig c;
        c.platform = platform;
        c.moduleDir = moduleDir.string();
        const auto* self = this;
        c.getenv = [self](const char* name) {
            const auto it = self->vars.find(name);
            return it == self->vars.end() ? std::string() : it->second;
        };
        return c;
    }
};

void testPeParser() {
    FileVersion v;
    const std::vector<std::uint8_t> pe = makePe({3, 7, 10, 0});
    CHECK(parsePeFileVersion(pe.data(), pe.size(), v));
    CHECK((v == FileVersion{3, 7, 10, 0}));
    const std::vector<std::uint8_t> pe32 = makePe({1, 3, 0, 1234}, true);
    CHECK(parsePeFileVersion(pe32.data(), pe32.size(), v));
    CHECK((v == FileVersion{1, 3, 0, 1234}));
    const std::vector<std::uint8_t> nores = makePe({1, 0, 0, 0}, false, false);
    CHECK(!parsePeFileVersion(nores.data(), nores.size(), v));
    CHECK(!parsePeFileVersion(nullptr, 0, v));
    const char junk[] = "MZ not really a PE";
    CHECK(!parsePeFileVersion(reinterpret_cast<const std::uint8_t*>(junk), sizeof(junk), v));
    // Every truncation that cuts into the version block must fail cleanly (no out-of-bounds read); the
    // version block ends at 0x200 + 0x58 + 92, the rest of the section is padding.
    int accepted = 0;
    for (std::size_t n = 0; n < 0x200 + 0x58 + 92; ++n) {
        std::vector<std::uint8_t> t(pe.begin(), pe.begin() + std::ptrdiff_t(n));
        accepted += parsePeFileVersion(t.data(), t.size(), v) ? 1 : 0;
    }
    CHECK(accepted == 0);
    // Every single-byte corruption must either parse or fail, never crash.
    for (std::size_t i = 0; i < 0x200 + 0xB0; ++i) {
        std::vector<std::uint8_t> c = pe;
        c[i] ^= 0xFF;
        (void)parsePeFileVersion(c.data(), c.size(), v);
    }
    // Soname versions.
    CHECK(parseSonameVersion("libnvidia-ngx-dlss.so.310.1.0", v) && (v == FileVersion{310, 1, 0, 0}));
    CHECK(parseSonameVersion("libfoo.so.3", v) && (v == FileVersion{3, 0, 0, 0}));
    CHECK(!parseSonameVersion("libfoo.so", v));
    CHECK(!parseSonameVersion("libfoo.so.x1", v));
    CHECK(!parseSonameVersion("libfoo.so.1.2.3.4.5", v));
    CHECK(compareVersions({3, 1, 0, 0}, {3, 0, 9, 9}) > 0 && compareVersions({2, 0, 0, 0}, {2, 0, 0, 0}) == 0);
    CHECK(formatVersion({3, 7, 10, 0}) == "3.7.10" && formatVersion({1, 2, 3, 4}) == "1.2.3.4");
}

void testDiscovery(const fs::path& scratch) {
    const PluginPlatform win = PluginPlatform::Windows;
    // ---- absent: nothing installed anywhere; three directories searched, all named in the log ----
    {
        const fs::path mod = scratch / "absent" / "game";
        fs::create_directories(mod);
        Env env;
        env.vars["FUSE_NVIDIA_SDK_DIR"] = (scratch / "absent" / "nvsdk").string();
        env.vars["FUSE_RELIGHT_PLUGIN_DIR"] = (scratch / "absent" / "root").string();
        const auto rs = discoverPlugins(env.config(win, mod));
        CHECK(rs.size() == defaultPluginSpecs(win).size());
        for (const PluginResult& r : rs) {
            CHECK(r.status == PluginStatus::Absent);
            CHECK(!r.usable());
        }
        const PluginResult& d = find(rs, "dlss");
        CHECK(d.searched.size() == 3);
        CHECK(d.detail.find("not installed") != std::string::npos);
        CHECK(d.detail.find("FUSE_NVIDIA_SDK_DIR") != std::string::npos);
        CHECK(d.detail.find("fuse_relight_plugins") != std::string::npos);
        CHECK(find(rs, "xess").searched.size() == 2); // FUSE_XESS_SDK_DIR unset
        const std::string report = formatDiscoveryReport(rs);
        CHECK(report.find("fuse-relight plugins: dlss: absent: not installed") != std::string::npos);
        CHECK(report.find("fuse-relight plugins: nrd: absent:") != std::string::npos);
        const std::string json = formatDiscoveryJson(rs);
        CHECK(json.find("\"id\":\"reflex\",\"status\":\"absent\",\"usable\":false") != std::string::npos);
    }
    // ---- present in the packaged location (<module>/fuse_relight_plugins/nvidia) ----
    {
        const fs::path mod = scratch / "present" / "game";
        installDlss(mod / "fuse_relight_plugins" / "nvidia", {3, 7, 10, 0});
        writeFile(mod / "fuse_relight_plugins" / "xess" / "LIBXESS.DLL", makePe({1, 3, 1, 0})); // case-insensitive
        Env env;
        const auto rs = discoverPlugins(env.config(win, mod));
        const PluginResult& d = find(rs, "dlss");
        CHECK(d.status == PluginStatus::Available);
        CHECK(d.usable());
        CHECK(d.dir == (mod / "fuse_relight_plugins" / "nvidia").string());
        CHECK(d.detail.find("nvngx_dlss.dll 3.7.10") != std::string::npos);
        CHECK(d.detail.find("sl.interposer.dll 2.14.1") != std::string::npos);
        CHECK(find(rs, "xess").status == PluginStatus::Available);
        // Same directory, but no RR / Reflex files: those report which files are missing.
        const PluginResult& rr = find(rs, "dlss_rr");
        CHECK(rr.status == PluginStatus::Incomplete);
        CHECK(rr.missing == (std::vector<std::string>{"sl.dlss_d.dll", "nvngx_dlssd.dll"}));
        CHECK(rr.detail.find("missing sl.dlss_d.dll, nvngx_dlssd.dll") != std::string::npos);
        CHECK(find(rs, "nrd").status == PluginStatus::Absent);
    }
    // ---- search order: per-plugin env dir > FUSE_RELIGHT_PLUGIN_DIR > module dir ----
    {
        const fs::path base = scratch / "order";
        installDlss(base / "envdir", {3, 8, 0, 0});
        installDlss(base / "root" / "nvidia", {3, 9, 0, 0});
        installDlss(base / "game" / "fuse_relight_plugins" / "nvidia", {3, 10, 0, 0});
        Env env;
        env.vars["FUSE_NVIDIA_SDK_DIR"] = (base / "envdir").string();
        env.vars["FUSE_RELIGHT_PLUGIN_DIR"] = (base / "root").string();
        CHECK(find(discoverPlugins(env.config(win, base / "game")), "dlss").dir == (base / "envdir").string());
        env.vars.erase("FUSE_NVIDIA_SDK_DIR");
        CHECK(find(discoverPlugins(env.config(win, base / "game")), "dlss").dir == (base / "root" / "nvidia").string());
        env.vars.erase("FUSE_RELIGHT_PLUGIN_DIR");
        CHECK(find(discoverPlugins(env.config(win, base / "game")), "dlss").dir ==
              (base / "game" / "fuse_relight_plugins" / "nvidia").string());
        // An env dir that exists but holds nothing of the plugin falls through to the next location.
        env.vars["FUSE_NVIDIA_SDK_DIR"] = (base / "root").string(); // files are in root/nvidia, not root
        CHECK(find(discoverPlugins(env.config(win, base / "game")), "dlss").dir ==
              (base / "game" / "fuse_relight_plugins" / "nvidia").string());
    }
    // ---- wrong versions ----
    {
        const fs::path base = scratch / "version";
        installDlss(base / "old" / "nvidia", {2, 4, 0, 0});
        installDlss(base / "newsl" / "nvidia", {3, 7, 0, 0}, {3, 0, 0, 0});
        writeFile(base / "old" / "xess" / "libxess.dll", makePe({0, 9, 0, 0}));
        writeFile(base / "newsl" / "xess" / "libxess.dll", makePe({3, 0, 0, 0}));
        writeText(base / "unknown" / "nrd" / "fuse_nrdplugin_nri.dll", "mock");
        writeText(base / "unknown" / "nrd" / "NRD.dll", "not a PE: no version resource");
        writeText(base / "badnrd" / "nrd" / "fuse_nrdplugin_nri.dll", "mock");
        writeFile(base / "badnrd" / "nrd" / "NRD.dll", makePe({5, 0, 0, 0}));
        Env env;
        env.vars["FUSE_RELIGHT_PLUGIN_DIR"] = (base / "old").string();
        auto rs = discoverPlugins(env.config(win, {}));
        const PluginResult& d = find(rs, "dlss");
        CHECK(d.status == PluginStatus::VersionTooOld);
        CHECK(!d.usable());
        CHECK(d.detail.find("nvngx_dlss.dll 2.4.0 is older than the minimum 3.1.0") != std::string::npos);
        CHECK(find(rs, "xess").status == PluginStatus::VersionTooOld);
        env.vars["FUSE_RELIGHT_PLUGIN_DIR"] = (base / "newsl").string();
        rs = discoverPlugins(env.config(win, {}));
        CHECK(find(rs, "dlss").status == PluginStatus::VersionTooNew);
        CHECK(find(rs, "dlss").detail.find("sl.interposer.dll 3.0.0 is newer than supported (major <= 2)") != std::string::npos);
        CHECK(find(rs, "xess").status == PluginStatus::VersionTooNew);
        env.vars["FUSE_RELIGHT_PLUGIN_DIR"] = (base / "unknown").string();
        rs = discoverPlugins(env.config(win, {}));
        CHECK(find(rs, "nrd").status == PluginStatus::VersionUnknown);
        CHECK(find(rs, "nrd").usable());
        CHECK(find(rs, "nrd").detail.find("no version information in NRD.dll") != std::string::npos);
        env.vars["FUSE_RELIGHT_PLUGIN_DIR"] = (base / "badnrd").string();
        CHECK(find(discoverPlugins(env.config(win, {})), "nrd").status == PluginStatus::VersionTooNew);
        // No module dir and no env: nothing searched, still a clear line.
        Env none;
        rs = discoverPlugins(none.config(win, {}));
        CHECK(find(rs, "dlss").searched.empty());
        CHECK(find(rs, "dlss").detail.find("no directory configured") != std::string::npos);
    }
    // ---- Linux naming: versioned sonames, highest version wins ----
    {
        const fs::path dir = scratch / "linux" / "nvidia";
        writeText(dir / "libfuse_nvplugin_ngx.so", "mock");
        writeText(dir / "libnvidia-ngx-dlss.so.3.5.0", "mock");
        writeText(dir / "libnvidia-ngx-dlss.so.310.1.0", "mock");
        writeText(dir / "libnvidia-ngx-dlssd.so.2.0.0", "mock");
        Env env;
        env.vars["FUSE_NVIDIA_SDK_DIR"] = dir.string();
        const auto rs = discoverPlugins(env.config(PluginPlatform::Linux, {}));
        const PluginResult& d = find(rs, "dlss");
        CHECK(d.status == PluginStatus::Available);
        CHECK(d.detail.find("libnvidia-ngx-dlss.so.310.1.0 310.1.0") != std::string::npos);
        CHECK(find(rs, "dlss_rr").status == PluginStatus::VersionTooOld);
        // Linux names are case-sensitive.
        const fs::path up = scratch / "linux_case" / "nvidia";
        writeText(up / "LIBFUSE_NVPLUGIN_NGX.SO", "mock");
        env.vars["FUSE_NVIDIA_SDK_DIR"] = up.string();
        CHECK(find(discoverPlugins(env.config(PluginPlatform::Linux, {})), "dlss").status == PluginStatus::Absent);
    }
}

} // namespace

int main(int argc, char** argv) {
    const fs::path scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "rl_plugin_discovery";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    testPeParser();
    testDiscovery(scratch);
    if (argc > 2) {
        FileVersion v;
        CHECK(readPeFileVersion(argv[2], v));
        CHECK(v.major != 0 || v.minor != 0 || v.patch != 0);
        std::printf("real PE %s: %s\n", argv[2], formatVersion(v).c_str());
    }
    // The spec table itself: every version rule names one of the plugin's files, ids are unique.
    for (const PluginPlatform p : {PluginPlatform::Windows, PluginPlatform::Linux}) {
        const auto specs = defaultPluginSpecs(p);
        for (std::size_t i = 0; i < specs.size(); ++i) {
            for (std::size_t j = i + 1; j < specs.size(); ++j) {
                CHECK(specs[i].id != specs[j].id);
            }
            for (const PluginVersionRule& rule : specs[i].versions) {
                bool listed = false;
                for (const std::string& f : specs[i].files) {
                    listed = listed || f == rule.file;
                }
                CHECK(listed);
            }
        }
    }
    if (g_failures == 0) {
        fs::remove_all(scratch, ec);
    }
    std::printf("rl_package_plugin_discovery: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
