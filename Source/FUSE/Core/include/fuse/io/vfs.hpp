#pragma once

#include <fuse/alloc/stack_allocator.hpp>
#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fuse::io {

/// Logical mount prefix for legacy and game content (WP-04).
enum class MountKind : u8 {
    Game = 0,
    T3DLegacy = 1,
    T2DLegacy = 2,
};

struct MountPoint {
    MountKind kind = MountKind::Game;
    std::string physicalPath;
    std::string virtualPrefix;
};

using LoadId = u64;

struct CompletedLoad {
    LoadId id = 0;
    bool success = false;
    Asset asset;
};

/// I/O lane request priority. The lane always serves the highest non-empty class first and keeps
/// FIFO (submission) order inside a class.
enum class IoPriority : u8 {
    Background = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

inline constexpr u32 kIoPriorityCount = 4u;

/// Per-thread linear decode scratch (a bump arena from fuse/alloc). Every decode job receives the
/// scratch of the worker thread it runs on, already reset; nothing allocated from it survives the
/// job. Warm scratch never touches the heap.
using DecodeScratch = fuse::alloc::StackAllocator;

/// Runs on a JobScheduler worker after the I/O thread finished reading `load.asset.bytes`
/// (only for successful reads). May rewrite `load` in place (e.g. replace bytes with the decoded
/// payload). `scratch` is reset before the call.
using DecodeFn = std::function<void(CompletedLoad& load, DecodeScratch& scratch)>;

/// Receives the finished load (after decode when one was requested). Runs on the I/O thread for
/// plain reads, on the decode worker when a DecodeFn was given, and synchronously on the
/// submitting thread when the virtual path does not resolve. The callee may move the bytes out;
/// bytes it leaves in place are recycled by the lane for a later read (no steady-state heap).
using LoadCallback = std::function<void(CompletedLoad& load)>;

struct LoadRequest {
    IoPriority priority = IoPriority::Normal;
    /// Optional decode stage (JobScheduler job, per-thread DecodeScratch).
    DecodeFn decode;
    /// Optional completion callback. When empty the result goes to the drain queue
    /// (drainCompletedLoads / completedLoadCount / peekCompletedLoadOrder).
    LoadCallback onComplete;
};

enum class CancelResult : u8 {
    /// No live load with this id (never submitted, already delivered and drained, or cancelled).
    NotFound = 0,
    /// The read was still queued and has been dropped; it will never be read or delivered.
    DroppedQueued = 1,
    /// The read or decode is in flight; its result will be discarded (callback never runs).
    DiscardInFlight = 2,
    /// The load had completed and was waiting in the drain queue; it has been removed.
    DiscardedCompleted = 3,
    /// The completion callback is already running; too late to cancel.
    AlreadyDelivered = 4,
};

struct IoLaneConfig {
    /// Dedicated I/O threads owned by the VFS (clamped to [1, 2]).
    u32 ioThreadCount = 1;
    /// Capacity of each worker's DecodeScratch arena.
    u32 decodeScratchBytes = 256u * 1024u;
    /// Bytes the lane may read per frame (advanceFrame starts a new frame). 0 = unlimited. A read
    /// larger than the whole budget is still issued when it is the first read of its frame, so
    /// oversized files cannot starve. Synchronous reads (readFileSync) are exempt.
    u64 frameReadBudgetBytes = 0;
    /// Suspend the queue while fuse::platform power state is Background.
    bool followPowerState = true;
    /// Instrumentation: called on the I/O thread right before a read starts (tests, tracing).
    std::function<void(LoadId)> onReadBegin;
};

struct IoLaneStats {
    u32 ioThreadCount = 0;
    u32 queued = 0;
    /// Reads, decodes and deliveries currently in progress.
    u32 inFlight = 0;
    bool suspended = false;
    /// The next read did not fit the frame budget; the lane waits for advanceFrame.
    bool budgetStalled = false;
    u64 frameIndex = 0;
    u64 bytesReadThisFrame = 0;
    u64 bytesReadTotal = 0;
    u64 readsIssued = 0;
    u64 loadsDelivered = 0;
    u64 cancelledQueued = 0;
    u64 discardedInFlight = 0;
    u64 decodesRun = 0;
};

/// VFS registry and asynchronous I/O lane (WP-04 / UNI-VFS-1).
///
/// Mount table: guarded by a shared_mutex, so resolve() is safe from any thread while mounts
/// change. I/O lane: 1-2 dedicated threads owned by the VFS pull reads from a four-class priority
/// queue, honour a per-frame byte budget, stop taking new reads while suspended (manual suspend()
/// or platform PowerState::Background; in-flight reads finish, queued ones stay), and hand results
/// to a JobScheduler decode job, a caller callback, or the game-thread drain queue. Compute
/// workers never block on file I/O.
class VirtualFileSystem {
public:
    static VirtualFileSystem& instance();

    VirtualFileSystem();
    ~VirtualFileSystem();

    VirtualFileSystem(const VirtualFileSystem&) = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

    // ---- mount table (thread safe) -------------------------------------------------------------

    void mount(MountKind kind, std::string_view physicalPath, std::string_view virtualPrefix);
    /// Removes the first mount with exactly this prefix. Returns false when none matched.
    bool unmount(std::string_view virtualPrefix);
    bool resolve(std::string_view virtualPath, std::string& outPhysical) const;
    u32 mountCount() const;

    // ---- asynchronous loads --------------------------------------------------------------------

    /// Queue a Normal-priority read on the I/O lane; the result goes to the drain queue. When the
    /// path does not resolve a failed completion is queued immediately. Returns the load id.
    LoadId submitLoadAsync(std::string_view virtualPath);
    LoadId submitLoadAsync(std::string_view virtualPath, IoPriority priority);
    /// Full form: priority, optional decode job, optional completion callback.
    LoadId submitLoad(std::string_view virtualPath, LoadRequest request);

    /// Drop a queued read or discard an in-flight / undrained result.
    CancelResult cancel(LoadId id);

    /// Blocking read through the lane (jumps every queue, ignores suspension and the frame
    /// budget). Called from an I/O lane thread it reads inline. Returns false when the path does
    /// not resolve or the read fails.
    bool readFileSync(std::string_view virtualPath, std::vector<u8>& outBytes);

    /// Game thread: move completed worker loads into HandleTable pending publishes.
    u32 drainCompletedLoads(HandleTable<Asset>& table);

    /// Completed loads waiting for drain (observable from any thread).
    u32 completedLoadCount() const;

    /// Completed load ids waiting for drain, in FIFO completion order (no drain).
    std::vector<LoadId> peekCompletedLoadOrder() const;

    /// Game thread: inspect the most recent drain batch (tests / logging).
    const std::vector<CompletedLoad>& lastDrainedLoads() const { return m_lastDrained; }

    // ---- lane control --------------------------------------------------------------------------

    /// Stops the I/O threads (in-flight reads finish, queued reads stay queued), applies `config`
    /// and restarts the threads. Must not be called from an I/O lane thread or a lane callback.
    void configureLane(IoLaneConfig config);
    IoLaneConfig laneConfig() const;

    /// Stop taking new reads; in-flight reads complete, queued reads stay queued.
    void suspend();
    void resume();
    /// True while suspended manually or by PowerState::Background.
    bool isSuspended() const;

    /// Start a new frame for the read budget.
    void advanceFrame();
    void setFrameReadBudget(u64 bytesPerFrame);

    /// Wait until nothing is queued or in flight. Returns false on timeout (e.g. suspended or
    /// budget-stalled with queued reads).
    bool waitIdle(u32 timeoutMs);

    IoLaneStats laneStats() const;

    /// Scratch of the calling thread (created on first use with the configured capacity).
    static DecodeScratch& threadDecodeScratch();

    /// Power-state hook (registered with fuse::platform by the constructor).
    void onPowerStateChanged(bool background);

private:
    enum class SlotState : u8 { Free, Queued, Reading, Decoding, Delivering };

    struct Slot {
        u32 index = 0;
        SlotState state = SlotState::Free;
        IoPriority priority = IoPriority::Normal;
        bool bypass = false;
        bool cancelRequested = false;
        std::string physicalPath;
        CompletedLoad load;
        DecodeFn decode;
        LoadCallback onComplete;
    };

    /// Fixed-capacity FIFO of slot indices (grown only when the slot pool grows).
    struct IndexRing {
        std::vector<u32> data;
        u32 head = 0;
        u32 count = 0;

        void grow(u32 capacity);
        void pushBack(u32 value);
        void pushFront(u32 value);
        u32 front() const { return data[head]; }
        u32 popFront();
        bool erase(u32 value);
        bool empty() const { return count == 0u; }
    };

    void pushCompleted(CompletedLoad load);
    LoadId submitInternal(std::string_view virtualPath, LoadRequest& request, bool bypass);
    void deliverFailedResolve(LoadId id, std::string_view virtualPath, LoadRequest& request);

    void ensureLaneStartedLocked();
    void laneThreadMain();
    Slot* pickNextLocked();
    Slot* takeFreeSlotLocked();
    void releaseSlotLocked(Slot* slot);
    Slot* findSlotLocked(LoadId id);
    bool suspendedLocked() const { return m_manualSuspend || (m_config.followPowerState && m_powerSuspend); }
    void discardInFlightLocked(Slot* slot);

    void runDecode(Slot* slot);
    void deliver(Slot* slot);

    mutable std::shared_mutex m_mountMutex;
    std::vector<MountPoint> m_mounts;

    std::atomic<LoadId> m_nextLoadId{1};
    mutable std::mutex m_completedMutex;
    std::vector<CompletedLoad> m_completed;
    std::vector<CompletedLoad> m_lastDrained;

    mutable std::mutex m_laneMutex;
    std::condition_variable m_laneCv;
    std::condition_variable m_idleCv;
    IoLaneConfig m_config;
    std::vector<std::thread> m_threads;
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<u32> m_freeSlots;
    IndexRing m_rings[kIoPriorityCount];
    IndexRing m_syncRing;
    bool m_laneStop = false;
    bool m_manualSuspend = false;
    bool m_powerSuspend = false;
    bool m_budgetStalled = false;
    u32 m_queued = 0;
    u32 m_inFlight = 0;
    u64 m_frameIndex = 0;
    u64 m_frameBytes = 0;
    IoLaneStats m_counters;
};

} // namespace fuse::io
