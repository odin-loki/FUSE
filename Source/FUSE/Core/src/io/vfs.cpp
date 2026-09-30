#include <fuse/io/vfs.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/platform/power.hpp>

#include <chrono>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fuse::io {
namespace {

/// Slot byte buffers above this capacity are released after delivery instead of recycled.
constexpr usize kRetainedSlotBytes = 8u * 1024u * 1024u;

/// Heap-free (once warm) whole-file reader: raw OS handles, no stream buffers.
class RawFile {
public:
    RawFile() = default;
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;
    ~RawFile() { close(); }

#if defined(_WIN32)
    bool open(const std::string& path) {
        close();
        if (path.empty()) {
            return false;
        }
        thread_local std::wstring wide;
        const int length = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
        if (length <= 0) {
            return false;
        }
        wide.resize(static_cast<usize>(length));
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(), length);
        m_handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }

    bool size(u64& outSize) const {
        LARGE_INTEGER value{};
        if (m_handle == INVALID_HANDLE_VALUE || GetFileSizeEx(m_handle, &value) == 0 || value.QuadPart < 0) {
            return false;
        }
        outSize = static_cast<u64>(value.QuadPart);
        return true;
    }

    bool readAll(u8* destination, u64 byteCount) {
        u64 done = 0;
        while (done < byteCount) {
            const u64 remaining = byteCount - done;
            const DWORD chunk = static_cast<DWORD>(remaining > (1ull << 30) ? (1ull << 30) : remaining);
            DWORD got = 0;
            if (ReadFile(m_handle, destination + done, chunk, &got, nullptr) == 0 || got == 0) {
                return false;
            }
            done += got;
        }
        return true;
    }

    void close() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    bool open(const std::string& path) {
        close();
        if (path.empty()) {
            return false;
        }
        int flags = O_RDONLY;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        do {
            m_fd = ::open(path.c_str(), flags);
        } while (m_fd < 0 && errno == EINTR);
        return m_fd >= 0;
    }

    bool size(u64& outSize) const {
        struct stat info {};
        if (m_fd < 0 || ::fstat(m_fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
            return false;
        }
        outSize = static_cast<u64>(info.st_size);
        return true;
    }

    bool readAll(u8* destination, u64 byteCount) {
        u64 done = 0;
        while (done < byteCount) {
            const u64 remaining = byteCount - done;
            const usize chunk = static_cast<usize>(remaining > (1ull << 30) ? (1ull << 30) : remaining);
            const ssize_t got = ::read(m_fd, destination + done, chunk);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                return false;
            }
            done += static_cast<u64>(got);
        }
        return true;
    }

    void close() {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd = -1;
#endif
};

/// Reads a whole file into `bytes` (capacity reused). Returns false when it cannot be read.
bool readWholeFile(const std::string& physicalPath, std::vector<u8>& bytes) {
    RawFile file;
    u64 size = 0;
    if (!file.open(physicalPath) || !file.size(size)) {
        bytes.clear();
        return false;
    }
    bytes.resize(static_cast<usize>(size));
    if (size != 0u && !file.readAll(bytes.data(), size)) {
        bytes.clear();
        return false;
    }
    return true;
}

fuse::jobs::JobPriority toJobPriority(IoPriority priority) {
    switch (priority) {
    case IoPriority::Critical:
        return fuse::jobs::JobPriority::Critical;
    case IoPriority::High:
        return fuse::jobs::JobPriority::High;
    case IoPriority::Normal:
        return fuse::jobs::JobPriority::Normal;
    case IoPriority::Background:
        break;
    }
    return fuse::jobs::JobPriority::Low;
}

u32 clampThreadCount(u32 count) {
    return count < 1u ? 1u : (count > 2u ? 2u : count);
}

thread_local bool t_onIoLane = false;

struct ThreadScratch {
    std::unique_ptr<DecodeScratch> scratch;
    u32 capacity = 0;
};
thread_local ThreadScratch t_scratch;
std::atomic<u32> g_decodeScratchBytes{256u * 1024u};

// Live VFS instances receive platform power-state changes through one registered callback.
std::mutex g_registryMutex;
std::vector<VirtualFileSystem*> g_registry;
std::once_flag g_powerHookOnce;

void registerInstance(VirtualFileSystem* vfs) {
    std::call_once(g_powerHookOnce, [] {
        fuse::platform::registerPowerStateCallback([](fuse::platform::PowerState, fuse::platform::PowerState current) {
            const std::lock_guard<std::mutex> lock(g_registryMutex);
            for (VirtualFileSystem* instance : g_registry) {
                instance->onPowerStateChanged(current == fuse::platform::PowerState::Background);
            }
        });
    });
    const std::lock_guard<std::mutex> lock(g_registryMutex);
    g_registry.push_back(vfs);
}

void unregisterInstance(VirtualFileSystem* vfs) {
    const std::lock_guard<std::mutex> lock(g_registryMutex);
    for (usize index = 0; index < g_registry.size(); ++index) {
        if (g_registry[index] == vfs) {
            g_registry.erase(g_registry.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
    }
}

} // namespace

// ---- IndexRing ---------------------------------------------------------------------------------

void VirtualFileSystem::IndexRing::grow(u32 capacity) {
    const u32 current = static_cast<u32>(data.size());
    if (capacity <= current) {
        return;
    }
    u32 next = current < 16u ? 16u : current * 2u;
    if (next < capacity) {
        next = capacity;
    }
    std::vector<u32> grown(next);
    for (u32 index = 0; index < count; ++index) {
        grown[index] = data[(head + index) % current];
    }
    data.swap(grown);
    head = 0;
}

void VirtualFileSystem::IndexRing::pushBack(u32 value) {
    const u32 capacity = static_cast<u32>(data.size());
    data[(head + count) % capacity] = value;
    ++count;
}

void VirtualFileSystem::IndexRing::pushFront(u32 value) {
    const u32 capacity = static_cast<u32>(data.size());
    head = (head + capacity - 1u) % capacity;
    data[head] = value;
    ++count;
}

u32 VirtualFileSystem::IndexRing::popFront() {
    const u32 value = data[head];
    head = (head + 1u) % static_cast<u32>(data.size());
    --count;
    return value;
}

bool VirtualFileSystem::IndexRing::erase(u32 value) {
    const u32 capacity = static_cast<u32>(data.size());
    for (u32 index = 0; index < count; ++index) {
        if (data[(head + index) % capacity] != value) {
            continue;
        }
        for (u32 shift = index; shift + 1u < count; ++shift) {
            data[(head + shift) % capacity] = data[(head + shift + 1u) % capacity];
        }
        --count;
        return true;
    }
    return false;
}

// ---- construction ------------------------------------------------------------------------------

VirtualFileSystem& VirtualFileSystem::instance() {
    static VirtualFileSystem vfs;
    return vfs;
}

VirtualFileSystem::VirtualFileSystem() {
    m_powerSuspend = fuse::platform::getPowerState() == fuse::platform::PowerState::Background;
    registerInstance(this);
}

VirtualFileSystem::~VirtualFileSystem() {
    unregisterInstance(this);

    std::vector<std::thread> threads;
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        m_laneStop = true;
        threads.swap(m_threads);
    }
    m_laneCv.notify_all();
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Decode jobs still running on scheduler workers reference their slots; give them a bounded
    // grace period (the scheduler may already be shut down at process exit).
    std::unique_lock<std::mutex> lock(m_laneMutex);
    m_idleCv.wait_for(lock, std::chrono::seconds(5), [this] { return m_inFlight == 0u; });
}

// ---- mount table -------------------------------------------------------------------------------

void VirtualFileSystem::mount(MountKind kind, std::string_view physicalPath, std::string_view virtualPrefix) {
    MountPoint point;
    point.kind = kind;
    point.physicalPath = std::string(physicalPath);
    point.virtualPrefix = std::string(virtualPrefix);
    const std::unique_lock<std::shared_mutex> lock(m_mountMutex);
    m_mounts.push_back(std::move(point));
}

bool VirtualFileSystem::unmount(std::string_view virtualPrefix) {
    const std::unique_lock<std::shared_mutex> lock(m_mountMutex);
    for (usize index = 0; index < m_mounts.size(); ++index) {
        if (m_mounts[index].virtualPrefix == virtualPrefix) {
            m_mounts.erase(m_mounts.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
    }
    return false;
}

bool VirtualFileSystem::resolve(std::string_view virtualPath, std::string& outPhysical) const {
    const std::shared_lock<std::shared_mutex> lock(m_mountMutex);
    for (const MountPoint& point : m_mounts) {
        if (virtualPath.size() < point.virtualPrefix.size()) {
            continue;
        }
        if (virtualPath.substr(0, point.virtualPrefix.size()) != point.virtualPrefix) {
            continue;
        }

        const std::string_view remainder = virtualPath.substr(point.virtualPrefix.size());
        outPhysical = point.physicalPath;
        if (!remainder.empty() && remainder.front() != '/') {
            outPhysical.push_back('/');
        }
        outPhysical.append(remainder);
        return true;
    }
    return false;
}

u32 VirtualFileSystem::mountCount() const {
    const std::shared_lock<std::shared_mutex> lock(m_mountMutex);
    return static_cast<u32>(m_mounts.size());
}

// ---- submission --------------------------------------------------------------------------------

LoadId VirtualFileSystem::submitLoadAsync(std::string_view virtualPath) {
    return submitLoadAsync(virtualPath, IoPriority::Normal);
}

LoadId VirtualFileSystem::submitLoadAsync(std::string_view virtualPath, IoPriority priority) {
    LoadRequest request;
    request.priority = priority;
    return submitInternal(virtualPath, request, false);
}

LoadId VirtualFileSystem::submitLoad(std::string_view virtualPath, LoadRequest request) {
    return submitInternal(virtualPath, request, false);
}

LoadId VirtualFileSystem::submitInternal(std::string_view virtualPath, LoadRequest& request, bool bypass) {
    const LoadId id = m_nextLoadId.fetch_add(1u, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lock(m_laneMutex);
    Slot* slot = takeFreeSlotLocked();
    if (!resolve(virtualPath, slot->physicalPath)) {
        releaseSlotLocked(slot);
        lock.unlock();
        deliverFailedResolve(id, virtualPath, request);
        return id;
    }

    slot->load.id = id;
    slot->load.success = false;
    slot->load.asset.virtualPath.assign(virtualPath.data(), virtualPath.size());
    slot->priority = bypass ? IoPriority::Critical : request.priority;
    slot->bypass = bypass;
    slot->cancelRequested = false;
    slot->decode = std::move(request.decode);
    slot->onComplete = std::move(request.onComplete);
    slot->state = SlotState::Queued;
    if (bypass) {
        m_syncRing.pushBack(slot->index);
    } else {
        m_rings[static_cast<u32>(slot->priority)].pushBack(slot->index);
    }
    ++m_queued;
    ensureLaneStartedLocked();
    lock.unlock();
    m_laneCv.notify_one();
    return id;
}

void VirtualFileSystem::deliverFailedResolve(LoadId id, std::string_view virtualPath, LoadRequest& request) {
    CompletedLoad failed;
    failed.id = id;
    failed.success = false;
    failed.asset.virtualPath = std::string(virtualPath);
    if (request.onComplete) {
        request.onComplete(failed);
        return;
    }
    pushCompleted(std::move(failed));
}

bool VirtualFileSystem::readFileSync(std::string_view virtualPath, std::vector<u8>& outBytes) {
    if (t_onIoLane) {
        // A lane callback reading synchronously must not wait on its own lane.
        thread_local std::string physical;
        if (!resolve(virtualPath, physical)) {
            outBytes.clear();
            return false;
        }
        return readWholeFile(physical, outBytes);
    }

    struct SyncWait {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool success = false;
        std::vector<u8>* out = nullptr;
    } wait;
    wait.out = &outBytes;

    LoadRequest request;
    request.priority = IoPriority::Critical;
    request.onComplete = [&wait](CompletedLoad& load) {
        if (load.success) {
            wait.out->swap(load.asset.bytes);
        } else {
            wait.out->clear();
        }
        const std::lock_guard<std::mutex> lock(wait.mutex);
        wait.success = load.success;
        wait.done = true;
        wait.cv.notify_one();
    };
    submitInternal(virtualPath, request, true);

    std::unique_lock<std::mutex> lock(wait.mutex);
    wait.cv.wait(lock, [&wait] { return wait.done; });
    return wait.success;
}

// ---- cancellation ------------------------------------------------------------------------------

CancelResult VirtualFileSystem::cancel(LoadId id) {
    if (id == 0u) {
        return CancelResult::NotFound;
    }
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        Slot* slot = findSlotLocked(id);
        if (slot != nullptr) {
            switch (slot->state) {
            case SlotState::Queued: {
                const bool erased = slot->bypass ? m_syncRing.erase(slot->index)
                                                 : m_rings[static_cast<u32>(slot->priority)].erase(slot->index);
                (void)erased;
                --m_queued;
                ++m_counters.cancelledQueued;
                releaseSlotLocked(slot);
                m_idleCv.notify_all();
                return CancelResult::DroppedQueued;
            }
            case SlotState::Reading:
            case SlotState::Decoding:
                slot->cancelRequested = true;
                return CancelResult::DiscardInFlight;
            case SlotState::Delivering:
                return CancelResult::AlreadyDelivered;
            case SlotState::Free:
                break;
            }
        }
    }

    const std::lock_guard<std::mutex> lock(m_completedMutex);
    for (usize index = 0; index < m_completed.size(); ++index) {
        if (m_completed[index].id == id) {
            m_completed.erase(m_completed.begin() + static_cast<std::ptrdiff_t>(index));
            return CancelResult::DiscardedCompleted;
        }
    }
    return CancelResult::NotFound;
}

// ---- slot pool ---------------------------------------------------------------------------------

VirtualFileSystem::Slot* VirtualFileSystem::takeFreeSlotLocked() {
    if (m_freeSlots.empty()) {
        auto slot = std::make_unique<Slot>();
        slot->index = static_cast<u32>(m_slots.size());
        m_slots.push_back(std::move(slot));
        const u32 capacity = static_cast<u32>(m_slots.size());
        m_freeSlots.reserve(capacity);
        for (IndexRing& ring : m_rings) {
            ring.grow(capacity);
        }
        m_syncRing.grow(capacity);
        return m_slots.back().get();
    }
    const u32 index = m_freeSlots.back();
    m_freeSlots.pop_back();
    return m_slots[index].get();
}

void VirtualFileSystem::releaseSlotLocked(Slot* slot) {
    slot->state = SlotState::Free;
    slot->bypass = false;
    slot->cancelRequested = false;
    slot->decode = nullptr;
    slot->onComplete = nullptr;
    slot->load.id = 0;
    slot->load.success = false;
    if (slot->load.asset.bytes.capacity() > kRetainedSlotBytes) {
        std::vector<u8>().swap(slot->load.asset.bytes);
    } else {
        slot->load.asset.bytes.clear();
    }
    m_freeSlots.push_back(slot->index);
}

VirtualFileSystem::Slot* VirtualFileSystem::findSlotLocked(LoadId id) {
    for (const std::unique_ptr<Slot>& slot : m_slots) {
        if (slot->state != SlotState::Free && slot->load.id == id) {
            return slot.get();
        }
    }
    return nullptr;
}

// ---- lane threads ------------------------------------------------------------------------------

void VirtualFileSystem::ensureLaneStartedLocked() {
    if (!m_threads.empty() || m_laneStop) {
        return;
    }
    const u32 count = clampThreadCount(m_config.ioThreadCount);
    m_threads.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        m_threads.emplace_back([this] { laneThreadMain(); });
    }
}

VirtualFileSystem::Slot* VirtualFileSystem::pickNextLocked() {
    if (!m_syncRing.empty()) {
        return m_slots[m_syncRing.popFront()].get();
    }
    if (suspendedLocked() || m_budgetStalled) {
        return nullptr;
    }
    for (u32 priority = kIoPriorityCount; priority-- > 0u;) {
        if (!m_rings[priority].empty()) {
            return m_slots[m_rings[priority].popFront()].get();
        }
    }
    return nullptr;
}

void VirtualFileSystem::discardInFlightLocked(Slot* slot) {
    ++m_counters.discardedInFlight;
    releaseSlotLocked(slot);
    --m_inFlight;
    m_idleCv.notify_all();
}

void VirtualFileSystem::laneThreadMain() {
    t_onIoLane = true;
    std::unique_lock<std::mutex> lock(m_laneMutex);
    for (;;) {
        Slot* slot = nullptr;
        m_laneCv.wait(lock, [&] {
            if (m_laneStop) {
                return true;
            }
            slot = pickNextLocked();
            return slot != nullptr;
        });
        if (slot == nullptr) {
            break; // stop requested
        }

        slot->state = SlotState::Reading;
        --m_queued;
        ++m_inFlight;
        const LoadId id = slot->load.id;
        lock.unlock();

        // m_config only changes while the lane threads are stopped.
        if (m_config.onReadBegin) {
            m_config.onReadBegin(id);
        }

        RawFile file;
        u64 size = 0;
        const bool opened = file.open(slot->physicalPath) && file.size(size);

        lock.lock();
        if (slot->cancelRequested) {
            discardInFlightLocked(slot);
            continue;
        }
        const u64 budget = m_config.frameReadBudgetBytes;
        if (opened && !slot->bypass && budget != 0u && m_frameBytes != 0u && m_frameBytes + size > budget) {
            // Does not fit this frame: back to the head of its class until advanceFrame().
            m_rings[static_cast<u32>(slot->priority)].pushFront(slot->index);
            slot->state = SlotState::Queued;
            ++m_queued;
            --m_inFlight;
            m_budgetStalled = true;
            continue;
        }
        if (opened && !slot->bypass) {
            m_frameBytes += size;
        }
        ++m_counters.readsIssued;
        lock.unlock();

        CompletedLoad& load = slot->load;
        bool success = false;
        if (opened) {
            load.asset.bytes.resize(static_cast<usize>(size));
            success = size == 0u || file.readAll(load.asset.bytes.data(), size);
        }
        file.close();
        if (!success) {
            load.asset.bytes.clear();
        }
        load.success = success;

        lock.lock();
        if (success) {
            m_counters.bytesReadTotal += size;
        }
        if (slot->cancelRequested) {
            discardInFlightLocked(slot);
            continue;
        }
        if (success && slot->decode) {
            slot->state = SlotState::Decoding;
            const IoPriority priority = slot->priority;
            lock.unlock();
            fuse::jobs::JobScheduler::instance().submit([this, slot] { runDecode(slot); }, toJobPriority(priority));
            lock.lock();
            continue;
        }
        slot->state = SlotState::Delivering;
        lock.unlock();
        deliver(slot);
        lock.lock();
    }
    t_onIoLane = false;
}

void VirtualFileSystem::runDecode(Slot* slot) {
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        if (slot->cancelRequested) {
            discardInFlightLocked(slot);
            return;
        }
    }

    DecodeScratch& scratch = threadDecodeScratch();
    scratch.reset();
    slot->decode(slot->load, scratch);
    scratch.reset();

    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        ++m_counters.decodesRun;
        if (slot->cancelRequested) {
            discardInFlightLocked(slot);
            return;
        }
        slot->state = SlotState::Delivering;
    }
    deliver(slot);
}

void VirtualFileSystem::deliver(Slot* slot) {
    if (slot->onComplete) {
        slot->onComplete(slot->load);
    } else {
        pushCompleted(std::move(slot->load));
    }

    const std::lock_guard<std::mutex> lock(m_laneMutex);
    ++m_counters.loadsDelivered;
    releaseSlotLocked(slot);
    --m_inFlight;
    m_idleCv.notify_all();
}

DecodeScratch& VirtualFileSystem::threadDecodeScratch() {
    const u32 wanted = g_decodeScratchBytes.load(std::memory_order_relaxed);
    if (!t_scratch.scratch || t_scratch.capacity != wanted) {
        t_scratch.scratch = std::make_unique<DecodeScratch>(wanted, "io_decode_scratch");
        t_scratch.capacity = wanted;
    }
    return *t_scratch.scratch;
}

// ---- lane control ------------------------------------------------------------------------------

void VirtualFileSystem::configureLane(IoLaneConfig config) {
    std::vector<std::thread> threads;
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        m_laneStop = true;
        threads.swap(m_threads);
    }
    m_laneCv.notify_all();
    for (std::thread& thread : threads) {
        thread.join();
    }

    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        config.ioThreadCount = clampThreadCount(config.ioThreadCount);
        m_config = std::move(config);
        g_decodeScratchBytes.store(m_config.decodeScratchBytes, std::memory_order_relaxed);
        m_laneStop = false;
        if (m_queued != 0u) {
            ensureLaneStartedLocked();
        }
    }
    m_laneCv.notify_all();
}

IoLaneConfig VirtualFileSystem::laneConfig() const {
    const std::lock_guard<std::mutex> lock(m_laneMutex);
    return m_config;
}

void VirtualFileSystem::suspend() {
    const std::lock_guard<std::mutex> lock(m_laneMutex);
    m_manualSuspend = true;
}

void VirtualFileSystem::resume() {
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        m_manualSuspend = false;
    }
    m_laneCv.notify_all();
}

bool VirtualFileSystem::isSuspended() const {
    const std::lock_guard<std::mutex> lock(m_laneMutex);
    return suspendedLocked();
}

void VirtualFileSystem::onPowerStateChanged(bool background) {
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        m_powerSuspend = background;
    }
    m_laneCv.notify_all();
}

void VirtualFileSystem::advanceFrame() {
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        ++m_frameIndex;
        m_frameBytes = 0;
        m_budgetStalled = false;
    }
    m_laneCv.notify_all();
}

void VirtualFileSystem::setFrameReadBudget(u64 bytesPerFrame) {
    {
        const std::lock_guard<std::mutex> lock(m_laneMutex);
        m_config.frameReadBudgetBytes = bytesPerFrame;
        m_budgetStalled = false;
    }
    m_laneCv.notify_all();
}

bool VirtualFileSystem::waitIdle(u32 timeoutMs) {
    std::unique_lock<std::mutex> lock(m_laneMutex);
    return m_idleCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [this] { return m_queued == 0u && m_inFlight == 0u; });
}

IoLaneStats VirtualFileSystem::laneStats() const {
    const std::lock_guard<std::mutex> lock(m_laneMutex);
    IoLaneStats stats = m_counters;
    stats.ioThreadCount = static_cast<u32>(m_threads.size());
    stats.queued = m_queued;
    stats.inFlight = m_inFlight;
    stats.suspended = suspendedLocked();
    stats.budgetStalled = m_budgetStalled;
    stats.frameIndex = m_frameIndex;
    stats.bytesReadThisFrame = m_frameBytes;
    return stats;
}

// ---- drain queue -------------------------------------------------------------------------------

void VirtualFileSystem::pushCompleted(CompletedLoad load) {
    const std::lock_guard<std::mutex> lock(m_completedMutex);
    m_completed.push_back(std::move(load));
}

u32 VirtualFileSystem::drainCompletedLoads(HandleTable<Asset>& table) {
    std::vector<CompletedLoad> batch;
    {
        const std::lock_guard<std::mutex> lock(m_completedMutex);
        batch.swap(m_completed);
    }

    m_lastDrained = batch;
    for (CompletedLoad& load : batch) {
        if (load.success) {
            table.enqueuePublish(std::move(load.asset));
        }
    }
    return static_cast<u32>(batch.size());
}

u32 VirtualFileSystem::completedLoadCount() const {
    const std::lock_guard<std::mutex> lock(m_completedMutex);
    return static_cast<u32>(m_completed.size());
}

std::vector<LoadId> VirtualFileSystem::peekCompletedLoadOrder() const {
    const std::lock_guard<std::mutex> lock(m_completedMutex);
    std::vector<LoadId> order;
    order.reserve(m_completed.size());
    for (const CompletedLoad& load : m_completed) {
        order.push_back(load.id);
    }
    return order;
}

} // namespace fuse::io
