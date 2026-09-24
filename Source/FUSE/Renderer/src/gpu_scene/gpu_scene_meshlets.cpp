#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_meshlets.hpp>

#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::renderer::gpu_scene {

namespace {

constexpr u64 kSectionAlign = 16u;

u64 alignUp(u64 value, u64 align) { return (value + align - 1u) / align * align; }

template <typename T>
void putSection(std::vector<u8>& blob, u64 offset, const T* data, usize count) {
    if (count > 0u) {
        std::memcpy(blob.data() + offset, data, count * sizeof(T));
    }
}

} // namespace

GpuMeshlet packGpuMeshlet(const geometry::MeshletRecord& r) {
    GpuMeshlet g{};
    g.vertexOffset = r.vertex_offset;
    g.triangleOffset = r.triangle_offset;
    g.counts = (r.vertex_count & 0xFFu) | ((r.triangle_count & 0xFFu) << 8u) | ((r.submesh & 0xFFFFu) << 16u);
    for (u32 a = 0; a < 3u; ++a) {
        g.center[a] = r.center[a];
        g.coneApex[a] = r.cone_apex[a];
        g.coneAxis[a] = r.cone_axis[a];
        g.aabbMin[a] = r.aabb_min[a];
        g.aabbMax[a] = r.aabb_max[a];
    }
    g.radius = r.radius;
    g.coneCutoff = r.cone_cutoff;
    const geometry::i8 s8[4] = {r.cone_axis_s8[0], r.cone_axis_s8[1], r.cone_axis_s8[2], r.cone_cutoff_s8};
    std::memcpy(&g.coneS8, s8, sizeof(s8)); // little-endian bytes 0..3, as on disk
    return g;
}

MeshletGeometryLayout packMeshletGeometry(const geometry::MeshletMesh& mesh, std::vector<u8>& blob) {
    MeshletGeometryLayout l{};
    const usize meshlets = mesh.meshlets.size();
    const usize vertices = mesh.vertex_count();
    u64 cursor = 0;
    auto section = [&](u64& field, u64 bytes) {
        field = cursor;
        cursor = alignUp(cursor + bytes, kSectionAlign);
    };
    section(l.meshlets, meshlets * sizeof(GpuMeshlet));
    section(l.submeshes, mesh.submeshes.size() * sizeof(GpuSubmesh));
    section(l.meshletVertices, mesh.meshlet_vertices.size() * sizeof(u32));
    section(l.meshletTriangles, mesh.meshlet_triangles.size() * sizeof(u32));
    section(l.positions, mesh.positions.size() * sizeof(u16));
    section(l.normals, vertices * sizeof(u32));
    section(l.tangents, vertices * sizeof(u32));
    section(l.uvs, vertices * sizeof(u32));
    l.totalBytes = std::max<u64>(cursor, kSectionAlign);

    blob.assign(static_cast<usize>(l.totalBytes), 0u);
    for (usize i = 0; i < meshlets; ++i) {
        const GpuMeshlet g = packGpuMeshlet(mesh.meshlets[i]);
        std::memcpy(blob.data() + l.meshlets + i * sizeof(GpuMeshlet), &g, sizeof(g));
    }
    for (usize i = 0; i < mesh.submeshes.size(); ++i) {
        const geometry::SubmeshRange& s = mesh.submeshes[i];
        const GpuSubmesh g{s.meshlet_offset, s.meshlet_count, s.material_index, s.triangle_count};
        std::memcpy(blob.data() + l.submeshes + i * sizeof(GpuSubmesh), &g, sizeof(g));
    }
    putSection(blob, l.meshletVertices, mesh.meshlet_vertices.data(), mesh.meshlet_vertices.size());
    putSection(blob, l.meshletTriangles, mesh.meshlet_triangles.data(), mesh.meshlet_triangles.size());
    putSection(blob, l.positions, mesh.positions.data(), mesh.positions.size());
    putSection(blob, l.normals, mesh.normals.data(), std::min(mesh.normals.size(), vertices));
    putSection(blob, l.tangents, mesh.tangents.data(), std::min(mesh.tangents.size(), vertices));
    putSection(blob, l.uvs, mesh.uvs.data(), std::min(mesh.uvs.size(), vertices));
    return l;
}

u32 appendMeshletIndices(const geometry::MeshletMesh& mesh, std::vector<u32>& indices) {
    const usize base = indices.size();
    indices.resize(base + mesh.meshlet_triangles.size() * 3u);
    u32* out = indices.data() + base;
    for (const geometry::MeshletRecord& m : mesh.meshlets) {
        for (u32 t = 0; t < m.triangle_count; ++t) {
            const u32 packed = mesh.meshlet_triangles[m.triangle_offset + t];
            u32* tri = out + static_cast<usize>(m.triangle_offset + t) * 3u;
            for (u32 c = 0; c < 3u; ++c) {
                tri[c] = mesh.meshlet_vertices[m.vertex_offset + geometry::triangle_index(packed, c)];
            }
        }
    }
    return static_cast<u32>(indices.size() - base);
}

GpuMesh makeGpuMesh(const geometry::MeshletMesh& mesh, const MeshletGeometryLayout& layout, u64 baseAddress,
                    u32 geometryHandle) {
    GpuMesh g{};
    if (baseAddress != 0u) {
        g.meshlets = baseAddress + layout.meshlets;
        g.submeshes = baseAddress + layout.submeshes;
        g.meshletVertices = baseAddress + layout.meshletVertices;
        g.meshletTriangles = baseAddress + layout.meshletTriangles;
        g.positions = baseAddress + layout.positions;
        g.normals = baseAddress + layout.normals;
        g.tangents = baseAddress + layout.tangents;
        g.uvs = baseAddress + layout.uvs;
    }
    g.meshletCount = static_cast<u32>(mesh.meshlets.size());
    g.submeshCount = static_cast<u32>(mesh.submeshes.size());
    g.vertexCount = mesh.vertex_count();
    g.triangleCount = mesh.triangle_count();
    for (u32 a = 0; a < 3u; ++a) {
        g.quantOffset[a] = mesh.quant.offset[a];
        g.quantStep[a] = mesh.quant.step[a];
    }
    g.geometryHandle = geometryHandle;

    // Exact bounds of what the GPU rasterises: decode the quantised positions (bit-exact codec,
    // geometry/vertex_codec_kernel.hpp), centre on their AABB, radius = farthest vertex (f64,
    // rounded up to the next f32 so the sphere is conservative).
    geometry::DecodedVertices decoded;
    geometry::decode_vertices(mesh, decoded, kernel::Backend::CpuReference);
    const usize n = decoded.positions.size() / 3u;
    if (n > 0u) {
        f32 lo[3] = {decoded.positions[0], decoded.positions[1], decoded.positions[2]};
        f32 hi[3] = {lo[0], lo[1], lo[2]};
        for (usize v = 1; v < n; ++v) {
            for (u32 a = 0; a < 3u; ++a) {
                lo[a] = std::min(lo[a], decoded.positions[v * 3u + a]);
                hi[a] = std::max(hi[a], decoded.positions[v * 3u + a]);
            }
        }
        for (u32 a = 0; a < 3u; ++a) {
            g.boundsCenter[a] = static_cast<f32>((static_cast<f64>(lo[a]) + static_cast<f64>(hi[a])) * 0.5);
        }
        f64 max2 = 0.0;
        for (usize v = 0; v < n; ++v) {
            f64 d2 = 0.0;
            for (u32 a = 0; a < 3u; ++a) {
                const f64 d = static_cast<f64>(decoded.positions[v * 3u + a]) - static_cast<f64>(g.boundsCenter[a]);
                d2 += d * d;
            }
            max2 = std::max(max2, d2);
        }
        f32 r32 = static_cast<f32>(std::sqrt(max2));
        while (static_cast<f64>(r32) * static_cast<f64>(r32) < max2) {
            r32 = std::nextafter(r32, 3.0e38f);
        }
        g.boundsRadius = r32;
    }
    return g;
}

u32 GpuScene::addMeshletMesh(const geometry::MeshletMesh& mesh) {
    if (!m_initialized) {
        return kInvalidIndex;
    }
    std::vector<u8> blob;
    const MeshletGeometryLayout layout = packMeshletGeometry(mesh, blob);
    Geometry geometry{};
    u32 handle = 0;
#if defined(FUSE_VULKAN_BACKEND)
    if (m_gpu) {
        if (!createBuffer(geometry.buffer, layout.totalBytes, "gpu_scene.geometry")) {
            return kInvalidIndex;
        }
        const usize maxPiece = std::max<usize>(m_desc.upload->ringCapacity() / 2u, 1u);
        bool ok = true;
        for (usize done = 0; ok && done < blob.size();) {
            const usize piece = std::min(maxPiece, blob.size() - done);
            usize ring = 0;
            ok = m_desc.upload->stage(blob.data() + done, piece, ring) &&
                 m_desc.upload->recordBufferCopy(geometry.buffer.handle, ring, done, piece);
            done += piece;
        }
        if (!ok) {
            m_desc.allocator->destroyBuffer(geometry.buffer);
            return kInvalidIndex;
        }
        if (m_desc.bindless != nullptr) {
            geometry.slot = m_desc.bindless->registerBufferSlot(geometry.buffer, false);
            handle = m_desc.bindless->shaderHandle(geometry.slot);
        }
        m_geometry.push_back(geometry);
    }
#endif
    const u32 firstIndex = static_cast<u32>(m_indexMirror.size());
    const u32 indexCount = appendMeshletIndices(mesh, m_indexMirror);
    if (!uploadIndices(firstIndex, indexCount)) {
        m_indexMirror.resize(firstIndex);
        return kInvalidIndex;
    }
    GpuMesh gpuMesh = makeGpuMesh(mesh, layout, geometry.buffer.deviceAddress, handle);
    gpuMesh.firstIndex = firstIndex;
    gpuMesh.indexCount = indexCount;
    gpuMesh.vertexOffset = 0;
    return addMesh(gpuMesh);
}

bool GpuScene::uploadIndices(u32 firstIndex, u32 count) {
    if (!m_gpu || count == 0u) {
        return true;
    }
#if defined(FUSE_VULKAN_BACKEND)
    const u32 needed = firstIndex + count;
    usize from = firstIndex;
    if (needed > m_indexCapacity || m_indexBuffer.handle == nullptr) {
        // Grow by doubling: a new buffer receives the whole mirror, the old one retires at this serial.
        u32 capacity = std::max(m_indexCapacity, 16384u);
        while (capacity < needed) {
            capacity *= 2u;
        }
        BufferDesc desc{};
        desc.size = static_cast<usize>(capacity) * sizeof(u32);
        desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Index) | static_cast<u32>(BufferUsage::Storage) |
                                              static_cast<u32>(BufferUsage::TransferDst) |
                                              static_cast<u32>(BufferUsage::TransferSrc) |
                                              static_cast<u32>(BufferUsage::ShaderDeviceAddress));
        desc.memoryUsage = MemoryUsage::GpuOnly;
        desc.name = "gpu_scene.indices";
        Buffer fresh{};
        if (!m_desc.allocator->createBuffer(desc, fresh) || fresh.deviceAddress == 0u) {
            if (fresh.handle != nullptr) {
                m_desc.allocator->destroyBuffer(fresh);
            }
            return false;
        }
        BindlessSlotHandle none{};
        retire(m_indexBuffer, none);
        m_indexBuffer = fresh;
        m_indexCapacity = capacity;
        from = 0;
    }
    const u8* bytes = reinterpret_cast<const u8*>(m_indexMirror.data());
    const usize end = static_cast<usize>(needed) * sizeof(u32);
    const usize maxPiece = std::max<usize>((m_desc.upload->ringCapacity() / 2u) & ~usize{3u}, 4u);
    for (usize done = from * sizeof(u32); done < end;) {
        const usize piece = std::min(maxPiece, end - done);
        usize ring = 0;
        if (!m_desc.upload->stage(bytes + done, piece, ring) ||
            !m_desc.upload->recordBufferCopy(m_indexBuffer.handle, ring, done, piece)) {
            return false;
        }
        done += piece;
    }
    return true;
#else
    (void)firstIndex;
    return true;
#endif
}

} // namespace fuse::renderer::gpu_scene
