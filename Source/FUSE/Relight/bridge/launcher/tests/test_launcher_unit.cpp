// FUSE Relight RL-2.3: launcher unit tests (any platform): anti-cheat table and scans, glob, PE
// machine detection, command-line quoting. Copyright (c) 2026 FUSE contributors (AGPL-3.0).

#include <fuse/relight/bridge/launcher/launcher.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace fuse::relight::bridge::launcher;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                \
    } while (0)

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

void writePe(const fs::path& p, uint16_t machine, bool valid = true) {
    std::vector<char> img(0x100, 0);
    img[0] = 'M';
    img[1] = 'Z';
    const uint32_t off = 0x80;
    std::memcpy(img.data() + 0x3c, &off, 4);
    std::memcpy(img.data() + off, valid ? "PE\0\0" : "XX\0\0", 4);
    std::memcpy(img.data() + off + 4, &machine, 2);
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(img.data(), static_cast<std::streamsize>(img.size()));
}

bool hasProduct(const std::vector<AntiCheatHit>& hits, const char* product) {
    for (const AntiCheatHit& h : hits) {
        if (std::strcmp(h.marker->product, product) == 0) {
            return true;
        }
    }
    return false;
}

void testTable() {
    const auto& m = knownAntiCheatMarkers();
    CHECK(m.size() >= 30);
    std::set<std::string> products;
    std::set<std::string> keys;
    bool wellFormed = true;
    for (const AntiCheatMarker& x : m) {
        wellFormed = wellFormed && x.product[0] && x.pattern[0] && x.note[0];
        products.insert(x.product);
        const bool unique = keys.insert(std::string(toString(x.kind)) + ":" + x.pattern).second;
        CHECK(unique);
    }
    CHECK(wellFormed);
    for (const char* p : {"EasyAntiCheat", "BattlEye", "Riot Vanguard", "FACEIT", "PunkBuster", "nProtect GameGuard",
                          "XIGNCODE3", "EA Anticheat"}) {
        CHECK(products.count(p) == 1);
    }
}

void testGlob() {
    CHECK(globMatch("EasyAntiCheat.exe", "easyanticheat.EXE"));
    CHECK(!globMatch("EasyAntiCheat.exe", "EasyAntiCheat.exe.bak"));
    CHECK(globMatch("*_BE.exe", "Arma3_BE.exe"));
    CHECK(globMatch("*_BE.exe", "_be.exe"));
    CHECK(!globMatch("*_BE.exe", "Arma3.exe"));
    CHECK(globMatch("*", ""));
    CHECK(globMatch("a*b*c", "aXXbYYc"));
    CHECK(!globMatch("a*b*c", "aXXbYY"));
    CHECK(globMatch("*a*", "bab"));
}

void testInstallScan() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / ("rl_launcher_unit_" + std::to_string(std::rand()));
    fs::remove_all(root, ec);

    const fs::path clean = root / "clean" / "game" / "game.exe";
    touch(clean);
    touch(root / "clean" / "game" / "data.pak");
    fs::create_directories(root / "clean" / "game" / "pbx");
    CHECK(scanGameInstall(utf8String(clean)).empty());

    const fs::path eac = root / "eac" / "game.exe";
    touch(eac);
    fs::create_directories(root / "eac" / "EasyAntiCheat");
    CHECK(hasProduct(scanGameInstall(utf8String(eac)), "EasyAntiCheat"));

    const fs::path eacFileNotDir = root / "eacfile" / "game.exe";
    touch(eacFileNotDir);
    touch(root / "eacfile" / "EasyAntiCheat");  // a file named like the directory marker
    CHECK(scanGameInstall(utf8String(eacFileNotDir)).empty());

    const fs::path be = root / "be" / "Game_BE.exe";
    touch(be);
    CHECK(hasProduct(scanGameInstall(utf8String(be)), "BattlEye"));

    const fs::path parent = root / "parent" / "bin" / "game.exe";
    touch(parent);
    fs::create_directories(root / "parent" / "battleye");  // case-insensitive, in the parent
    CHECK(hasProduct(scanGameInstall(utf8String(parent)), "BattlEye"));

    const fs::path pb = root / "pb" / "game.exe";
    touch(pb);
    touch(root / "pb" / "PB" / "PbCl.dll");
    CHECK(hasProduct(scanGameInstall(utf8String(pb)), "PunkBuster"));

    const fs::path grand = root / "grand" / "a" / "b" / "game.exe";
    touch(grand);
    fs::create_directories(root / "grand" / "EasyAntiCheat");  // grandparent: out of scope
    CHECK(scanGameInstall(utf8String(grand)).empty());

    const fs::path boot = root / "boot" / "start_protected_game.exe";
    touch(boot);
    CHECK(hasProduct(scanGameInstall(utf8String(boot)), "EasyAntiCheat"));
    fs::remove_all(root, ec);
}

void testProcessScan() {
    CHECK(scanProcesses({"explorer.exe", "game.exe"}).empty());
    CHECK(hasProduct(scanProcesses({"explorer.exe", "BEService.exe"}), "BattlEye"));
    CHECK(hasProduct(scanProcesses({"C:\\Program Files\\Riot Vanguard\\vgc.exe"}), "Riot Vanguard"));
    CHECK(hasProduct(scanProcesses({"GameMon64.des"}), "nProtect GameGuard"));
    CHECK(!runningProcessNames().empty());
}

void testPe() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / ("rl_launcher_pe_" + std::to_string(std::rand()));
    uint16_t m = 0;
    writePe(root / "x64.exe", kPeMachineAmd64);
    writePe(root / "x86.exe", kPeMachineI386);
    writePe(root / "bad.exe", kPeMachineI386, false);
    touch(root / "text.exe");
    CHECK(readPeMachine(utf8String((root / "x64.exe")), m) && m == kPeMachineAmd64);
    CHECK(readPeMachine(utf8String((root / "x86.exe")), m) && m == kPeMachineI386);
    CHECK(!readPeMachine(utf8String((root / "bad.exe")), m));
    CHECK(!readPeMachine(utf8String((root / "text.exe")), m));
    CHECK(!readPeMachine(utf8String((root / "missing.exe")), m));
    fs::remove_all(root, ec);
}

void testQuote() {
    CHECK(quoteArgument("plain") == "plain");
    CHECK(quoteArgument("") == "\"\"");
    CHECK(quoteArgument("a b") == "\"a b\"");
    CHECK(quoteArgument("C:\\Program Files\\x\\") == "\"C:\\Program Files\\x\\\\\"");
    CHECK(quoteArgument("q\"uote") == "\"q\\\"uote\"");
    CHECK(quoteArgument("C:\\no\\space") == "C:\\no\\space");
}

void testExitCodes() {
    CHECK(exitCodeFor(LaunchStatus::RefusedAntiCheat) == kExitRefusedAntiCheat);
    CHECK(exitCodeFor(LaunchStatus::ArchMismatch) == kExitArchMismatch);
    CHECK(exitCodeFor(LaunchStatus::InjectFailed) == kExitInjectFailed);
    CHECK(exitCodeFor(LaunchStatus::Ok) == 0);
}

}  // namespace

int main() {
    testTable();
    testGlob();
    testInstallScan();
    testProcessScan();
    testPe();
    testQuote();
    testExitCodes();
    std::printf("rl_bridge_launcher_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
