#include <fuse/physics/assets/collision_asset.hpp>

#include <fuse/physics/shapes/shape_pool.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

namespace fuse::physics {

namespace {

u32 fnv1a(const u8* data, usize size) {
    u32 hash = 2166136261u;
    for (usize i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

class Writer {
public:
    void u32v(u32 value) {
        for (u32 i = 0; i < 4u; ++i) {
            m_bytes.push_back(static_cast<u8>((value >> (8u * i)) & 0xFFu));
        }
    }
    void f32v(f32 value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32v(bits);
    }
    void vec(vec3 v) {
        f32v(v.x);
        f32v(v.y);
        f32v(v.z);
    }
    void byte(u8 value) { m_bytes.push_back(value); }
    void pad4() {
        while (m_bytes.size() % 4u != 0u) {
            m_bytes.push_back(0u);
        }
    }
    std::vector<u8>& bytes() { return m_bytes; }

private:
    std::vector<u8> m_bytes;
};

class Reader {
public:
    Reader(const u8* data, usize size) : m_data(data), m_size(size) {}

    bool u32v(u32& out) {
        if (m_offset + 4u > m_size) {
            return false;
        }
        out = static_cast<u32>(m_data[m_offset]) | (static_cast<u32>(m_data[m_offset + 1u]) << 8u) |
              (static_cast<u32>(m_data[m_offset + 2u]) << 16u) | (static_cast<u32>(m_data[m_offset + 3u]) << 24u);
        m_offset += 4u;
        return true;
    }
    bool f32v(f32& out) {
        u32 bits = 0;
        if (!u32v(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    bool vec(vec3& out) { return f32v(out.x) && f32v(out.y) && f32v(out.z); }
    bool byte(u8& out) {
        if (m_offset + 1u > m_size) {
            return false;
        }
        out = m_data[m_offset++];
        return true;
    }
    bool pad4() {
        while (m_offset % 4u != 0u) {
            if (m_offset >= m_size) {
                return false;
            }
            ++m_offset;
        }
        return true;
    }
    /// Remaining bytes can hold `count` elements of `bytes` each (guards absurd counts).
    [[nodiscard]] bool fits(u64 count, u64 bytes) const { return count * bytes <= m_size - m_offset; }
    [[nodiscard]] usize offset() const { return m_offset; }

private:
    const u8* m_data;
    usize m_size;
    usize m_offset = 0;
};

} // namespace

std::vector<u8> serializeCollisionAsset(const CollisionAsset& asset) {
    Writer w;
    w.u32v(kCollisionAssetMagic);
    w.u32v(kCollisionAssetVersion);
    w.u32v(static_cast<u32>(asset.hulls.size()));
    w.u32v(static_cast<u32>(asset.meshes.size()));
    w.u32v(0u);
    for (const ConvexHull& hull : asset.hulls) {
        w.u32v(static_cast<u32>(hull.vertices.size()));
        w.u32v(static_cast<u32>(hull.faces.size()));
        w.u32v(static_cast<u32>(hull.faceIndices.size()));
        for (const vec3& v : hull.vertices) {
            w.vec(v);
        }
        for (const HullFace& face : hull.faces) {
            w.u32v(face.firstIndex);
            w.u32v(face.indexCount);
        }
        for (const u32 index : hull.faceIndices) {
            w.u32v(index);
        }
    }
    for (const TriMesh& mesh : asset.meshes) {
        w.u32v(static_cast<u32>(mesh.vertices().size()));
        w.u32v(mesh.triangleCount());
        w.u32v(static_cast<u32>(mesh.nodes().size()));
        for (const vec3& v : mesh.vertices()) {
            w.vec(v);
        }
        for (const u32 index : mesh.indices()) {
            w.u32v(index);
        }
        for (const TriMeshBvhNode& node : mesh.nodes()) {
            w.vec(node.boundsMin);
            w.u32v(node.first);
            w.vec(node.boundsMax);
            w.u32v(node.count);
        }
        for (const u8 flags : mesh.allEdgeFlags()) {
            w.byte(flags);
        }
        w.pad4();
    }
    std::vector<u8>& bytes = w.bytes();
    const u32 hash = fnv1a(bytes.data(), bytes.size());
    w.u32v(hash);
    return std::move(w.bytes());
}

bool deserializeCollisionAsset(const u8* data, usize size, CollisionAsset& out, std::string* error) {
    out = CollisionAsset{};
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        out = CollisionAsset{};
        return false;
    };
    if (data == nullptr || size < 24u || size % 4u != 0u) {
        return fail("truncated .fusecol");
    }
    const u32 stored = static_cast<u32>(data[size - 4u]) | (static_cast<u32>(data[size - 3u]) << 8u) |
             (static_cast<u32>(data[size - 2u]) << 16u) | (static_cast<u32>(data[size - 1u]) << 24u);
    if (fnv1a(data, size - 4u) != stored) {
        return fail("hash mismatch (corrupt .fusecol)");
    }
    Reader r(data, size - 4u);
    u32 magic = 0;
    u32 version = 0;
    u32 hullCount = 0;
    u32 meshCount = 0;
    u32 flags = 0;
    if (!r.u32v(magic) || !r.u32v(version) || !r.u32v(hullCount) || !r.u32v(meshCount) || !r.u32v(flags)) {
        return fail("truncated header");
    }
    if (magic != kCollisionAssetMagic) {
        return fail("not a .fusecol file");
    }
    if (version != kCollisionAssetVersion || flags != 0u) {
        return fail("unsupported .fusecol version " + std::to_string(version));
    }
    if (!r.fits(hullCount, 12u) || !r.fits(meshCount, 12u)) {
        return fail("piece counts exceed the file");
    }
    for (u32 h = 0; h < hullCount; ++h) {
        u32 vertexCount = 0;
        u32 faceCount = 0;
        u32 indexCount = 0;
        if (!r.u32v(vertexCount) || !r.u32v(faceCount) || !r.u32v(indexCount) || !r.fits(vertexCount, 12u) ||
            !r.fits(faceCount, 8u) || !r.fits(indexCount, 4u)) {
            return fail("truncated hull " + std::to_string(h));
        }
        ConvexHull hull{};
        hull.vertices.resize(vertexCount);
        hull.faces.resize(faceCount);
        hull.faceIndices.resize(indexCount);
        for (vec3& v : hull.vertices) {
            if (!r.vec(v)) {
                return fail("truncated hull vertices");
            }
        }
        for (HullFace& face : hull.faces) {
            if (!r.u32v(face.firstIndex) || !r.u32v(face.indexCount)) {
                return fail("truncated hull faces");
            }
        }
        for (u32& index : hull.faceIndices) {
            if (!r.u32v(index)) {
                return fail("truncated hull face indices");
            }
        }
        std::string why;
        if (!finalizeConvexHull(hull, &why)) {
            return fail("hull " + std::to_string(h) + ": " + why);
        }
        out.hulls.push_back(std::move(hull));
    }
    for (u32 m = 0; m < meshCount; ++m) {
        u32 vertexCount = 0;
        u32 triangleCount = 0;
        u32 nodeCount = 0;
        if (!r.u32v(vertexCount) || !r.u32v(triangleCount) || !r.u32v(nodeCount) || !r.fits(vertexCount, 12u) ||
            !r.fits(triangleCount, 13u) || !r.fits(nodeCount, 32u)) {
            return fail("truncated mesh " + std::to_string(m));
        }
        std::vector<vec3> vertices(vertexCount);
        std::vector<u32> indices(static_cast<usize>(triangleCount) * 3u);
        std::vector<TriMeshBvhNode> nodes(nodeCount);
        std::vector<u8> edgeFlags(triangleCount);
        for (vec3& v : vertices) {
            if (!r.vec(v)) {
                return fail("truncated mesh vertices");
            }
        }
        for (u32& index : indices) {
            if (!r.u32v(index)) {
                return fail("truncated mesh indices");
            }
        }
        for (TriMeshBvhNode& node : nodes) {
            if (!r.vec(node.boundsMin) || !r.u32v(node.first) || !r.vec(node.boundsMax) || !r.u32v(node.count)) {
                return fail("truncated mesh BVH");
            }
        }
        for (u8& f : edgeFlags) {
            if (!r.byte(f)) {
                return fail("truncated mesh edge flags");
            }
        }
        if (!r.pad4()) {
            return fail("truncated mesh padding");
        }
        TriMesh mesh;
        std::string why;
        if (!mesh.assign(std::move(vertices), std::move(indices), std::move(nodes), std::move(edgeFlags), &why)) {
            return fail("mesh " + std::to_string(m) + ": " + why);
        }
        out.meshes.push_back(std::move(mesh));
    }
    if (r.offset() != size - 4u) {
        return fail("trailing bytes in .fusecol");
    }
    return true;
}

bool readCollisionAssetFile(const std::string& path, CollisionAsset& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) {
            *error = "cannot open " + path;
        }
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return deserializeCollisionAsset(bytes.data(), bytes.size(), out, error);
}

bool writeCollisionAssetFile(const std::string& path, const CollisionAsset& asset, std::string* error) {
    const std::vector<u8> bytes = serializeCollisionAsset(asset);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) {
            *error = "cannot write " + path;
        }
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        if (error != nullptr) {
            *error = "write failed: " + path;
        }
        return false;
    }
    return true;
}

CollisionAssetRefs registerCollisionAsset(CollisionAsset asset, u64 assetId) {
    CollisionAssetRefs refs{};
    refs.assetId = assetId;
    ShapePool& pool = ShapePool::global();
    pool.unbindAsset(assetId);
    for (u32 i = 0; i < asset.hulls.size(); ++i) {
        const u32 ref = pool.addHull(std::move(asset.hulls[i]));
        refs.hullRefs.push_back(ref);
        if (ref != kNoShapeRef) {
            pool.bindAsset(assetId, i, CollisionShapeType::ConvexHull, ref);
        }
    }
    for (u32 i = 0; i < asset.meshes.size(); ++i) {
        const u32 ref = pool.addMesh(std::move(asset.meshes[i]));
        refs.meshRefs.push_back(ref);
        if (ref != kNoShapeRef) {
            pool.bindAsset(assetId, i, CollisionShapeType::TriMesh, ref);
        }
    }
    return refs;
}

bool loadCollisionAssetFile(const std::string& path, u64 assetId, CollisionAssetRefs* refs, std::string* error) {
    CollisionAsset asset;
    if (!readCollisionAssetFile(path, asset, error)) {
        return false;
    }
    CollisionAssetRefs registered = registerCollisionAsset(std::move(asset), assetId);
    if (refs != nullptr) {
        *refs = std::move(registered);
    }
    return true;
}

void unregisterCollisionAsset(const CollisionAssetRefs& refs) {
    ShapePool& pool = ShapePool::global();
    pool.unbindAsset(refs.assetId);
    for (const u32 ref : refs.hullRefs) {
        pool.release(ref);
    }
    for (const u32 ref : refs.meshRefs) {
        pool.release(ref);
    }
}

} // namespace fuse::physics
