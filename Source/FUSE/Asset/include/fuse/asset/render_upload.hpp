#pragma once

// UNI-U7-ASSET-1: the hand-off from asset decode (job workers) to the render thread.
//
//   VFS I/O lane read -> decode job (JobScheduler worker) -> RenderUploadQueue::push (MPSC, any thread)
//   -> render thread AssetRegistry::drainRenderUploads -> IRenderUploadSink::upload -> HandleTable
//   publish -> game thread AssetRegistry::update (HandleTable::commit).
//
// IRenderUploadSink is the renderer's side of the contract (implemented by the renderer upload
// package, E06). It is called on the render thread only, one command at a time, never concurrently.
// The CPU payload is shared (std::shared_ptr<const AssetPayload>): the sink may keep a reference
// while a GPU copy is in flight; the registry keeps its own reference for CPU consumers.

#include <fuse/asset/asset_id.hpp>
#include <fuse/asset/cooked_material.hpp>
#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/asset/cooked_texture.hpp>
#include <fuse/types.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace fuse::asset {

enum class AssetType : u8 {
    Unknown = 0,
    Mesh,     ///< `.fusemesh` (FMSH v1 / v2)
    Texture,  ///< `.fusetex`
    Material, ///< `.fusemat` (binary)
};

[[nodiscard]] const char* asset_type_name(AssetType type);
/// By extension (case-insensitive): .fusemesh -> Mesh, .fusetex -> Texture, .fusemat -> Material.
[[nodiscard]] AssetType asset_type_from_path(std::string_view virtualPath);

/// Decoded, CPU-side asset data (what the cooked readers produce).
struct AssetPayload {
    std::variant<std::monostate, CookedMesh, CookedTexture, CookedMaterial> data;

    [[nodiscard]] AssetType type() const;
    [[nodiscard]] const CookedMesh* mesh() const { return std::get_if<CookedMesh>(&data); }
    [[nodiscard]] const CookedTexture* texture() const { return std::get_if<CookedTexture>(&data); }
    [[nodiscard]] const CookedMaterial* material() const { return std::get_if<CookedMaterial>(&data); }
};

/// Decode cooked bytes of `type` into `out` (runs on job workers; pure, thread safe).
bool decode_asset_payload(AssetType type, const u8* data, usize size, AssetPayload& out, std::string* error = nullptr);

enum class RenderUploadKind : u8 {
    Upload = 0, ///< create GPU resources for `payload`; the sink returns an opaque resource id
    Release,    ///< destroy `gpu_resource` (the asset was unloaded or its load was cancelled)
};

struct RenderUploadCommand {
    RenderUploadKind kind = RenderUploadKind::Upload;
    AssetId id{};
    AssetType type = AssetType::Unknown;
    u64 ticket = 0;               ///< load generation; stale tickets are dropped by the registry
    std::string virtual_path;     ///< as first requested (diagnostics)
    std::shared_ptr<const AssetPayload> payload; ///< Upload only
    u64 gpu_resource = 0;         ///< Release only: what upload() returned
};

struct RenderUploadResult {
    bool ok = false;
    u64 gpu_resource = 0; ///< opaque to the asset core (e.g. a GpuScene mesh row, a bindless index)
    std::string error;
};

/// Render-thread upload interface (implemented by the renderer, E06).
class IRenderUploadSink {
public:
    virtual ~IRenderUploadSink() = default;
    /// Create the GPU copy of `command.payload`. Called on the render thread.
    virtual RenderUploadResult upload(const RenderUploadCommand& command) = 0;
    /// Destroy `command.gpu_resource` (returned by an earlier successful upload). Render thread.
    virtual void release(const RenderUploadCommand& command) = 0;
};

/// A sink for CPU-only use (headless tools, servers, tests): every upload succeeds with resource 0
/// and release does nothing.
class CpuOnlyUploadSink final : public IRenderUploadSink {
public:
    RenderUploadResult upload(const RenderUploadCommand& command) override;
    void release(const RenderUploadCommand& command) override;
};

/// Unbounded lock-free multi-producer / single-consumer queue of upload commands (Vyukov's
/// node-based MPSC queue). push() is wait-free and may be called from any number of threads;
/// tryPop() must only be called by one consumer thread at a time (the render thread). A command
/// pushed by one producer is popped after every command that producer pushed earlier (per-producer
/// FIFO). Each push allocates one node; a pop frees one.
class RenderUploadQueue {
public:
    RenderUploadQueue();
    ~RenderUploadQueue();
    RenderUploadQueue(const RenderUploadQueue&) = delete;
    RenderUploadQueue& operator=(const RenderUploadQueue&) = delete;

    void push(RenderUploadCommand&& command);
    /// Consumer only. False when the queue is (momentarily) empty.
    bool tryPop(RenderUploadCommand& out);

    /// Commands pushed / popped so far (monotonic; any thread).
    [[nodiscard]] u64 pushedCount() const { return m_pushed.load(std::memory_order_acquire); }
    [[nodiscard]] u64 poppedCount() const { return m_popped.load(std::memory_order_acquire); }

private:
    struct Node;
    std::atomic<Node*> m_head;   ///< producers: last pushed node (never null)
    Node* m_tail;                ///< consumer: stub node whose successor is the next command
    std::atomic<u64> m_pushed{0};
    std::atomic<u64> m_popped{0};
};

} // namespace fuse::asset
