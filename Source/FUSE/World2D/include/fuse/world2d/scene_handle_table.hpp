#pragma once

// UNI-WP05-1 / WP-04: World2D / World3D publish their scene nodes as fuse::Handle<Object> through a
// generation-checked fuse::HandleTable. Worker jobs (cull, snapshot consumers) only ever see the
// handles in the snapshot SoA; the game thread resolves them here. Removing a node from its world,
// or destroying it while it is still published, removes its slot (the generation bumps), so any
// handle a worker still holds is rejected by `valid` / `resolve` afterwards.

#include <fuse/handle.hpp>
#include <fuse/handle_table.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse {

class SceneObject2D;

class SceneHandleTable {
public:
    SceneHandleTable() = default;
    ~SceneHandleTable();
    SceneHandleTable(const SceneHandleTable&) = delete;
    SceneHandleTable& operator=(const SceneHandleTable&) = delete;

    /// Game thread: inserts `node` and stamps its Object handle. A node already published here keeps
    /// its handle; a node published in another table is moved to this one.
    Handle<Object> publish(SceneObject2D& node);
    /// Game thread: removes `node`'s slot and clears its handle (no-op when not published here).
    void unpublish(SceneObject2D& node);
    /// Game thread: node for a live handle, nullptr for a stale or foreign handle.
    SceneObject2D* resolve(Handle<Object> handle) const;
    /// Any thread that holds no reference into the table while the game thread mutates it (the
    /// usual rule for snapshot consumers is: query between frames, or not at all).
    bool valid(Handle<Object> handle) const;
    u32 liveCount() const { return m_table.liveCount(); }

private:
    friend class SceneObject2D;
    /// Called by ~SceneObject2D for a node still published here.
    void onNodeDestroyed_(SceneObject2D& node);

    static Handle<SceneObject2D*> toSlot_(Handle<Object> handle) {
        return Handle<SceneObject2D*>(handle.index(), handle.generation());
    }

    HandleTable<SceneObject2D*> m_table;
    /// Generation of the live handle per slot index (0 = free): lets the destructor detach survivors.
    std::vector<u32> m_liveGeneration;
};

} // namespace fuse
