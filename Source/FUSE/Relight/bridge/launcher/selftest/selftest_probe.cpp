// FUSE Relight RL-2.3: launcher self-test probe DLL (stands in for the Relight d3d9.dll).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0). New code.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
volatile LONG g_attached = 0;
}

extern "C" __declspec(dllexport) int fuse_launcher_probe_attached() { return static_cast<int>(g_attached); }

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_attached = 1;
    }
    return TRUE;
}
