#include <fuse/physics/shapes/shape_pool.hpp>

#include <fuse/physics/rotation.hpp>
#include <fuse/physics/spatial/svo.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {

ShapePool& ShapePool::global() {
    static ShapePool pool;
    return pool;
}

ShapePool::ShapePool() {
    for (std::atomic<Chunk*>& chunk : m_chunks) {
        chunk.store(nullptr, std::memory_order_relaxed);
    }
}

ShapePool::~ShapePool() = default;

u32 ShapePool::insert(Entry entry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    u32 ref = kNoShapeRef;
    if (!m_free.empty()) {
        // Lowest free slot first (deterministic references for a deterministic registration order).
        const auto lowest = std::min_element(m_free.begin(), m_free.end());
        ref = *lowest;
        m_free.erase(lowest);
    } else {
        if (m_next >= kMaxChunks * kChunkSize) {
            return kNoShapeRef;
        }
        ref = m_next++;
        const u32 chunkIndex = ref >> kChunkBits;
        if (m_chunks[chunkIndex].load(std::memory_order_relaxed) == nullptr) {
            m_owned.push_back(std::make_unique<Chunk>());
            m_chunks[chunkIndex].store(m_owned.back().get(), std::memory_order_release);
        }
    }
    Chunk* chunk = m_chunks[ref >> kChunkBits].load(std::memory_order_relaxed);
    entry.used = true;
    (*chunk)[ref & (kChunkSize - 1u)] = std::move(entry);
    m_live.fetch_add(1u, std::memory_order_relaxed);
    return ref;
}

const ShapePool::Entry* ShapePool::entry(u32 ref) const {
    if (ref == kNoShapeRef || (ref >> kChunkBits) >= kMaxChunks) {
        return nullptr;
    }
    const Chunk* chunk = m_chunks[ref >> kChunkBits].load(std::memory_order_acquire);
    if (chunk == nullptr) {
        return nullptr;
    }
    const Entry& e = (*chunk)[ref & (kChunkSize - 1u)];
    return e.used ? &e : nullptr;
}

u32 ShapePool::addHull(ConvexHull hull) {
    if (hull.empty()) {
        return kNoShapeRef;
    }
    Entry e{};
    e.type = CollisionShapeType::ConvexHull;
    e.hull = std::make_unique<ConvexHull>(std::move(hull));
    return insert(std::move(e));
}

u32 ShapePool::addMesh(TriMesh mesh) {
    if (mesh.empty()) {
        return kNoShapeRef;
    }
    Entry e{};
    e.type = CollisionShapeType::TriMesh;
    e.mesh = std::make_unique<TriMesh>(std::move(mesh));
    return insert(std::move(e));
}

u32 ShapePool::addSdf(std::shared_ptr<const SdfSampler> sdf) {
    if (sdf == nullptr) {
        return kNoShapeRef;
    }
    Entry e{};
    e.type = CollisionShapeType::SdfMesh;
    e.sdf = std::move(sdf);
    return insert(std::move(e));
}

u32 ShapePool::addVoxelView(const VoxelVolume* volume) {
    if (volume == nullptr) {
        return kNoShapeRef;
    }
    Entry e{};
    e.type = CollisionShapeType::Voxel;
    e.voxel = volume;
    return insert(std::move(e));
}

void ShapePool::release(u32 ref) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ref == kNoShapeRef || (ref >> kChunkBits) >= kMaxChunks) {
        return;
    }
    Chunk* chunk = m_chunks[ref >> kChunkBits].load(std::memory_order_relaxed);
    if (chunk == nullptr || !(*chunk)[ref & (kChunkSize - 1u)].used) {
        return;
    }
    (*chunk)[ref & (kChunkSize - 1u)] = Entry{};
    m_free.push_back(ref);
    m_live.fetch_sub(1u, std::memory_order_relaxed);
    for (auto it = m_assets.begin(); it != m_assets.end();) {
        it = it->second == ref ? m_assets.erase(it) : std::next(it);
    }
}

CollisionShapeType ShapePool::type(u32 ref) const {
    const Entry* e = entry(ref);
    return e != nullptr ? e->type : CollisionShapeType::Sphere;
}

bool ShapePool::valid(u32 ref) const {
    return entry(ref) != nullptr;
}

const ConvexHull* ShapePool::hull(u32 ref) const {
    const Entry* e = entry(ref);
    return e != nullptr ? e->hull.get() : nullptr;
}

const TriMesh* ShapePool::mesh(u32 ref) const {
    const Entry* e = entry(ref);
    return e != nullptr ? e->mesh.get() : nullptr;
}

const SdfSampler* ShapePool::sdf(u32 ref) const {
    const Entry* e = entry(ref);
    return e != nullptr ? e->sdf.get() : nullptr;
}

const VoxelVolume* ShapePool::voxel(u32 ref) const {
    const Entry* e = entry(ref);
    return e != nullptr ? e->voxel : nullptr;
}

bool ShapePool::localBounds(u32 ref, vec3& lo, vec3& hi) const {
    const Entry* e = entry(ref);
    if (e == nullptr) {
        return false;
    }
    switch (e->type) {
    case CollisionShapeType::ConvexHull:
        lo = e->hull->boundsMin;
        hi = e->hull->boundsMax;
        return true;
    case CollisionShapeType::TriMesh:
        lo = e->mesh->boundsMin();
        hi = e->mesh->boundsMax();
        return true;
    case CollisionShapeType::SdfMesh:
        lo = e->sdf->boundsMin();
        hi = e->sdf->boundsMax();
        return true;
    case CollisionShapeType::Voxel: {
        const VoxelVolume& v = *e->voxel;
        const ivec3 dims = v.dims();
        lo = v.origin();
        hi = v.origin() + vec3{static_cast<f32>(dims.x), static_cast<f32>(dims.y), static_cast<f32>(dims.z)} * v.voxelSize();
        return true;
    }
    default:
        return false;
    }
}

vec3 ShapePool::halfExtents(u32 ref) const {
    vec3 lo;
    vec3 hi;
    if (!localBounds(ref, lo, hi)) {
        return {};
    }
    return {std::max(std::fabs(lo.x), std::fabs(hi.x)), std::max(std::fabs(lo.y), std::fabs(hi.y)),
            std::max(std::fabs(lo.z), std::fabs(hi.z))};
}

void ShapePool::bindAsset(u64 assetId, u32 piece, CollisionShapeType type, u32 ref) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_assets[{assetId, piece, static_cast<u32>(type)}] = ref;
}

u32 ShapePool::findAsset(u64 assetId, u32 piece, CollisionShapeType type) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_assets.find({assetId, piece, static_cast<u32>(type)});
    return it != m_assets.end() ? it->second : kNoShapeRef;
}

void ShapePool::unbindAsset(u64 assetId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_assets.begin(); it != m_assets.end();) {
        it = std::get<0>(it->first) == assetId ? m_assets.erase(it) : std::next(it);
    }
}

aabb pooledShapeWorldBounds(u32 ref, vec3 position, const quat& orientation) {
    vec3 lo;
    vec3 hi;
    if (!ShapePool::global().localBounds(ref, lo, hi)) {
        return {position, position};
    }
    const vec3 centre = (lo + hi) * 0.5f;
    const vec3 half = (hi - lo) * 0.5f;
    const vec3 worldCentre = position + rotate(orientation, centre);
    const vec3 worldHalf = orientedBoxHalfExtents(orientation, half);
    return {worldCentre - worldHalf, worldCentre + worldHalf};
}

} // namespace fuse::physics
