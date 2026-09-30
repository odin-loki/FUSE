#pragma once

// UNI-U7-ASSET-1: the runtime asset registry.
//
// Maps AssetId <-> VFS virtual path <-> Handle<Asset> (fuse/handle_table.hpp), reference counts
// assets, loads them asynchronously and unloads them. One load:
//
//   game thread     acquire(path)            refcount 0 -> 1: VFS submitLoad (I/O lane, priority)
//   I/O lane        read the cooked bytes
//   job worker      decode_asset_payload     (VFS decode stage = JobScheduler job) -> CookedMesh /
//                                            CookedTexture / CookedMaterial, then push an Upload
//                                            command on the MPSC RenderUploadQueue
//   render thread   drainRenderUploads(sink) IRenderUploadSink::upload, then HandleTable::enqueuePublish
//   game thread     update()                 HandleTable::commit -> the asset is Ready, handle(id) valid
//
// release() to refcount 0 cancels a load in flight (VFS cancel while queued / reading / decoding; a
// stale ticket makes the render thread drop a queued upload and update() drop a pending publish) or
// unloads a Ready asset (handle removed on the game thread, a Release command sent to the render
// thread so the sink frees the GPU copy). Acquiring again later starts a fresh load.
//
// Threading: acquire / release / update / get / handle / unloadAll are game-thread calls (the
// HandleTable contract); drainRenderUploads is the render thread's (single consumer; may be the game
// thread in single-threaded hosts and tests); the identity and state queries (idOf, pathOf,
// refCount, state, error, stats) are safe from any thread. Worker callbacks keep the registry's
// internals alive, so destroying the registry with loads in flight is safe (they are cancelled);
// call unloadAll() and drain the render queue first when GPU copies must be released.

#include <fuse/asset/asset_id.hpp>
#include <fuse/asset/render_upload.hpp>
#include <fuse/handle.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace fuse::asset {

/// A loaded asset (the HandleTable payload).
struct Asset {
    AssetId id{};
    AssetType type = AssetType::Unknown;
    u64 ticket = 0;       ///< load generation it was produced by
    u64 gpu_resource = 0; ///< IRenderUploadSink::upload result
    std::shared_ptr<const AssetPayload> payload; ///< CPU data (null when keepCpuData is off)
};

using AssetHandle = Handle<Asset>;

enum class AssetLoadState : u8 {
    Unloaded = 0, ///< registered, not loaded (never acquired, released, or cancelled)
    Queued,       ///< waiting on / in the VFS I/O lane
    Decoding,     ///< decode job running
    Uploading,    ///< decoded; Upload command queued for the render thread
    Publishing,   ///< uploaded; waiting for the game-thread commit (update())
    Ready,        ///< committed: handle(id) valid, get(id) non-null
    Failed,       ///< read, decode or upload failed (error(id) says why); acquire again to retry
};

[[nodiscard]] const char* asset_load_state_name(AssetLoadState state);

struct AssetRegistryConfig {
    io::IoPriority priority = io::IoPriority::Normal; ///< default I/O priority of acquire()
    bool keepCpuData = true; ///< keep the decoded payload after upload (CPU consumers, tools, tests)
};

struct AssetRegistryStats {
    u32 registered = 0;      ///< ids known (loaded or not)
    u32 ready = 0;
    u32 inFlight = 0;        ///< Queued + Decoding + Uploading + Publishing
    u64 loadsStarted = 0;
    u64 decodes = 0;
    u64 decodesOnJobWorker = 0; ///< decodes that ran on a JobScheduler worker thread
    u64 failures = 0;
    u64 uploads = 0;            ///< successful IRenderUploadSink::upload calls
    u64 releases = 0;           ///< IRenderUploadSink::release calls
    u64 cancelled = 0;          ///< loads cancelled by release() before they became Ready
    u64 staleUploadsDropped = 0;   ///< Upload commands dropped on the render thread (cancelled)
    u64 stalePublishesDropped = 0; ///< uploaded assets dropped at commit (cancelled meanwhile)
    u64 unloads = 0;               ///< Ready assets unloaded
};

class AssetRegistry {
public:
    /// Uses io::VirtualFileSystem::instance().
    AssetRegistry();
    explicit AssetRegistry(io::VirtualFileSystem& vfs, AssetRegistryConfig config = {});
    ~AssetRegistry();

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // ---- identity (any thread) -------------------------------------------------------------------

    /// Register `virtualPath` without loading it (idempotent). Returns its id, or an invalid id when
    /// the path is empty or collides with a different registered path.
    AssetId registerPath(std::string_view virtualPath);
    /// The id a path would get (no registration).
    [[nodiscard]] static AssetId idOf(std::string_view virtualPath) { return asset_id_of(virtualPath); }
    /// The virtual path as first registered ("" when unknown).
    [[nodiscard]] std::string pathOf(AssetId id) const;
    [[nodiscard]] bool contains(AssetId id) const;
    [[nodiscard]] AssetType typeOf(AssetId id) const;

    // ---- lifetime (game thread) ------------------------------------------------------------------

    /// Reference `virtualPath` (registering it); the first reference starts the async load.
    AssetId acquire(std::string_view virtualPath);
    AssetId acquire(std::string_view virtualPath, io::IoPriority priority);
    /// Reference an already registered id. False when the id is unknown.
    bool acquire(AssetId id);
    /// Drop one reference; the last one cancels the load or unloads the asset. Returns the remaining
    /// count (0 also for unknown ids).
    u32 release(AssetId id);
    /// Drop every reference of every asset (shutdown / level change).
    void unloadAll();

    [[nodiscard]] u32 refCount(AssetId id) const;
    [[nodiscard]] AssetLoadState state(AssetId id) const;
    [[nodiscard]] std::string error(AssetId id) const;

    // ---- render thread -----------------------------------------------------------------------------

    /// Pop up to `maxCommands` commands from the upload queue and run them on `sink`. Returns the
    /// number of commands popped (including dropped stale ones).
    u32 drainRenderUploads(IRenderUploadSink& sink, u32 maxCommands = 0xFFFFFFFFu);

    // ---- game thread -------------------------------------------------------------------------------

    /// Commit uploaded assets (HandleTable::commit). Returns how many became Ready.
    u32 update();

    [[nodiscard]] AssetHandle handle(AssetId id) const;
    [[nodiscard]] const Asset* get(AssetHandle handle) const;
    [[nodiscard]] const Asset* get(AssetId id) const;
    [[nodiscard]] const CookedMesh* mesh(AssetId id) const;
    [[nodiscard]] const CookedTexture* texture(AssetId id) const;
    [[nodiscard]] const CookedMaterial* material(AssetId id) const;
    [[nodiscard]] u64 gpuResource(AssetId id) const;

    /// Loading-screen helper (single-threaded hosts, tests): pump drainRenderUploads(sink) + update()
    /// until nothing is in flight. False on timeout.
    bool pumpUntilIdle(IRenderUploadSink& sink, u32 timeoutMs);

    [[nodiscard]] AssetRegistryStats stats() const;
    /// The render-thread queue (renderers that drain it themselves; tests).
    [[nodiscard]] RenderUploadQueue& uploadQueue();

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace fuse::asset
