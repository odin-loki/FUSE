#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <fuse/platform/profile.hpp>
#include <fuse/platform/thread.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace fuse::platform {

namespace {
std::atomic<ThreadId> g_renderThreadId{0};
std::atomic<ThreadId> g_mainThreadId{0};

ThreadId hashCurrentThread() {
    return static_cast<ThreadId>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

constexpr int clampThreadPriority(int priority) {
    if (priority < -2) {
        return -2;
    }
    if (priority > 2) {
        return 2;
    }
    return priority;
}

#if defined(_WIN32)

// EfficiencyClass is the byte after Flags. Older SDKs still call that byte Reserved[0];
// the two layouts are the same size, so either name is the efficiency class.
template <typename Processor>
unsigned efficiencyClassOf(const Processor& processor) {
    if constexpr (requires { processor.EfficiencyClass; }) {
        return static_cast<unsigned>(processor.EfficiencyClass);
    } else {
        return static_cast<unsigned>(processor.Reserved[0]);
    }
}

u32 countHighestEfficiencyClass(const unsigned char* buffer, DWORD bytes) {
    DWORD offset = 0;
    unsigned maxClass = 0;
    u32 count = 0;
    const DWORD minCore = static_cast<DWORD>(offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) + 2);
    while (offset < bytes) {
        if (bytes - offset < sizeof(DWORD) + sizeof(DWORD)) {
            break;
        }
        const auto* info =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer + offset);
        const DWORD size = info->Size;
        if (size < sizeof(DWORD) + sizeof(DWORD) || size > bytes - offset) {
            break;
        }
        if (info->Relationship == RelationProcessorCore && size >= minCore) {
            const unsigned cls = efficiencyClassOf(info->Processor);
            if (count == 0 || cls > maxClass) {
                maxClass = cls;
                count = 1;
            } else if (cls == maxClass) {
                ++count;
            }
        }
        offset += size;
    }
    return count;
}

// Cores (RelationProcessorCore records) in the highest EfficiencyClass. 0 means the query failed.
u32 windowsHighestEfficiencyClassCoreCount() {
    constexpr DWORD kStackBytes = 4096;
    constexpr DWORD kMaxBytes = 1u << 20;
    alignas(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) unsigned char stack[kStackBytes];
    DWORD bytes = kStackBytes;
    if (GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(stack),
            &bytes)) {
        return countHighestEfficiencyClass(stack, bytes);
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes <= kStackBytes || bytes > kMaxBytes) {
        return 0;
    }

    void* const heap = std::malloc(bytes);
    if (heap == nullptr) {
        return 0;
    }
    const DWORD required = bytes;
    const BOOL ok = GetLogicalProcessorInformationEx(
        RelationProcessorCore, static_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(heap), &bytes);
    u32 count = 0;
    if (ok && bytes > 0 && bytes <= required) {
        count = countHighestEfficiencyClass(static_cast<const unsigned char*>(heap), bytes);
    }
    std::free(heap);
    return count;
}

void applyCurrentThreadPriority(int priority) {
    // THREAD_PRIORITY_{LOWEST,BELOW_NORMAL,NORMAL,ABOVE_NORMAL,HIGHEST} are -2..2.
    static_assert(THREAD_PRIORITY_LOWEST == -2);
    static_assert(THREAD_PRIORITY_BELOW_NORMAL == -1);
    static_assert(THREAD_PRIORITY_NORMAL == 0);
    static_assert(THREAD_PRIORITY_ABOVE_NORMAL == 1);
    static_assert(THREAD_PRIORITY_HIGHEST == 2);
    (void)SetThreadPriority(GetCurrentThread(), priority);
}

#elif !defined(__EMSCRIPTEN__)

bool trySetRangedPolicy(pthread_t self, int policy, int priority) {
    const int minPri = sched_get_priority_min(policy);
    const int maxPri = sched_get_priority_max(policy);
    if (minPri < 0 || maxPri <= minPri) {
        return false;
    }
    sched_param param{};
    param.sched_priority =
        minPri + static_cast<int>((static_cast<long long>(priority + 2) * (maxPri - minPri)) / 4);
    return pthread_setschedparam(self, policy, &param) == 0;
}

void applyNice(int priority) {
    // -2..2 → nice 19, 10, 0, -10, -20. Documented on setThreadPriority.
    static constexpr int kNice[5] = {19, 10, 0, -10, -20};
    (void)setpriority(PRIO_PROCESS, 0, kNice[priority + 2]);
}

void applyCurrentThreadPriority(int priority) {
    const pthread_t self = pthread_self();
    int policy = SCHED_OTHER;
    sched_param current{};
    if (pthread_getschedparam(self, &policy, &current) == 0 &&
        trySetRangedPolicy(self, policy, priority)) {
        return;
    }
    // SCHED_OTHER is the policy an unprivileged process can set. It cannot express -2..2
    // (the only legal sched_priority is 0), so the level is carried by nice.
    sched_param other{};
    const int otherMin = sched_get_priority_min(SCHED_OTHER);
    other.sched_priority = otherMin > 0 ? otherMin : 0;
    (void)pthread_setschedparam(self, SCHED_OTHER, &other);
    applyNice(priority);
}

#else

void applyCurrentThreadPriority(int priority) {
    (void)priority;
}

#endif

} // namespace

u32 getCoreCount() {
    const unsigned int cores = std::thread::hardware_concurrency();
    return cores > 0 ? static_cast<u32>(cores) : 1u;
}

u32 getPerformanceCoreCount() {
    const u32 cores = getCoreCount();
#if defined(_WIN32)
    const u32 perf = windowsHighestEfficiencyClassCoreCount();
    if (perf >= 1u && perf <= cores) {
        return perf;
    }
#endif
    return cores;
}

u32 recommendedFiberStackBytes() {
    return currentJobProfileLimits().fiberStackBytes;
}

ThreadId currentThreadId() {
    return hashCurrentThread();
}

u32 chromeTraceThreadId() {
    return static_cast<u32>(currentThreadId() & 0xFFFFFFFFu);
}

void registerMainThread() {
    g_mainThreadId.store(currentThreadId(), std::memory_order_release);
}

ThreadId mainThreadId() {
    return g_mainThreadId.load(std::memory_order_acquire);
}

bool isMainThread() {
    const ThreadId registered = g_mainThreadId.load(std::memory_order_acquire);
    if (registered == 0) {
        return true;
    }
    return currentThreadId() == registered;
}

void registerRenderThread() {
    g_renderThreadId.store(currentThreadId(), std::memory_order_release);
}

ThreadId renderThread() {
    return g_renderThreadId.load(std::memory_order_acquire);
}

bool isRenderThread() {
    const ThreadId registered = g_renderThreadId.load(std::memory_order_acquire);
    if (registered == 0) {
        return true;
    }
    return currentThreadId() == registered;
}

void setThreadPriority(ThreadId thread, int priority) {
    if (thread != 0 && thread != currentThreadId()) {
        return;
    }
    applyCurrentThreadPriority(clampThreadPriority(priority));
}

} // namespace fuse::platform
