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

// FUSE Relight RL-2.1: OS layer of the bridge IPC core (Win32 + POSIX backends).
//
// Changes from upstream (util_sharedmemory, util_semaphore, util_process):
// - Two backends behind one interface: Win32 (named file mappings and semaphores; also what the
//   MinGW builds run under Wine) and POSIX (shm_open + mmap; process-shared sem_t placed inside the
//   shared region). Upstream is Win32 only.
// - create() and open() are separate. Upstream always "created or opened" and zero-filled the
//   mapping if it happened to be first, so a stale mapping with the same name was silently reused.
//   Here create() fails with Result::Exists and the opener validates the layout it maps.
// - No exceptions or process exits on failure: every call returns a Result.
// - PeerProcess replaces RegisterWaitForSingleObject exit callbacks: liveness is polled from inside
//   every blocking wait (see wait.hpp), which bounds peer-death detection by the poll interval and
//   works the same on both backends.
// - Object names are session-scoped by the caller (upstream prefixed a global GUID).
#pragma once

#include <fuse/relight/bridge/ipc/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fuse::relight::bridge::ipc {

uint64_t nowMs() noexcept;  // monotonic milliseconds
uint32_t currentPid() noexcept;
void sleepMs(uint32_t ms) noexcept;
void cpuRelax() noexcept;
std::string selfExecutablePath();

// A named shared-memory object mapped read/write into this process.
class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    SharedMemory(SharedMemory&& o) noexcept;
    SharedMemory& operator=(SharedMemory&& o) noexcept;

    // Creates a new zero-filled object of `bytes`; Result::Exists if the name is taken.
    Result create(const std::string& name, size_t bytes);
    // Maps an existing object; Result::NotFound if it does not exist, Malformed if smaller than
    // minBytes.
    Result open(const std::string& name, size_t minBytes);
    // POSIX: removes the name (the mapping stays valid; the memory is freed with the last
    // mapping). Win32: no-op (the object lives as long as a handle is open).
    void unlinkName() noexcept;
    void reset() noexcept;

    void* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool valid() const noexcept { return data_ != nullptr; }
    const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
    void* data_ = nullptr;
    size_t size_ = 0;
    intptr_t handle_ = 0;  // Win32 HANDLE of the mapping
    bool linked_ = false;  // POSIX: we still own the name
};

// Counting semaphore shared by two processes. POSIX keeps the sem_t in `slot` (kSlotBytes of the
// shared region); Win32 uses a named semaphore and ignores the slot.
class IpcSemaphore {
public:
    static constexpr size_t kSlotBytes = 64;

    IpcSemaphore() = default;
    ~IpcSemaphore();
    IpcSemaphore(const IpcSemaphore&) = delete;
    IpcSemaphore& operator=(const IpcSemaphore&) = delete;

    Result create(void* slot, const std::string& name, uint32_t initial, uint32_t maximum);
    Result open(void* slot, const std::string& name);
    // Success, Timeout or Failure. timeoutMs may be kNoWait; kInfinite is not allowed here (the
    // callers slice their waits to poll peer liveness).
    Result wait(uint32_t timeoutMs) noexcept;
    void post(uint32_t count = 1) noexcept;
    void reset() noexcept;

private:
    void* sem_ = nullptr;   // POSIX sem_t* inside the shared slot
    intptr_t handle_ = 0;   // Win32 HANDLE
    bool creator_ = false;
};

// Liveness of the process at the other end of a session.
class PeerProcess {
public:
    PeerProcess() = default;
    ~PeerProcess();
    PeerProcess(const PeerProcess&) = delete;
    PeerProcess& operator=(const PeerProcess&) = delete;

    bool attach(uint32_t pid);
    // False once the process has exited (including a zombie that was not reaped yet).
    bool alive() noexcept;
    uint32_t pid() const noexcept { return pid_; }
    bool attached() const noexcept { return pid_ != 0; }
    void reset() noexcept;

private:
    uint32_t pid_ = 0;
    intptr_t handle_ = 0;  // Win32 process HANDLE (SYNCHRONIZE)
    std::atomic<bool> dead_ {false};  // alive() may be polled from several blocked threads
};

// A child process (tests spawn their peers with it; the RL-2.3 launcher has its own).
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // args[0] is the executable path.
    Result spawn(const std::vector<std::string>& args);
    uint32_t pid() const noexcept { return pid_; }
    // Waits for exit; Success with *exitCode, or Timeout.
    Result wait(uint32_t timeoutMs, int* exitCode);
    // Abrupt termination (SIGKILL / TerminateProcess): the peer-death tests use it.
    void kill() noexcept;

private:
    uint32_t pid_ = 0;
    intptr_t handle_ = 0;
    bool reaped_ = false;
    int exitCode_ = 0;
};

}  // namespace fuse::relight::bridge::ipc
