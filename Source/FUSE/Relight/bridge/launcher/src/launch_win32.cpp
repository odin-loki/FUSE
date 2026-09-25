/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/launcher/launcher.cpp@0867d3c (main: working directory, command
// line assembly, CREATE_SUSPENDED + injection + ResumeThread + wait for exit code)

// FUSE Relight RL-2.3: Win32 part of the launcher (see launcher.hpp for what changed).

#include <fuse/relight/bridge/launcher/launcher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>

namespace fuse::relight::bridge::launcher {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return std::string();
    }
    std::string s(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string fullPath(const std::string& p) {
    const std::wstring w = widen(p);
    const DWORD n = ::GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) {
        return p;
    }
    std::wstring out(n, L'\0');
    const DWORD m = ::GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
    out.resize(m);
    return narrow(out.c_str());
}

std::string lastError() {
    const DWORD e = ::GetLastError();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "error %lu", static_cast<unsigned long>(e));
    return buf;
}

// Loads `dll` into the suspended process with a remote LoadLibraryW. kernel32 is mapped at the same
// address in every process of the same bitness, so the local LoadLibraryW address is valid there.
bool injectDll(HANDLE process, const std::string& dll, uint32_t timeoutMs, std::string& message) {
    const std::wstring path = widen(dll);
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        message = "VirtualAllocEx failed (" + lastError() + ")";
        return false;
    }
    bool ok = false;
    if (!::WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr)) {
        message = "WriteProcessMemory failed (" + lastError() + ")";
    } else {
        const FARPROC loadLibrary = ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        auto start = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void (*)(void)>(loadLibrary));
        HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, start, remote, 0, nullptr);
        if (thread == nullptr) {
            message = "CreateRemoteThread failed (" + lastError() + ")";
        } else {
            if (::WaitForSingleObject(thread, timeoutMs) != WAIT_OBJECT_0) {
                message = "LoadLibraryW did not return within the timeout";
            } else {
                DWORD module = 0;  // low 32 bits of the HMODULE: zero means the load failed
                ::GetExitCodeThread(thread, &module);
                ok = module != 0;
                if (!ok) {
                    message = "LoadLibraryW(" + dll + ") failed in the target process";
                }
            }
            ::CloseHandle(thread);
        }
    }
    // A thread that timed out may still use the string: leave it allocated in that case.
    if (ok || message.find("timeout") == std::string::npos) {
        ::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    return ok;
}

}  // namespace

std::vector<std::string> runningProcessNames() {
    std::vector<std::string> names;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return names;
    }
    PROCESSENTRY32W pe {};
    pe.dwSize = sizeof(pe);
    for (BOOL more = ::Process32FirstW(snap, &pe); more; more = ::Process32NextW(snap, &pe)) {
        names.push_back(narrow(pe.szExeFile));
    }
    ::CloseHandle(snap);
    return names;
}

LaunchReport launch(const LaunchSpec& spec, const std::vector<std::string>* processNames) {
    LaunchReport rep;
    // Resolve the executable like upstream (SearchPath with .exe), then its full path.
    std::string exe = spec.exe;
    {
        wchar_t found[MAX_PATH * 4] = {};
        const std::wstring w = widen(spec.exe);
        if (::SearchPathW(nullptr, w.c_str(), L".exe", MAX_PATH * 4, found, nullptr) != 0) {
            exe = narrow(found);
        }
        exe = fullPath(exe);
    }
    if (::GetFileAttributesW(widen(exe).c_str()) == INVALID_FILE_ATTRIBUTES) {
        rep.status = LaunchStatus::NotFound;
        rep.message = exe + ": not found";
        return rep;
    }

    // 1) Anti-cheat: nothing is created when a marker matches.
    rep.antiCheat = scanGameInstall(exe);
    const std::vector<AntiCheatHit> procs = scanProcesses(processNames ? *processNames : runningProcessNames());
    rep.antiCheat.insert(rep.antiCheat.end(), procs.begin(), procs.end());
    if (!rep.antiCheat.empty()) {
        rep.status = LaunchStatus::RefusedAntiCheat;
        rep.message = "refusing to launch " + exe + ": " + rep.antiCheat.front().marker->product + " (" +
                      toString(rep.antiCheat.front().marker->kind) + " " + rep.antiCheat.front().where + ")";
        return rep;
    }

    // 2) Bitness: LoadLibraryW's address is only valid in a process of our own architecture.
    uint16_t machine = 0;
    const uint16_t self = sizeof(void*) == 8 ? kPeMachineAmd64 : kPeMachineI386;
    if (!readPeMachine(exe, machine)) {
        rep.status = LaunchStatus::NotFound;
        rep.message = exe + ": not a PE executable";
        return rep;
    }
    if (machine != self) {
        rep.status = LaunchStatus::ArchMismatch;
        rep.message = exe + (machine == kPeMachineI386 ? ": 32-bit game, use the x86 build of the launcher"
                                                       : ": 64-bit game, use the x64 build of the launcher");
        return rep;
    }

    // 3) Create suspended (upstream flags), inject, resume.
    std::string workdir = spec.workdir;
    if (workdir.empty()) {
        const size_t slash = exe.find_last_of("\\/");
        workdir = slash == std::string::npos ? std::string(".") : exe.substr(0, slash);
    }
    std::string cmdline = quoteArgument(exe);
    for (const std::string& a : spec.args) {
        cmdline += ' ';
        cmdline += quoteArgument(a);
    }
    std::wstring wcmd = widen(cmdline);
    const std::wstring wexe = widen(exe);
    const std::wstring wdir = widen(fullPath(workdir));
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    if (!::CreateProcessW(wexe.c_str(), wcmd.data(), nullptr, nullptr, TRUE, CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED,
                          nullptr, wdir.c_str(), &si, &pi)) {
        rep.status = LaunchStatus::CreateFailed;
        rep.message = "CreateProcess(" + exe + ") failed (" + lastError() + ")";
        return rep;
    }
    rep.pid = pi.dwProcessId;
    for (const std::string& dll : spec.dlls) {
        std::string msg;
        if (!injectDll(pi.hProcess, fullPath(dll), spec.injectTimeoutMs, msg)) {
            // Never run the game half-configured: it would start without Relight or crash later.
            ::TerminateProcess(pi.hProcess, static_cast<UINT>(kExitInjectFailed));
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            rep.status = LaunchStatus::InjectFailed;
            rep.message = msg;
            return rep;
        }
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    if (spec.wait) {
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        rep.exitCode = static_cast<int>(code);
    }
    ::CloseHandle(pi.hProcess);
    rep.status = LaunchStatus::Ok;
    return rep;
}

}  // namespace fuse::relight::bridge::launcher
