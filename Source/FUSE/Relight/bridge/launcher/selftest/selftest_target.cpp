// FUSE Relight RL-2.3: launcher self-test target (a stand-in game). Copyright (c) 2026 FUSE
// contributors (MIT). New code.
//
//   (no args)             write ran.txt next to the exe; exit 0
//   --expect-probe [a b]  exit 0 if the probe DLL was attached before main() and every following
//                         argument round-tripped ("arg with spaces", "q\"uote"); 10/11/12 otherwise
//   --expect-no-probe     exit 0 if the probe DLL is NOT loaded, else 13
//   --exit N              exit N
//   --sleep MS            idle for MS milliseconds (the fake anti-cheat service), no ran.txt
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

void markRan() {
    char path[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    std::string p(path, n);
    p = p.substr(0, p.find_last_of("\\/") + 1) + "ran.txt";
    std::ofstream(p) << "ran\n";
}

}  // namespace

int main(int argc, char** argv) {
    // First thing: was the probe injected while we were suspended?
    HMODULE probe = ::GetModuleHandleA("fuse_relight_launcher_selftest_probe.dll");
    if (argc >= 3 && std::strcmp(argv[1], "--sleep") == 0) {
        ::Sleep(static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10)));
        return 0;
    }
    markRan();
    if (argc >= 2 && std::strcmp(argv[1], "--expect-probe") == 0) {
        if (probe == nullptr) {
            return 10;
        }
        using AttachFn = int (*)();
        auto attached = reinterpret_cast<AttachFn>(
            reinterpret_cast<void (*)(void)>(::GetProcAddress(probe, "fuse_launcher_probe_attached")));
        if (attached == nullptr || attached() != 1) {
            return 11;
        }
        if (argc >= 4 && (std::strcmp(argv[2], "arg with spaces") != 0 || std::strcmp(argv[3], "q\"uote") != 0)) {
            return 12;
        }
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--expect-no-probe") == 0) {
        return probe == nullptr ? 0 : 13;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--exit") == 0) {
        return std::atoi(argv[2]);
    }
    return 0;
}
