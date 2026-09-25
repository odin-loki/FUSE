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

// POSIX backend (Linux-native unit tests; any future dxvk-native host).
#if !defined(_WIN32)

#include <fuse/relight/bridge/ipc/platform.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace fuse::relight::bridge::ipc {

static_assert(sizeof(sem_t) <= IpcSemaphore::kSlotBytes, "sem_t does not fit the shared semaphore slot");

namespace {

std::string shmName(const std::string& name) {
    // One leading slash, no others (POSIX portability rule for shm_open names).
    std::string out = "/frl.";
    for (char c : name) {
        out.push_back(c == '/' ? '_' : c);
    }
    return out;
}

}  // namespace

uint64_t nowMs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

uint32_t currentPid() noexcept { return static_cast<uint32_t>(::getpid()); }

void sleepMs(uint32_t ms) noexcept {
    if (ms == 0) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#endif
}

std::string selfExecutablePath() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    return buf;
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
    const std::string os = shmName(name);
    const int fd = ::shm_open(os.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return errno == EEXIST ? Result::Exists : Result::Failure;
    }
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        ::shm_unlink(os.c_str());
        return Result::Failure;
    }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        ::shm_unlink(os.c_str());
        return Result::Failure;
    }
    // ftruncate zero-fills: the "first process initializes" memset of upstream is implicit.
    name_ = name;
    data_ = p;
    size_ = bytes;
    linked_ = true;
    return Result::Success;
}

Result SharedMemory::open(const std::string& name, size_t minBytes) {
    reset();
    const std::string os = shmName(name);
    const int fd = ::shm_open(os.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        return errno == ENOENT ? Result::NotFound : Result::Failure;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return Result::Failure;
    }
    const size_t bytes = static_cast<size_t>(st.st_size);
    if (bytes < minBytes || bytes == 0) {
        ::close(fd);
        // A creator between shm_open and ftruncate shows size 0: report "not there yet".
        return bytes == 0 ? Result::NotFound : Result::Malformed;
    }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        return Result::Failure;
    }
    name_ = name;
    data_ = p;
    size_ = bytes;
    linked_ = false;
    return Result::Success;
}

void SharedMemory::unlinkName() noexcept {
    if (!name_.empty()) {
        ::shm_unlink(shmName(name_).c_str());
    }
    linked_ = false;
}

void SharedMemory::reset() noexcept {
    if (data_ != nullptr) {
        ::munmap(data_, size_);
    }
    if (linked_) {
        ::shm_unlink(shmName(name_).c_str());
    }
    data_ = nullptr;
    size_ = 0;
    linked_ = false;
    name_.clear();
}

// ---- IpcSemaphore -----------------------------------------------------------------------------

IpcSemaphore::~IpcSemaphore() { reset(); }

Result IpcSemaphore::create(void* slot, const std::string&, uint32_t initial, uint32_t) {
    reset();
    sem_t* s = static_cast<sem_t*>(slot);
    if (::sem_init(s, 1, initial) != 0) {
        return Result::Failure;
    }
    sem_ = s;
    creator_ = true;
    return Result::Success;
}

Result IpcSemaphore::open(void* slot, const std::string&) {
    reset();
    sem_ = slot;
    creator_ = false;
    return Result::Success;
}

Result IpcSemaphore::wait(uint32_t timeoutMs) noexcept {
    sem_t* s = static_cast<sem_t*>(sem_);
    if (s == nullptr) {
        return Result::Failure;
    }
    if (timeoutMs == 0) {
        while (::sem_trywait(s) != 0) {
            if (errno == EAGAIN) {
                return Result::Timeout;
            }
            if (errno != EINTR) {
                return Result::Failure;
            }
        }
        return Result::Success;
    }
    timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += static_cast<time_t>(timeoutMs / 1000);
    ts.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    while (::sem_timedwait(s, &ts) != 0) {
        if (errno == ETIMEDOUT) {
            return Result::Timeout;
        }
        if (errno != EINTR) {
            return Result::Failure;
        }
    }
    return Result::Success;
}

void IpcSemaphore::post(uint32_t count) noexcept {
    sem_t* s = static_cast<sem_t*>(sem_);
    for (uint32_t i = 0; s != nullptr && i < count; ++i) {
        ::sem_post(s);
    }
}

void IpcSemaphore::reset() noexcept {
    // The sem_t lives in shared memory that may still be used by the peer: never sem_destroy it
    // here. The region's owner drops it with the mapping.
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
    pid_ = pid;
    dead_ = false;
    return true;
}

bool PeerProcess::alive() noexcept {
    if (pid_ == 0) {
        return false;
    }
    if (dead_) {
        return false;
    }
    const pid_t p = static_cast<pid_t>(pid_);
    // Our own child: an exited child stays a zombie (kill(pid, 0) succeeds) until reaped. WNOWAIT
    // leaves it for ChildProcess::wait to reap and read the exit code.
    siginfo_t info {};
    if (::waitid(P_PID, static_cast<id_t>(p), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
        if (info.si_pid == p) {
            dead_ = true;
            return false;
        }
        return true;
    }
    if (::kill(p, 0) != 0 && errno == ESRCH) {
        dead_ = true;
        return false;
    }
#if defined(__linux__)
    // Not our child: a zombie of some other parent is still dead to us.
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(p));
    if (FILE* f = std::fopen(path, "r")) {
        char buf[512];
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        const char* close = std::strrchr(buf, ')');
        if (close != nullptr && close[1] == ' ' && (close[2] == 'Z' || close[2] == 'X')) {
            dead_ = true;
            return false;
        }
    }
#endif
    return true;
}

void PeerProcess::reset() noexcept {
    pid_ = 0;
    dead_ = false;
}

// ---- ChildProcess -----------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    if (pid_ != 0 && !reaped_) {
        kill();
        int code = 0;
        wait(5000, &code);
    }
}

Result ChildProcess::spawn(const std::vector<std::string>& args) {
    if (args.empty()) {
        return Result::Failure;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t p = ::fork();
    if (p < 0) {
        return Result::Failure;
    }
    if (p == 0) {
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    pid_ = static_cast<uint32_t>(p);
    reaped_ = false;
    return Result::Success;
}

Result ChildProcess::wait(uint32_t timeoutMs, int* exitCode) {
    if (pid_ == 0) {
        return Result::Failure;
    }
    const uint64_t start = nowMs();
    while (!reaped_) {
        int status = 0;
        const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        if (r == static_cast<pid_t>(pid_)) {
            reaped_ = true;
            exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            break;
        }
        if (r < 0 && errno != EINTR) {
            return Result::Failure;
        }
        if (timeoutMs != kInfinite && nowMs() - start >= timeoutMs) {
            return Result::Timeout;
        }
        sleepMs(1);
    }
    if (exitCode != nullptr) {
        *exitCode = exitCode_;
    }
    return Result::Success;
}

void ChildProcess::kill() noexcept {
    if (pid_ != 0 && !reaped_) {
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
    }
}

}  // namespace fuse::relight::bridge::ipc

#endif  // !_WIN32
