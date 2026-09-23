#include <fuse/profiler/profiler.hpp>

#include <fuse/platform/thread.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define FUSE_PROFILER_X86_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

namespace fuse::profiler {

bool isValidEventName(const char* name);

namespace {

// ---- Recording model ----------------------------------------------------------------------------
//
// Hot path (FUSE_PROFILE_SCOPE): each thread appends to its own ring (ThreadBuffer) — no locks and
// no shared read-modify-write: one relaxed load of the enabled flag and reset epoch, two raw clock
// reads (invariant TSC on x86-64, steady_clock elsewhere), plain stores of a 48-byte record, and a
// release store of the owner-only write counter. Scope ids come from a per-thread block of the
// global counter. Nothing allocates after a thread's first event (which acquires its buffer).
//
// Read side (eventCount / eventAt / export — not hot): the per-thread rings are merged in record
// order into one view of the latest kRingCapacity events, with ticks converted to nanoseconds.
// The view is rebuilt only when some ring has advanced (or reset() ran) since the last merge.

constexpr u32 kRingCapacity = 4096u;
constexpr u32 kMaxThreadBuffers = 256u;
constexpr u32 kScopeIdBlock = 1024u;

std::atomic<bool> g_enabled{true};
std::atomic<u32> g_frameIndex{0};
std::atomic<u32> g_nextScopeId{1};
std::atomic<u32> g_nextFlowId{1};
std::atomic<u32> g_openAsyncFlowCount{0};
std::atomic<u32> g_maxFlowNestingDepth{0};
/// Bumped by reset(): rings recorded under an older epoch are logically empty.
std::atomic<u32> g_resetEpoch{1};

std::mutex g_exportMutex;

// ---- Clock --------------------------------------------------------------------------------------

u64 steadyNanoseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

#if defined(FUSE_PROFILER_X86_TSC)
bool invariantTscAvailable() {
#if defined(_MSC_VER)
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) {
        return false;
    }
    __cpuid(regs, 0x80000007);
    return (static_cast<unsigned>(regs[3]) & (1u << 8)) != 0u;
#else
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000007u) {
        return false;
    }
    if (__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (edx & (1u << 8)) != 0u;
#endif
}
#endif

/// Tick source, fixed before the first buffer is handed out (see acquireThreadBuffer): the
/// invariant TSC when the CPU has one (a plain `rdtsc`, several times cheaper than a
/// steady_clock read), else steady_clock nanoseconds (1 tick == 1 ns).
bool g_useTsc = false;
u64 g_clockTicks0 = 0;
u64 g_clockNs0 = 0;
f64 g_nsPerTick = 1.0;
bool g_clockCalibrated = false;
std::once_flag g_clockInitOnce;

inline u64 readTicks() {
#if defined(FUSE_PROFILER_X86_TSC)
    if (g_useTsc) {
        return static_cast<u64>(__rdtsc());
    }
#endif
    return steadyNanoseconds();
}

void initClock() {
    std::call_once(g_clockInitOnce, []() {
#if defined(FUSE_PROFILER_X86_TSC)
        g_useTsc = invariantTscAvailable();
#endif
        g_clockNs0 = steadyNanoseconds();
        g_clockTicks0 = readTicks();
        g_clockCalibrated = !g_useTsc;
    });
}

/// Read side only (under g_viewMutex): fix ticks -> ns once, from >= 10 ms of TSC vs steady_clock.
void calibrateClock() {
    initClock();
    if (g_clockCalibrated) {
        return;
    }
    constexpr u64 kCalibrationNs = 10'000'000u;
    u64 ns = steadyNanoseconds();
    if (ns - g_clockNs0 < kCalibrationNs) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(kCalibrationNs - (ns - g_clockNs0)));
    }
    const u64 ticks = readTicks();
    ns = steadyNanoseconds();
    if (ticks > g_clockTicks0 && ns > g_clockNs0) {
        g_nsPerTick = static_cast<f64>(ns - g_clockNs0) / static_cast<f64>(ticks - g_clockTicks0);
    }
    g_clockCalibrated = true;
}

u64 ticksToNs(u64 ticks) {
    if (!g_useTsc) {
        return ticks;
    }
    const f64 delta = (static_cast<f64>(ticks) - static_cast<f64>(g_clockTicks0)) * g_nsPerTick;
    const f64 ns = static_cast<f64>(g_clockNs0) + delta;
    return ns > 0.0 ? static_cast<u64>(ns) : 0u;
}

u64 tickSpanToNs(u64 ticks) {
    return g_useTsc ? static_cast<u64>(static_cast<f64>(ticks) * g_nsPerTick) : ticks;
}

// ---- Per-thread rings ---------------------------------------------------------------------------

/// Compact ring record (48 bytes). `ticks` is the record time (the merge order key); for
/// GPU / CUDA complete events the event timestamp is `ticks - value.durationTicks`.
struct Record {
    const char* name;
    u64 ticks;
    union {
        s64 intValue;
        f64 floatValue;
        u64 durationTicks;
    } value;
    u32 scopeId;
    u32 nestingDepth;
    u32 flowNestingDepth;
    u32 threadId;
    u32 counterSnapshotFrame;
    EventPhase phase;
    CounterValueKind counterKind;
};
static_assert(sizeof(Record) <= 48u, "keep the profiler ring record compact");

struct ThreadBuffer {
    /// Events written since `epoch` began (owner stores with release; readers acquire).
    std::atomic<u64> written{0};
    std::atomic<u32> epoch{0};
    std::atomic<u32> maxNestingDepth{0};
    std::atomic<bool> inUse{false};
    Record records[kRingCapacity];
};

ThreadBuffer* g_threadBuffers[kMaxThreadBuffers] = {};
std::atomic<u32> g_threadBufferCount{0};
std::mutex g_registryMutex;

/// Hot per-thread state: trivially initialised so thread_local access needs no init guard.
struct ThreadState {
    ThreadBuffer* buffer;
    u32 epoch;          ///< reset epoch the ring / id block / max depth belong to
    u32 threadId;
    u32 nextScopeId;
    u32 scopeIdEnd;
    u32 maxNestingDepth;
    u32 nestingDepth;
    u32 flowNestingDepth;
    bool acquireFailed;
};
constinit thread_local ThreadState t_state{};

struct ThreadBufferRelease {
    ~ThreadBufferRelease() {
        if (t_state.buffer != nullptr) {
            // Recorded events stay visible; a later thread may reuse the ring (and append to it).
            // This thread records nothing more (other thread_local destructors may still run).
            ThreadBuffer* buffer = t_state.buffer;
            t_state.buffer = nullptr;
            t_state.acquireFailed = true;
            buffer->inUse.store(false, std::memory_order_release);
        }
    }
};

ThreadBuffer* acquireThreadBuffer() {
    initClock();
    const std::lock_guard<std::mutex> lock(g_registryMutex);
    const u32 count = g_threadBufferCount.load(std::memory_order_relaxed);
    ThreadBuffer* buffer = nullptr;
    for (u32 i = 0; i < count && buffer == nullptr; ++i) {
        bool expected = false;
        if (g_threadBuffers[i]->inUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            buffer = g_threadBuffers[i];
        }
    }
    if (buffer == nullptr && count < kMaxThreadBuffers) {
        buffer = new ThreadBuffer; // records stay untouched until written
        buffer->inUse.store(true, std::memory_order_relaxed);
        g_threadBuffers[count] = buffer;
        g_threadBufferCount.store(count + 1u, std::memory_order_release);
    }
    return buffer;
}

/// Slow path: first event on this thread, or first event after reset().
bool prepareThreadState(ThreadState& state, u32 epoch) {
    if (state.buffer == nullptr) {
        if (state.acquireFailed) {
            return false;
        }
        static thread_local ThreadBufferRelease release;
        (void)release;
        state.buffer = acquireThreadBuffer();
        if (state.buffer == nullptr) {
            state.acquireFailed = true; // more live threads than rings: this thread records nothing
            return false;
        }
        state.threadId = fuse::platform::chromeTraceThreadId();
    }
    if (state.buffer->epoch.load(std::memory_order_relaxed) != epoch) {
        state.buffer->written.store(0u, std::memory_order_relaxed);
        state.buffer->maxNestingDepth.store(0u, std::memory_order_relaxed);
        state.buffer->epoch.store(epoch, std::memory_order_release);
    }
    if (state.epoch != epoch) {
        state.nextScopeId = 0u;
        state.scopeIdEnd = 0u;
        state.maxNestingDepth = 0u;
    }
    state.epoch = epoch;
    return true;
}

/// The calling thread's state with a ring for the current epoch, or null when not recording.
inline ThreadState* recordingThreadState() {
    ThreadState& state = t_state;
    const u32 epoch = g_resetEpoch.load(std::memory_order_relaxed);
    if (state.buffer == nullptr || state.epoch != epoch) [[unlikely]] {
        if (!prepareThreadState(state, epoch)) {
            return nullptr;
        }
    }
    return &state;
}

inline u32 allocateScopeId(ThreadState& state) {
    if (state.nextScopeId == state.scopeIdEnd) [[unlikely]] {
        state.nextScopeId = g_nextScopeId.fetch_add(kScopeIdBlock, std::memory_order_relaxed);
        state.scopeIdEnd = state.nextScopeId + kScopeIdBlock;
    }
    return state.nextScopeId++;
}

inline void appendRecord(ThreadState& state, const Record& record) {
    ThreadBuffer& buffer = *state.buffer;
    const u64 index = buffer.written.load(std::memory_order_relaxed);
    buffer.records[index % kRingCapacity] = record;
    buffer.written.store(index + 1u, std::memory_order_release);
}

inline void appendScopeRecord(ThreadState& state, const char* name, EventPhase phase, u32 scopeId, u32 depth) {
    Record record;
    record.name = name;
    record.ticks = readTicks();
    record.value.durationTicks = 0u;
    record.scopeId = scopeId;
    record.nestingDepth = depth;
    record.flowNestingDepth = 0u;
    record.threadId = state.threadId;
    record.counterSnapshotFrame = 0u;
    record.phase = phase;
    record.counterKind = CounterValueKind::None;
    appendRecord(state, record);
}

u32 currentNestingDepth() {
    return t_state.nestingDepth;
}

u32 pushFlowNestingDepth() {
    const u32 next = ++t_state.flowNestingDepth;
    const u32 observed = g_maxFlowNestingDepth.load(std::memory_order_acquire);
    if (next > observed) {
        g_maxFlowNestingDepth.store(next, std::memory_order_release);
    }
    return next;
}

void popFlowNestingDepth() {
    if (t_state.flowNestingDepth > 0u) {
        --t_state.flowNestingDepth;
    }
}

u32 currentFlowNestingDepth() {
    return t_state.flowNestingDepth;
}

// ---- Merged read view ---------------------------------------------------------------------------

std::mutex g_viewMutex;
std::vector<ProfileEvent> g_view;
u64 g_viewSignature = ~0ull;
u32 g_viewEpoch = 0u;
u32 g_viewBufferCount = 0u;

struct MergeCursor {
    const ThreadBuffer* buffer;
    u64 first; ///< oldest retained index
    u64 next;  ///< one past the next record to take (walking backwards)
};

ProfileEvent toProfileEvent(const Record& record) {
    ProfileEvent event{};
    event.name = record.name;
    event.phase = record.phase;
    event.threadId = record.threadId;
    event.scopeId = record.scopeId;
    event.nestingDepth = record.nestingDepth;
    event.flowNestingDepth = record.flowNestingDepth;
    event.counterKind = record.counterKind;
    event.counterSnapshotFrame = record.counterSnapshotFrame;
    if (record.phase == EventPhase::GpuComplete || record.phase == EventPhase::CudaComplete) {
        event.timestampNs = ticksToNs(record.ticks - record.value.durationTicks);
        event.durationNs = tickSpanToNs(record.value.durationTicks);
    } else {
        event.timestampNs = ticksToNs(record.ticks);
        if (record.counterKind == CounterValueKind::Int) {
            event.counterIntValue = record.value.intValue;
        } else if (record.counterKind == CounterValueKind::Float) {
            event.counterFloatValue = record.value.floatValue;
        }
    }
    return event;
}

/// Rebuild g_view when a ring advanced or reset() ran. Caller holds g_viewMutex.
void refreshViewLocked() {
    const u32 epoch = g_resetEpoch.load(std::memory_order_acquire);
    const u32 bufferCount = g_threadBufferCount.load(std::memory_order_acquire);
    MergeCursor cursors[kMaxThreadBuffers];
    u32 cursorCount = 0u;
    u64 signature = 0u;
    for (u32 i = 0; i < bufferCount; ++i) {
        const ThreadBuffer* buffer = g_threadBuffers[i];
        const u64 written = buffer->written.load(std::memory_order_acquire);
        if (buffer->epoch.load(std::memory_order_acquire) != epoch || written == 0u) {
            continue;
        }
        signature += written;
        const u64 retained = std::min<u64>(written, kRingCapacity);
        cursors[cursorCount++] = MergeCursor{buffer, written - retained, written};
    }
    if (epoch == g_viewEpoch && bufferCount == g_viewBufferCount && signature == g_viewSignature) {
        return;
    }
    g_viewEpoch = epoch;
    g_viewBufferCount = bufferCount;
    g_viewSignature = signature;

    calibrateClock();
    if (g_view.capacity() < kRingCapacity) {
        g_view.reserve(kRingCapacity);
    }
    u64 total = 0u;
    for (u32 i = 0; i < cursorCount; ++i) {
        total += cursors[i].next - cursors[i].first;
    }
    const u32 size = static_cast<u32>(std::min<u64>(total, kRingCapacity));
    g_view.resize(size);
    // Walk backwards taking the latest record across rings (each ring stays in program order), so
    // the view holds the newest `size` events in record order.
    for (u32 out = size; out > 0u; --out) {
        u32 best = kMaxThreadBuffers;
        u64 bestTicks = 0u;
        for (u32 i = 0; i < cursorCount; ++i) {
            const MergeCursor& cursor = cursors[i];
            if (cursor.next == cursor.first) {
                continue;
            }
            const u64 ticks = cursor.buffer->records[(cursor.next - 1u) % kRingCapacity].ticks;
            if (best == kMaxThreadBuffers || ticks > bestTicks) {
                best = i;
                bestTicks = ticks;
            }
        }
        MergeCursor& chosen = cursors[best];
        --chosen.next;
        g_view[out - 1u] = toProfileEvent(chosen.buffer->records[chosen.next % kRingCapacity]);
    }
}

u32 viewSize() {
    const std::lock_guard<std::mutex> lock(g_viewMutex);
    refreshViewLocked();
    return static_cast<u32>(g_view.size());
}

constexpr u32 kMarkerStackCapacity = 32u;

struct PendingMarker {
    const char* name = nullptr;
    u64 startTicks = 0;
    u32 scopeId = 0;
    u32 nestingDepth = 0;
};

struct MarkerStack {
    PendingMarker frames[kMarkerStackCapacity]{};
    u32 depth = 0;

    bool push(const PendingMarker& marker) {
        if (depth >= kMarkerStackCapacity) {
            return false;
        }
        frames[depth++] = marker;
        return true;
    }

    bool pop(PendingMarker& out) {
        if (depth == 0u) {
            return false;
        }
        out = frames[--depth];
        return true;
    }

    void clear() {
        depth = 0u;
    }
};

thread_local MarkerStack g_gpuMarkers{};
thread_local MarkerStack g_cudaMarkers{};

std::string formatCounterArgsJson(const ProfileEvent& event) {
    std::string args = "\"args\":{\"value\":";
    if (event.counterKind == CounterValueKind::Float) {
        char valueBuffer[64];
        std::snprintf(valueBuffer, sizeof(valueBuffer), "%.17g", event.counterFloatValue);
        args += valueBuffer;
    } else {
        args += std::to_string(static_cast<long long>(event.counterIntValue));
    }

    if (event.nestingDepth > 0u) {
        args += ",\"depth\":" + std::to_string(event.nestingDepth);
    }
    if (event.flowNestingDepth > 0u) {
        args += ",\"flow_depth\":" + std::to_string(event.flowNestingDepth);
    }
    if (event.counterSnapshotFrame > 0u) {
        args += ",\"snapshot_at_frame\":" + std::to_string(event.counterSnapshotFrame);
    }
    args += '}';
    return args;
}

std::string escapeJsonString(const char* value) {
    std::string escaped;
    if (value == nullptr) {
        return escaped;
    }

    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(*cursor) < 0x20u) {
                char unicode[8];
                std::snprintf(unicode, sizeof(unicode), "\\u%04x", static_cast<unsigned char>(*cursor));
                escaped += unicode;
            } else {
                escaped.push_back(*cursor);
            }
            break;
        }
    }
    return escaped;
}

void recordEvent(const char* name,
                 EventPhase phase,
                 u32 scopeId,
                 u32 nestingDepth,
                 u32 flowNestingDepth = 0u,
                 CounterValueKind counterKind = CounterValueKind::None,
                 s64 counterIntValue = 0,
                 f64 counterFloatValue = 0.0,
                 u32 counterSnapshotFrame = 0u,
                 u64 durationTicks = 0u) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    ThreadState* state = recordingThreadState();
    if (state == nullptr) {
        return;
    }

    Record record;
    record.name = name;
    record.ticks = readTicks();
    if (counterKind == CounterValueKind::Int) {
        record.value.intValue = counterIntValue;
    } else if (counterKind == CounterValueKind::Float) {
        record.value.floatValue = counterFloatValue;
    } else {
        record.value.durationTicks = durationTicks;
    }
    record.scopeId = scopeId;
    record.nestingDepth = nestingDepth;
    record.flowNestingDepth = flowNestingDepth;
    record.threadId = state->threadId;
    record.counterSnapshotFrame = counterSnapshotFrame;
    record.phase = phase;
    record.counterKind = counterKind;
    appendRecord(*state, record);
}

const char* chromePhaseToken(EventPhase phase) {
    switch (phase) {
    case EventPhase::Begin:
        return "B";
    case EventPhase::End:
        return "E";
    case EventPhase::FlowStart:
        return "s";
    case EventPhase::FlowFinish:
        return "f";
    case EventPhase::Counter:
        return "C";
    case EventPhase::GpuComplete:
    case EventPhase::CudaComplete:
        return "X";
    }
    return "X";
}

const char* chromeCategory(EventPhase phase) {
    switch (phase) {
    case EventPhase::FlowStart:
    case EventPhase::FlowFinish:
        return "async";
    case EventPhase::Counter:
        return "counter";
    case EventPhase::GpuComplete:
        return "gpu";
    case EventPhase::CudaComplete:
        return "cuda";
    default:
        return "cpu";
    }
}

void beginMarker(MarkerStack& stack, const char* name) {
    if (!g_enabled.load(std::memory_order_relaxed) || !isValidEventName(name)) {
        return;
    }

    ThreadState* state = recordingThreadState();
    if (state == nullptr) {
        return;
    }
    PendingMarker marker;
    marker.name = name;
    marker.startTicks = readTicks();
    marker.scopeId = allocateScopeId(*state);
    marker.nestingDepth = currentNestingDepth();
    stack.push(marker);
}

void endMarker(MarkerStack& stack, EventPhase phase) {
    PendingMarker marker;
    if (!stack.pop(marker)) {
        return;
    }

    // recordEvent stamps the end time; the duration places the start (see toProfileEvent).
    const u64 endTicks = readTicks();
    const u64 durationTicks = endTicks >= marker.startTicks ? endTicks - marker.startTicks : 0u;
    recordEvent(marker.name,
                phase,
                marker.scopeId,
                marker.nestingDepth,
                currentFlowNestingDepth(),
                CounterValueKind::None,
                0,
                0.0,
                0u,
                durationTicks);
}

} // namespace

bool isValidEventName(const char* name);

ProfileScope::ProfileScope(const char* name) : m_name(name) {
    // Hot path: no locks, no shared read-modify-write, no allocation (see "Recording model").
    if (!g_enabled.load(std::memory_order_relaxed) || name == nullptr || name[0] == '\0') {
        return;
    }
    ThreadState* state = recordingThreadState();
    if (state == nullptr) {
        return;
    }
    m_active = true;
    m_scopeId = allocateScopeId(*state);
    const u32 depth = ++state->nestingDepth;
    m_nestingDepth = depth;
    if (depth > state->maxNestingDepth) [[unlikely]] {
        state->maxNestingDepth = depth;
        state->buffer->maxNestingDepth.store(depth, std::memory_order_relaxed);
    }
    appendScopeRecord(*state, name, EventPhase::Begin, m_scopeId, depth);
}

ProfileScope::~ProfileScope() {
    if (!m_active) {
        return;
    }
    ThreadState* state = g_enabled.load(std::memory_order_relaxed) ? recordingThreadState() : nullptr;
    if (state != nullptr) {
        appendScopeRecord(*state, m_name, EventPhase::End, m_scopeId, m_nestingDepth);
    }
    if (t_state.nestingDepth > 0u) {
        --t_state.nestingDepth;
    }
}

const char* clockSourceName() {
    initClock();
    return g_useTsc ? "tsc" : "steady_clock";
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void setEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
}

void beginFrame() {
    g_frameIndex.fetch_add(1u, std::memory_order_acq_rel);
}

void endFrame() {
    // Frame boundary marker for future GPU correlation — no-op in CPU stub.
}

u32 frameIndex() {
    return g_frameIndex.load(std::memory_order_acquire);
}

u32 eventCount() {
    return viewSize();
}

u32 ringCapacity() {
    return kRingCapacity;
}

u32 maxNestingDepth() {
    const u32 epoch = g_resetEpoch.load(std::memory_order_acquire);
    const u32 count = g_threadBufferCount.load(std::memory_order_acquire);
    u32 depth = 0u;
    for (u32 i = 0; i < count; ++i) {
        const ThreadBuffer* buffer = g_threadBuffers[i];
        if (buffer->epoch.load(std::memory_order_acquire) == epoch) {
            depth = std::max(depth, buffer->maxNestingDepth.load(std::memory_order_relaxed));
        }
    }
    return depth;
}

u32 nestingDepth() {
    return currentNestingDepth();
}

u32 maxFlowNestingDepth() {
    return g_maxFlowNestingDepth.load(std::memory_order_acquire);
}

u32 scopeNestingDepth() {
    return currentNestingDepth();
}

u32 flowNestingDepth() {
    return currentFlowNestingDepth();
}

u32 openAsyncFlowCount() {
    return g_openAsyncFlowCount.load(std::memory_order_acquire);
}

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;
}

bool isScopeNestingBalanced() {
    return nestingDepth() == 0u;
}

bool isFlowNestingBalanced() {
    return flowNestingDepth() == 0u;
}

bool hasUnbalancedNesting() {
    return !isScopeNestingBalanced() || !isFlowNestingBalanced();
}

bool isFlowDepthDetached() {
    return flowNestingDepth() != openAsyncFlowCount();
}

bool isCrossThreadFlowHandoffPending() {
    return isFlowDepthDetached() && flowNestingDepth() > 0u;
}

NestingAsyncFlowPreflight preflightNestingAndAsyncFlow() {
    NestingAsyncFlowPreflight preflight{};
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.scopeNestingBalanced = isScopeNestingBalanced();
    preflight.flowNestingBalanced = isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
    return preflight;
}

bool hasEvents() {
    return eventCount() > 0u;
}

bool isBufferEmpty() {
    return eventCount() == 0u;
}

bool isBufferFull() {
    return eventCount() >= kRingCapacity;
}

bool isEventIndexValid(u32 index) {
    return index < eventCount();
}

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

bool isValidFlowId(u32 flowId) {
    return flowId != 0u;
}

bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;
}

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return isValidEventName(name) && isValidEventName(event.name)
        && std::strcmp(event.name, name) == 0;
}

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return isValidFlowId(flowId) && isFlowPhaseEvent(event) && event.scopeId == flowId
        && isValidEventName(event.name);
}

bool isValidProfileEvent(const ProfileEvent& event) {
    return isValidEventName(event.name);
}

bool isProfileEventSentinel(const ProfileEvent& event) {
    return event.name == nullptr && event.timestampNs == 0u && event.scopeId == 0u
        && event.phase == EventPhase::Begin;
}

u32 invalidNameEventCount() {
    const u32 total = eventCount();
    const u32 exportable = exportableEventCount();
    return total >= exportable ? total - exportable : 0u;
}

bool hasInvalidNameEvents() {
    return invalidNameEventCount() > 0u;
}

u32 exportableEventCount() {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (isValidEventName(eventAt(i).name)) {
            ++count;
        }
    }
    return count;
}

bool isEventExportable(u32 index) {
    return isEventIndexValid(index) && isValidEventName(eventAt(index).name);
}

const ProfileEvent& emptyProfileEvent() {
    static const ProfileEvent kEmpty{};
    return kEmpty;
}

const ProfileEvent& eventAt(u32 index) {
    const std::lock_guard<std::mutex> lock(g_viewMutex);
    refreshViewLocked();
    if (index >= g_view.size()) {
        return emptyProfileEvent();
    }
    return g_view[index];
}

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventIndexValid(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryFirstEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

bool tryLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlowId(flowId);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;
}

u32 findFirstEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && isValidEventName(event.name)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (event.phase == phase && isValidEventName(event.name)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && isValidEventName(event.name)) {
            ++count;
        }
    }
    return count;
}

u32 findFirstEventIndexByName(const char* name) {
    if (!isValidEventName(name)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (eventNameMatches(event, name)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByName(const char* name) {
    if (!isValidEventName(name)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (eventNameMatches(event, name)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByName(const char* name) {
    if (!isValidEventName(name)) {
        return 0u;
    }

    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventNameMatches(eventAt(i), name)) {
            ++count;
        }
    }
    return count;
}

bool hasEventsWithName(const char* name) {
    return findFirstEventIndexByName(name) != kInvalidEventIndex;
}

u32 findFirstEventIndexByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (eventMatchesFlowId(event, flowId)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (eventMatchesFlowId(event, flowId)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
        return 0u;
    }

    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventMatchesFlowId(eventAt(i), flowId)) {
            ++count;
        }
    }
    return count;
}

bool hasEventsWithFlowId(u32 flowId) {
    return findFirstEventIndexByFlowId(flowId) != kInvalidEventIndex;
}

u32 lastEventIndex() {
    const u32 count = eventCount();
    return count > 0u ? count - 1u : kInvalidEventIndex;
}

const ProfileEvent& lastEvent() {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        return emptyProfileEvent();
    }
    return eventAt(index);
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.profilerDisabled = !kChromeTraceExportEnabled || !enabled();
    preflight.eventCount = eventCount();
    preflight.exportableEventCount = exportableEventCount();
    preflight.frameIndex = frameIndex();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.bufferEmpty = isBufferEmpty();
    preflight.scopeNestingUnbalanced = !isScopeNestingBalanced();
    preflight.flowNestingUnbalanced = !isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.invalidNameEventCount = invalidNameEventCount();
    preflight.ringBufferFull = isBufferFull();
    preflight.hasInvalidNameEvents = hasInvalidNameEvents();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
    return preflight;
}

void reset() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);
    // Every ring becomes logically empty; owners clear their ring on their next event.
    g_resetEpoch.fetch_add(1u, std::memory_order_acq_rel);
    g_frameIndex.store(0u, std::memory_order_release);
    g_nextScopeId.store(1u, std::memory_order_release);
    g_nextFlowId.store(1u, std::memory_order_release);
    g_maxFlowNestingDepth.store(0u, std::memory_order_release);
    g_openAsyncFlowCount.store(0u, std::memory_order_release);
    t_state.nestingDepth = 0u;
    t_state.flowNestingDepth = 0u;
    g_gpuMarkers.clear();
    g_cudaMarkers.clear();
}

u32 nextFlowId() {
    return g_nextFlowId.fetch_add(1u, std::memory_order_acq_rel);
}

void beginAsyncFlow(const char* name, u32 flowId) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)) {
        return;
    }

    const u32 flowDepth = pushFlowNestingDepth();
    g_openAsyncFlowCount.fetch_add(1u, std::memory_order_acq_rel);
    recordEvent(name,
                EventPhase::FlowStart,
                flowId,
                currentNestingDepth(),
                flowDepth);
}

void endAsyncFlow(const char* name, u32 flowId) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)) {
        return;
    }

    if (g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u) {
        return;
    }

    g_openAsyncFlowCount.fetch_sub(1u, std::memory_order_acq_rel);

    const u32 flowDepth = currentFlowNestingDepth();
    recordEvent(name,
                EventPhase::FlowFinish,
                flowId,
                currentNestingDepth(),
                flowDepth);
    popFlowNestingDepth();
}

void sampleCounter(const char* track, s64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Int,
                value,
                0.0,
                0u);
}

void sampleCounterFloat(const char* track, f64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Float,
                0,
                value,
                0u);
}

void sampleCounterSnapshotAtFrame(const char* track, s64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Int,
                value,
                0.0,
                frameIndex());
}

void sampleCounterFloatSnapshotAtFrame(const char* track, f64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Float,
                0,
                value,
                frameIndex());
}

void profile_gpu_begin(const char* name, void* cmdBuffer) {
    (void)cmdBuffer;
    beginMarker(g_gpuMarkers, name);
}

void profile_gpu_end(void* cmdBuffer) {
    (void)cmdBuffer;
    endMarker(g_gpuMarkers, EventPhase::GpuComplete);
}

void profile_cuda_begin(const char* name, void* stream) {
    (void)stream;
    beginMarker(g_cudaMarkers, name);
}

void profile_cuda_end(void* stream) {
    (void)stream;
    endMarker(g_cudaMarkers, EventPhase::CudaComplete);
}

std::string exportChromeTraceJson() {
    if constexpr (!kChromeTraceExportEnabled) {
        return "{\"displayTimeUnit\":\"ns\",\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":0},"
               "\"traceEvents\":[]}";
    }

    const std::lock_guard<std::mutex> lock(g_exportMutex);

    char header[192];
    std::snprintf(header,
                  sizeof(header),
                  "{\"displayTimeUnit\":\"ns\",\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":%u},"
                  "\"traceEvents\":[",
                  g_frameIndex.load(std::memory_order_acquire));

    std::string json = header;
    std::vector<ProfileEvent> events;
    {
        // One consistent snapshot, even while other threads keep recording.
        const std::lock_guard<std::mutex> viewLock(g_viewMutex);
        refreshViewLocked();
        events = g_view;
    }
    bool first = true;

    for (const ProfileEvent& event : events) {
        if (!isValidEventName(event.name)) {
            continue;
        }

        const std::string escapedName = escapeJsonString(event.name);
        const char* phase = chromePhaseToken(event.phase);
        const char* category = chromeCategory(event.phase);
        const u64 timestampUs = event.timestampNs / 1000u;

        // Sized for the escaped name so long names are never truncated into invalid JSON.
        std::string bufferStorage(escapedName.size() + 768u, '\0');
        char* buffer = bufferStorage.data();
        const std::size_t bufferSize = bufferStorage.size();
        switch (event.phase) {
        case EventPhase::Begin:
        case EventPhase::End:
            std::snprintf(buffer,
                          bufferSize,
                          "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                          "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                          first ? "" : ",",
                          escapedName.c_str(),
                          category,
                          phase,
                          static_cast<unsigned long long>(timestampUs),
                          event.threadId,
                          event.scopeId,
                          event.nestingDepth);
            break;
        case EventPhase::FlowStart:
            if (event.nestingDepth > 0u && event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u,\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth,
                              event.flowNestingDepth);
            } else if (event.nestingDepth > 0u) {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth);
            } else if (event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"args\":{\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.flowNestingDepth);
            } else {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId);
            }
            break;
        case EventPhase::FlowFinish:
            if (event.nestingDepth > 0u && event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\",\"args\":{\"depth\":%u,\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth,
                              event.flowNestingDepth);
            } else if (event.nestingDepth > 0u) {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\",\"args\":{\"depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth);
            } else if (event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\",\"args\":{\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.flowNestingDepth);
            } else {
                std::snprintf(buffer,
                              bufferSize,
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\"}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId);
            }
            break;
        case EventPhase::Counter: {
            char* counterHeader = buffer;
            std::snprintf(counterHeader,
                          bufferSize,
                          "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                          "\"tid\":%u,",
                          first ? "" : ",",
                          escapedName.c_str(),
                          category,
                          phase,
                          static_cast<unsigned long long>(timestampUs),
                          event.threadId);
            json += counterHeader;
            json += formatCounterArgsJson(event);
            json += '}';
            first = false;
            continue;
        }
        case EventPhase::GpuComplete:
        case EventPhase::CudaComplete:
            std::snprintf(buffer,
                          bufferSize,
                          "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"dur\":%llu,\"pid\":1,"
                          "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                          first ? "" : ",",
                          escapedName.c_str(),
                          category,
                          phase,
                          static_cast<unsigned long long>(timestampUs),
                          static_cast<unsigned long long>(event.durationNs / 1000u),
                          event.threadId,
                          event.scopeId,
                          event.nestingDepth);
            break;
        }
        json += buffer;
        first = false;
    }

    json += "]}";
    return json;
}

} // namespace fuse::profiler
