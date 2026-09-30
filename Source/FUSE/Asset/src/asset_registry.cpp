// UNI-U7-ASSET-1: runtime asset registry (see fuse/asset/asset_registry.hpp).

#include <fuse/asset/asset_registry.hpp>

#include <fuse/handle_table.hpp>
#include <fuse/jobs/worker_context.hpp>

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fuse::asset {

const char* asset_load_state_name(AssetLoadState state) {
    switch (state) {
    case AssetLoadState::Unloaded:
        return "unloaded";
    case AssetLoadState::Queued:
        return "queued";
    case AssetLoadState::Decoding:
        return "decoding";
    case AssetLoadState::Uploading:
        return "uploading";
    case AssetLoadState::Publishing:
        return "publishing";
    case AssetLoadState::Ready:
        return "ready";
    case AssetLoadState::Failed:
        return "failed";
    }
    return "?";
}

namespace {

bool in_flight(AssetLoadState state) {
    return state == AssetLoadState::Queued || state == AssetLoadState::Decoding ||
           state == AssetLoadState::Uploading || state == AssetLoadState::Publishing;
}

} // namespace

struct AssetRegistry::Impl {
    struct Entry {
        std::string path;       ///< as first registered (used for the VFS read)
        std::string normalized; ///< identity (collision check)
        AssetType type = AssetType::Unknown;
        u32 refs = 0;
        AssetLoadState state = AssetLoadState::Unloaded;
        u64 ticket = 0;         ///< current load generation; 0 = none wanted
        io::LoadId loadId = 0;
        AssetHandle handle{};
        u64 gpu = 0;
        std::string error;
    };

    /// Shared by a load's decode job and its completion callback (both on the same worker).
    struct DecodeSlot {
        std::shared_ptr<AssetPayload> payload;
        std::string error;
        bool ok = false;
    };

    io::VirtualFileSystem* vfs = nullptr;
    AssetRegistryConfig config;

    mutable std::mutex mutex;
    std::unordered_map<AssetId, Entry, AssetIdHash> entries;
    u64 nextTicket = 1;
    bool shutdown = false;
    AssetRegistryStats counters;

    RenderUploadQueue queue;
    HandleTable<Asset> table; ///< enqueuePublish: render thread; commit / get / remove: game thread

    Entry* find(AssetId id) {
        const auto it = entries.find(id);
        return it == entries.end() ? nullptr : &it->second;
    }
    const Entry* find(AssetId id) const {
        const auto it = entries.find(id);
        return it == entries.end() ? nullptr : &it->second;
    }

    /// Locked. Registers (or finds) `path`; nullptr on an empty path or a hash collision.
    Entry* registerLocked(std::string_view path, AssetId& outId) {
        std::string normalized = normalize_asset_path(path);
        outId = asset_id_from_normalized(normalized);
        if (!outId.valid()) {
            return nullptr;
        }
        auto [it, inserted] = entries.try_emplace(outId);
        Entry& e = it->second;
        if (inserted) {
            e.path = std::string(path);
            e.normalized = std::move(normalized);
            e.type = asset_type_from_path(path);
        } else if (e.normalized != normalized) {
            outId = AssetId{};
            return nullptr; // 64-bit FNV collision between two different paths: refuse the newcomer
        }
        return &e;
    }

    /// Locked: marks the entry as loading and returns the ticket to submit with (0: nothing to do).
    u64 beginLoadLocked(Entry& e) {
        if (e.type == AssetType::Unknown) {
            e.state = AssetLoadState::Failed;
            e.error = "unknown asset type (extension) for '" + e.path + "'";
            ++counters.failures;
            return 0;
        }
        e.ticket = nextTicket++;
        e.state = AssetLoadState::Queued;
        e.error.clear();
        e.loadId = 0;
        ++counters.loadsStarted;
        return e.ticket;
    }

    /// Submits the VFS read + decode for one load. Called without `mutex` held (an unresolved path
    /// completes synchronously inside submitLoad).
    static void submit(const std::shared_ptr<Impl>& impl, AssetId id, AssetType type, u64 ticket,
                       const std::string& path, io::IoPriority priority);

    void fail(AssetId id, u64 ticket, const std::string& why) {
        const std::lock_guard<std::mutex> lock(mutex);
        ++counters.failures;
        Entry* e = find(id);
        if (e != nullptr && e->ticket == ticket) {
            e->state = AssetLoadState::Failed;
            e->error = why;
            e->loadId = 0;
        }
    }
};

AssetRegistry::AssetRegistry() : AssetRegistry(io::VirtualFileSystem::instance()) {}

AssetRegistry::AssetRegistry(io::VirtualFileSystem& vfs, AssetRegistryConfig config)
    : m_impl(std::make_shared<Impl>()) {
    m_impl->vfs = &vfs;
    m_impl->config = config;
}

AssetRegistry::~AssetRegistry() {
    std::vector<io::LoadId> loads;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->shutdown = true;
        for (auto& [id, e] : m_impl->entries) {
            (void)id;
            if (e.loadId != 0u) {
                loads.push_back(e.loadId);
            }
            e.ticket = 0;
        }
    }
    for (const io::LoadId load : loads) {
        (void)m_impl->vfs->cancel(load);
    }
    // Callbacks still running hold their own reference to *m_impl and see `shutdown`.
}

AssetId AssetRegistry::registerPath(std::string_view virtualPath) {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    AssetId id;
    m_impl->registerLocked(virtualPath, id);
    return id;
}

std::string AssetRegistry::pathOf(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::Entry* e = m_impl->find(id);
    return e != nullptr ? e->path : std::string{};
}

bool AssetRegistry::contains(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->find(id) != nullptr;
}

AssetType AssetRegistry::typeOf(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::Entry* e = m_impl->find(id);
    return e != nullptr ? e->type : AssetType::Unknown;
}

AssetId AssetRegistry::acquire(std::string_view virtualPath) {
    return acquire(virtualPath, m_impl->config.priority);
}

AssetId AssetRegistry::acquire(std::string_view virtualPath, io::IoPriority priority) {
    AssetId id;
    u64 ticket = 0;
    std::string path;
    AssetType type = AssetType::Unknown;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        Impl::Entry* e = m_impl->registerLocked(virtualPath, id);
        if (e == nullptr) {
            return AssetId{};
        }
        ++e->refs;
        if (e->refs == 1u && !in_flight(e->state) && e->state != AssetLoadState::Ready) {
            ticket = m_impl->beginLoadLocked(*e);
            path = e->path;
            type = e->type;
        }
    }
    if (ticket != 0u) {
        Impl::submit(m_impl, id, type, ticket, path, priority);
    }
    return id;
}

bool AssetRegistry::acquire(AssetId id) {
    std::string path;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        const Impl::Entry* e = m_impl->find(id);
        if (e == nullptr) {
            return false;
        }
        path = e->path;
    }
    return acquire(path).valid();
}

u32 AssetRegistry::release(AssetId id) {
    io::LoadId cancelLoad = 0;
    AssetHandle unloadHandle{};
    RenderUploadCommand releaseCommand;
    bool sendRelease = false;
    u32 remaining = 0;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        Impl::Entry* e = m_impl->find(id);
        if (e == nullptr || e->refs == 0u) {
            return 0;
        }
        remaining = --e->refs;
        if (remaining != 0u) {
            return remaining;
        }
        switch (e->state) {
        case AssetLoadState::Queued:
        case AssetLoadState::Decoding:
        case AssetLoadState::Uploading:
        case AssetLoadState::Publishing:
            // Invalidate the ticket first: whatever stage the load is in drops it from now on.
            cancelLoad = e->loadId;
            ++m_impl->counters.cancelled;
            break;
        case AssetLoadState::Ready:
            unloadHandle = e->handle;
            releaseCommand.kind = RenderUploadKind::Release;
            releaseCommand.id = id;
            releaseCommand.type = e->type;
            releaseCommand.ticket = e->ticket;
            releaseCommand.virtual_path = e->path;
            releaseCommand.gpu_resource = e->gpu;
            sendRelease = true;
            ++m_impl->counters.unloads;
            break;
        case AssetLoadState::Unloaded:
        case AssetLoadState::Failed:
            break;
        }
        e->ticket = 0;
        e->loadId = 0;
        e->handle = AssetHandle{};
        e->gpu = 0;
        e->state = AssetLoadState::Unloaded;
    }
    if (cancelLoad != 0u) {
        (void)m_impl->vfs->cancel(cancelLoad);
    }
    if (unloadHandle.isValid()) {
        m_impl->table.remove(unloadHandle);
    }
    if (sendRelease) {
        m_impl->queue.push(std::move(releaseCommand));
    }
    return 0;
}

void AssetRegistry::unloadAll() {
    std::vector<std::pair<AssetId, u32>> held;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (const auto& [id, e] : m_impl->entries) {
            if (e.refs != 0u) {
                held.emplace_back(id, e.refs);
            }
        }
    }
    for (const auto& [id, refs] : held) {
        for (u32 i = 0; i < refs; ++i) {
            release(id);
        }
    }
}

u32 AssetRegistry::refCount(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::Entry* e = m_impl->find(id);
    return e != nullptr ? e->refs : 0u;
}

AssetLoadState AssetRegistry::state(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::Entry* e = m_impl->find(id);
    return e != nullptr ? e->state : AssetLoadState::Unloaded;
}

std::string AssetRegistry::error(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::Entry* e = m_impl->find(id);
    return e != nullptr ? e->error : std::string{};
}

u32 AssetRegistry::drainRenderUploads(IRenderUploadSink& sink, u32 maxCommands) {
    Impl& impl = *m_impl;
    u32 popped = 0;
    RenderUploadCommand command;
    while (popped < maxCommands && impl.queue.tryPop(command)) {
        ++popped;
        if (command.kind == RenderUploadKind::Release) {
            sink.release(command);
            const std::lock_guard<std::mutex> lock(impl.mutex);
            ++impl.counters.releases;
            continue;
        }
        {
            const std::lock_guard<std::mutex> lock(impl.mutex);
            const Impl::Entry* e = impl.find(command.id);
            if (e == nullptr || e->ticket != command.ticket || e->state != AssetLoadState::Uploading) {
                ++impl.counters.staleUploadsDropped;
                continue;
            }
        }
        RenderUploadResult result = sink.upload(command);
        bool releaseNow = false;
        {
            const std::lock_guard<std::mutex> lock(impl.mutex);
            Impl::Entry* e = impl.find(command.id);
            const bool current = e != nullptr && e->ticket == command.ticket && e->state == AssetLoadState::Uploading;
            if (result.ok) {
                ++impl.counters.uploads;
            }
            if (!current) {
                // Cancelled while the sink was uploading: free the GPU copy right here (render thread).
                ++impl.counters.staleUploadsDropped;
                releaseNow = result.ok;
            } else if (!result.ok) {
                ++impl.counters.failures;
                e->state = AssetLoadState::Failed;
                e->error = "render upload failed: " + result.error;
            } else {
                e->state = AssetLoadState::Publishing;
                e->gpu = result.gpu_resource;
                Asset asset;
                asset.id = command.id;
                asset.type = command.type;
                asset.ticket = command.ticket;
                asset.gpu_resource = result.gpu_resource;
                if (impl.config.keepCpuData) {
                    asset.payload = command.payload;
                }
                impl.table.enqueuePublish(std::move(asset));
            }
        }
        if (releaseNow) {
            RenderUploadCommand release = command;
            release.kind = RenderUploadKind::Release;
            release.payload.reset();
            release.gpu_resource = result.gpu_resource;
            sink.release(release);
            const std::lock_guard<std::mutex> lock(impl.mutex);
            ++impl.counters.releases;
        }
        command = RenderUploadCommand{};
    }
    return popped;
}

u32 AssetRegistry::update() {
    Impl& impl = *m_impl;
    std::vector<AssetHandle> committed;
    impl.table.commit(&committed);
    u32 ready = 0;
    std::vector<RenderUploadCommand> releases;
    for (const AssetHandle h : committed) {
        const Asset* asset = impl.table.get(h);
        if (asset == nullptr) {
            continue;
        }
        bool keep = false;
        {
            const std::lock_guard<std::mutex> lock(impl.mutex);
            Impl::Entry* e = impl.find(asset->id);
            if (e != nullptr && e->ticket == asset->ticket && e->state == AssetLoadState::Publishing) {
                e->handle = h;
                e->state = AssetLoadState::Ready;
                e->loadId = 0;
                keep = true;
                ++ready;
            } else {
                ++impl.counters.stalePublishesDropped;
            }
        }
        if (!keep) {
            RenderUploadCommand release;
            release.kind = RenderUploadKind::Release;
            release.id = asset->id;
            release.type = asset->type;
            release.ticket = asset->ticket;
            release.gpu_resource = asset->gpu_resource;
            releases.push_back(std::move(release));
            impl.table.remove(h);
        }
    }
    for (RenderUploadCommand& release : releases) {
        impl.queue.push(std::move(release));
    }
    return ready;
}

AssetHandle AssetRegistry::handle(AssetId id) const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::Entry* e = m_impl->find(id);
    return e != nullptr ? e->handle : AssetHandle{};
}

const Asset* AssetRegistry::get(AssetHandle h) const {
    return m_impl->table.get(h);
}

const Asset* AssetRegistry::get(AssetId id) const {
    const AssetHandle h = handle(id);
    return h.isValid() ? m_impl->table.get(h) : nullptr;
}

const CookedMesh* AssetRegistry::mesh(AssetId id) const {
    const Asset* a = get(id);
    return a != nullptr && a->payload != nullptr ? a->payload->mesh() : nullptr;
}

const CookedTexture* AssetRegistry::texture(AssetId id) const {
    const Asset* a = get(id);
    return a != nullptr && a->payload != nullptr ? a->payload->texture() : nullptr;
}

const CookedMaterial* AssetRegistry::material(AssetId id) const {
    const Asset* a = get(id);
    return a != nullptr && a->payload != nullptr ? a->payload->material() : nullptr;
}

u64 AssetRegistry::gpuResource(AssetId id) const {
    const Asset* a = get(id);
    return a != nullptr ? a->gpu_resource : 0u;
}

bool AssetRegistry::pumpUntilIdle(IRenderUploadSink& sink, u32 timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        drainRenderUploads(sink);
        update();
        const AssetRegistryStats s = stats();
        if (s.inFlight == 0u && m_impl->queue.pushedCount() == m_impl->queue.poppedCount()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

AssetRegistryStats AssetRegistry::stats() const {
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    AssetRegistryStats s = m_impl->counters;
    s.registered = static_cast<u32>(m_impl->entries.size());
    s.ready = 0;
    s.inFlight = 0;
    for (const auto& [id, e] : m_impl->entries) {
        (void)id;
        s.ready += e.state == AssetLoadState::Ready ? 1u : 0u;
        s.inFlight += in_flight(e.state) ? 1u : 0u;
    }
    return s;
}

RenderUploadQueue& AssetRegistry::uploadQueue() {
    return m_impl->queue;
}

void AssetRegistry::Impl::submit(const std::shared_ptr<Impl>& impl, AssetId id, AssetType type, u64 ticket,
                                 const std::string& path, io::IoPriority priority) {
    auto slot = std::make_shared<DecodeSlot>();

    io::LoadRequest request;
    request.priority = priority;
    // Decode stage: a JobScheduler job (VFS I/O lane hands the bytes over), per-thread scratch unused.
    request.decode = [impl, slot, id, type, ticket](io::CompletedLoad& load, io::DecodeScratch& scratch) {
        (void)scratch;
        {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            Entry* e = impl->find(id);
            if (impl->shutdown || e == nullptr || e->ticket != ticket) {
                return; // cancelled: skip the work
            }
            e->state = AssetLoadState::Decoding;
            ++impl->counters.decodes;
            if (fuse::jobs::detail::isWorkerThread()) {
                ++impl->counters.decodesOnJobWorker;
            }
        }
        auto payload = std::make_shared<AssetPayload>();
        slot->ok = decode_asset_payload(type, load.asset.bytes.data(), load.asset.bytes.size(), *payload, &slot->error);
        if (slot->ok) {
            slot->payload = std::move(payload);
        }
    };
    request.onComplete = [impl, slot, id, type, ticket](io::CompletedLoad& load) {
        if (!load.success) {
            impl->fail(id, ticket, "read failed: '" + load.asset.virtualPath + "' (unresolved or unreadable)");
            return;
        }
        if (!slot->ok) {
            if (slot->payload == nullptr && slot->error.empty()) {
                return; // decode skipped: the load was cancelled
            }
            impl->fail(id, ticket, "decode failed: " + slot->error);
            return;
        }
        RenderUploadCommand command;
        command.kind = RenderUploadKind::Upload;
        command.id = id;
        command.type = type;
        command.ticket = ticket;
        command.payload = std::move(slot->payload);
        const std::lock_guard<std::mutex> lock(impl->mutex);
        Entry* e = impl->find(id);
        if (impl->shutdown || e == nullptr || e->ticket != ticket) {
            return; // cancelled after decode
        }
        command.virtual_path = e->path;
        e->state = AssetLoadState::Uploading;
        e->loadId = 0;
        // Pushed under the lock: whoever observes Uploading also finds the command in the queue.
        impl->queue.push(std::move(command));
    };

    const io::LoadId loadId = impl->vfs->submitLoad(path, std::move(request));
    const std::lock_guard<std::mutex> lock(impl->mutex);
    Entry* e = impl->find(id);
    if (e != nullptr && e->ticket == ticket &&
        (e->state == AssetLoadState::Queued || e->state == AssetLoadState::Decoding)) {
        e->loadId = loadId;
    }
}

} // namespace fuse::asset
