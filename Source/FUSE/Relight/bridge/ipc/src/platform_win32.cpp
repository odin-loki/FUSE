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
// Ported from dxvk-remix bridge/src/util/{util_sharedmemory,util_semaphore,util_process}.{h,cpp}@0867d3c

// Win32 backend (the shipped x86 client and x64 host; MinGW builds run it under Wine).
#if defined(_WIN32)

#include <fuse/relight/bridge/ipc/platform.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>

#include <utility>

namespace fuse::relight::bridge::ipc {

namespace {

std::string objectName(const std::string& name) {
    // Session-local namespace (upstream used global names prefixed with a GUID).
    std::string out = "Local\\frl.";
    for (char c : name) {
        out.push_back(c == '\\' ? '_' : c);
    }
    return out;
}

HANDLE h(intptr_t v) { return reinterpret_cast<HANDLE>(v); }
intptr_t hv(HANDLE v) { return reinterpret_cast<intptr_t>(v); }

}  // namespace

uint64_t nowMs() noexcept { return static_cast<uint64_t>(::GetTickCount64()); }

uint32_t currentPid() noexcept { return static_cast<uint32_t>(::GetCurrentProcessId()); }

void sleepMs(uint32_t ms) noexcept {
    if (ms == 0) {
        ::SwitchToThread();
    } else {
        ::Sleep(ms);
    }
}

void cpuRelax() noexcept { _mm_pause(); }

std::string selfExecutablePath() {
    char buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return n == 0 ? std::string() : std::string(buf, n);
}

// ---- SharedMemory -----------------------------------------------------------------------------

SharedMemory::~SharedMemory() { reset(); }

SharedMemory::SharedMemory(SharedMemory&& o) noexcept { *this = std::move(o); }

SharedMemory& SharedMemory::operator=(SharedMemory&& o) noexcept {
    if (this != &o) {
        reset();
        name_ = std::move(o.name_);
        data_ = o.data_;
        size_ = o.size_;
        handle_ = o.handle_;
        linked_ = o.linked_;
        o.data_ = nullptr;
        o.size_ = 0;
        o.handle_ = 0;
        o.linked_ = false;
    }
    return *this;
}

Result SharedMemory::create(const std::string& name, size_t bytes) {
    reset();
    const uint64_t size64 = bytes;
    HANDLE m = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size64 >> 32),
                                    static_cast<DWORD>(size64 & 0xFFFFFFFFu), objectName(name).c_str());
    if (m == nullptr) {
        return Result::Failure;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(m);
        return Result::Exists;
    }
    void* p = ::MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (p == nullptr) {
        ::CloseHandle(m);
        return Result::Failure;
    }
    // Pagefile-backed mappings are zero-filled on creation.
    name_ = name;
    data_ = p;
    size_ = bytes;
    handle_ = hv(m);
    return Result::Success;
}

Result SharedMemory::open(const std::string& name, size_t minBytes) {
    reset();
    HANDLE m = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, objectName(name).c_str());
    if (m == nullptr) {
        return ::GetLastError() == ERROR_FILE_NOT_FOUND ? Result::NotFound : Result::Failure;
    }
    void* p = ::MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (p == nullptr) {
        ::CloseHandle(m);
        return Result::Failure;
    }
    // The view's size: sum the regions of this allocation (a single VirtualQuery only reports the
    // first run of pages with identical state, which under Wine can be less than the section).
    size_t bytes = 0;
    for (;;) {
        MEMORY_BASIC_INFORMATION mbi {};
        if (::VirtualQuery(static_cast<uint8_t*>(p) + bytes, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase != p ||
            mbi.RegionSize == 0) {
            break;
        }
        bytes += static_cast<size_t>(mbi.RegionSize);
    }
    if (bytes < minBytes) {
        ::UnmapViewOfFile(p);
        ::CloseHandle(m);
        return Result::Malformed;
    }
    name_ = name;
    data_ = p;
    size_ = bytes;
    handle_ = hv(m);
    return Result::Success;
}

void SharedMemory::unlinkName() noexcept {}

void SharedMemory::reset() noexcept {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
    }
    if (handle_ != 0) {
        ::CloseHandle(h(handle_));
    }
    data_ = nullptr;
    size_ = 0;
    handle_ = 0;
    linked_ = false;
    name_.clear();
}

// ---- IpcSemaphore -----------------------------------------------------------------------------

IpcSemaphore::~IpcSemaphore() { reset(); }

Result IpcSemaphore::create(void*, const std::string& name, uint32_t initial, uint32_t maximum) {
    reset();
    HANDLE s = ::CreateSemaphoreA(nullptr, static_cast<LONG>(initial), static_cast<LONG>(maximum),
                                  objectName(name).c_str());
    if (s == nullptr) {
        return Result::Failure;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(s);
        return Result::Exists;
    }
    handle_ = hv(s);
    creator_ = true;
    return Result::Success;
}

Result IpcSemaphore::open(void*, const std::string& name) {
    reset();
    HANDLE s = ::OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, objectName(name).c_str());
    if (s == nullptr) {
        return ::GetLastError() == ERROR_FILE_NOT_FOUND ? Result::NotFound : Result::Failure;
    }
    handle_ = hv(s);
    creator_ = false;
    return Result::Success;
}

Result IpcSemaphore::wait(uint32_t timeoutMs) noexcept {
    if (handle_ == 0) {
        return Result::Failure;
    }
    const DWORD r = ::WaitForSingleObject(h(handle_), timeoutMs);
    if (r == WAIT_OBJECT_0) {
        return Result::Success;
    }
    return r == WAIT_TIMEOUT ? Result::Timeout : Result::Failure;
}

void IpcSemaphore::post(uint32_t count) noexcept {
    if (handle_ != 0 && count != 0) {
        ::ReleaseSemaphore(h(handle_), static_cast<LONG>(count), nullptr);
    }
}

void IpcSemaphore::reset() noexcept {
    if (handle_ != 0) {
        ::CloseHandle(h(handle_));
    }
    handle_ = 0;
    sem_ = nullptr;
    creator_ = false;
}

// ---- PeerProcess ------------------------------------------------------------------------------

PeerProcess::~PeerProcess() { reset(); }

bool PeerProcess::attach(uint32_t pid) {
    reset();
    if (pid == 0) {
        return false;
    }
    HANDLE p = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    pid_ = pid;
    if (p == nullptr) {
        // The process is already gone (or inaccessible, which the bridge treats the same way).
        dead_ = true;
        return true;
    }
    handle_ = hv(p);
    return true;
}

bool PeerProcess::alive() noexcept {
    if (pid_ == 0 || dead_) {
        return false;
    }
    if (::WaitForSingleObject(h(handle_), 0) == WAIT_OBJECT_0) {
        dead_ = true;
        return false;
    }
    return true;
}

void PeerProcess::reset() noexcept {
    if (handle_ != 0) {
        ::CloseHandle(h(handle_));
    }
    handle_ = 0;
    pid_ = 0;
    dead_ = false;
}

// ---- ChildProcess -----------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    if (handle_ != 0) {
        if (!reaped_) {
            kill();
            int code = 0;
            wait(5000, &code);
        }
        ::CloseHandle(h(handle_));
    }
}

Result ChildProcess::spawn(const std::vector<std::string>& args) {
    if (args.empty()) {
        return Result::Failure;
    }
    std::string cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) {
            cmd.push_back(' ');
        }
        cmd.push_back('"');
        cmd += a;
        cmd.push_back('"');
    }
    STARTUPINFOA si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    if (!::CreateProcessA(args[0].c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return Result::Failure;
    }
    ::CloseHandle(pi.hThread);
    handle_ = hv(pi.hProcess);
    pid_ = static_cast<uint32_t>(pi.dwProcessId);
    reaped_ = false;
    return Result::Success;
}

Result ChildProcess::wait(uint32_t timeoutMs, int* exitCode) {
    if (handle_ == 0) {
        return Result::Failure;
    }
    if (!reaped_) {
        const DWORD r = ::WaitForSingleObject(h(handle_), timeoutMs == kInfinite ? INFINITE : timeoutMs);
        if (r == WAIT_TIMEOUT) {
            return Result::Timeout;
        }
        if (r != WAIT_OBJECT_0) {
            return Result::Failure;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(h(handle_), &code);
        exitCode_ = static_cast<int>(code);
        reaped_ = true;
    }
    if (exitCode != nullptr) {
        *exitCode = exitCode_;
    }
    return Result::Success;
}

void ChildProcess::kill() noexcept {
    if (handle_ != 0 && !reaped_) {
        ::TerminateProcess(h(handle_), 137);
    }
}

}  // namespace fuse::relight::bridge::ipc

#endif  // _WIN32
