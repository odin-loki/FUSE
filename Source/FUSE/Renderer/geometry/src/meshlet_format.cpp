#include <fuse/renderer/geometry/meshlet_format.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fuse::renderer::geometry {

namespace {

constexpr u8 kMagic[4] = {'F', 'M', 'L', 'T'};
constexpr u32 kHeaderBytes = 64u;
constexpr u32 kChunkEntryBytes = 32u;
constexpr u32 kTrailerBytes = 8u;
constexpr u32 kChunkAlign = 16u;

constexpr u32 fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}

enum ChunkId : u32 { kQprm = 0, kSubm, kMshl, kMvrt, kMtri, kVpos, kVnrm, kVtan, kVuv0, kVsrc, kChunkKinds };

struct ChunkKind {
    u32 fourcc;
    u32 element_bytes;
    bool required;
};

constexpr ChunkKind kChunks[kChunkKinds] = {
    {fourcc('Q', 'P', 'R', 'M'), 48u, true}, {fourcc('S', 'U', 'B', 'M'), 16u, true},
    {fourcc('M', 'S', 'H', 'L'), 96u, true}, {fourcc('M', 'V', 'R', 'T'), 4u, true},
    {fourcc('M', 'T', 'R', 'I'), 4u, true},  {fourcc('V', 'P', 'O', 'S'), 8u, true},
    {fourcc('V', 'N', 'R', 'M'), 4u, true},  {fourcc('V', 'T', 'A', 'N'), 4u, true},
    {fourcc('V', 'U', 'V', '0'), 4u, true},  {fourcc('V', 'S', 'R', 'C'), 4u, false},
};

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool fail(std::string* error, MeshletFormatError* code, MeshletFormatError kind, const std::string& message) {
    set_error(error, std::string("fusemeshlet: ") + message);
    if (code != nullptr) {
        *code = kind;
    }
    return false;
}

// ---- little-endian writer / reader --------------------------------------------------------------

struct Writer {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void u16v(u16 v) {
        out.push_back(static_cast<u8>(v & 0xFFu));
        out.push_back(static_cast<u8>(v >> 8));
    }
    void u32v(u32 v) {
        for (u32 s = 0; s < 32u; s += 8u) {
            out.push_back(static_cast<u8>((v >> s) & 0xFFu));
        }
    }
    void u64v(u64 v) {
        u32v(static_cast<u32>(v & 0xFFFFFFFFu));
        u32v(static_cast<u32>(v >> 32));
    }
    void f32v(f32 v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u32v(bits);
    }
    void pad_to(usize alignment) {
        while (out.size() % alignment != 0u) {
            out.push_back(0u);
        }
    }
};

struct Reader {
    const u8* p;
    u16 u16v() {
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    u32 u32v() {
        const u32 v = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
                      (static_cast<u32>(p[3]) << 24);
        p += 4;
        return v;
    }
    u64 u64v() {
        const u64 lo = u32v();
        return lo | (static_cast<u64>(u32v()) << 32);
    }
    f32 f32v() {
        const u32 bits = u32v();
        f32 v = 0.f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    u8 u8v() { return *p++; }
    i8 i8v() { return static_cast<i8>(*p++); }
};

u64 element_count_of(const MeshletMesh& m, u32 kind) {
    switch (kind) {
    case kQprm: return 1u;
    case kSubm: return m.submeshes.size();
    case kMshl: return m.meshlets.size();
    case kMvrt: return m.meshlet_vertices.size();
    case kMtri: return m.meshlet_triangles.size();
    case kVpos: return m.positions.size() / 4u;
    case kVnrm: return m.normals.size();
    case kVtan: return m.tangents.size();
    case kVuv0: return m.uvs.size();
    case kVsrc: return m.source_vertices.size();
    default: return 0u;
    }
}

void write_payload(Writer& w, const MeshletMesh& m, u32 kind) {
    switch (kind) {
    case kQprm:
        for (s32 e : m.quant.exponent) {
            w.u32v(static_cast<u32>(e));
        }
        for (f32 o : m.quant.offset) {
            w.f32v(o);
        }
        for (f32 s : m.quant.step) {
            w.f32v(s);
        }
        for (u32 i = 0; i < 3u; ++i) {
            w.u32v(0u);
        }
        break;
    case kSubm:
        for (const SubmeshRange& s : m.submeshes) {
            w.u32v(s.meshlet_offset);
            w.u32v(s.meshlet_count);
            w.u32v(s.material_index);
            w.u32v(s.triangle_count);
        }
        break;
    case kMshl:
        for (const MeshletRecord& r : m.meshlets) {
            w.u32v(r.vertex_offset);
            w.u32v(r.triangle_offset);
            w.u8v(static_cast<u8>(r.vertex_count));
            w.u8v(static_cast<u8>(r.triangle_count));
            w.u16v(static_cast<u16>(r.submesh));
            for (f32 v : r.center) {
                w.f32v(v);
            }
            w.f32v(r.radius);
            for (f32 v : r.cone_apex) {
                w.f32v(v);
            }
            for (f32 v : r.cone_axis) {
                w.f32v(v);
            }
            w.f32v(r.cone_cutoff);
            for (i8 v : r.cone_axis_s8) {
                w.u8v(static_cast<u8>(v));
            }
            w.u8v(static_cast<u8>(r.cone_cutoff_s8));
            for (f32 v : r.aabb_min) {
                w.f32v(v);
            }
            for (f32 v : r.aabb_max) {
                w.f32v(v);
            }
            for (u32 i = 0; i < 3u; ++i) {
                w.u32v(0u);
            }
        }
        break;
    case kMvrt:
        for (u32 v : m.meshlet_vertices) {
            w.u32v(v);
        }
        break;
    case kMtri:
        for (u32 v : m.meshlet_triangles) {
            w.u32v(v);
        }
        break;
    case kVpos:
        for (u16 v : m.positions) {
            w.u16v(v);
        }
        break;
    case kVnrm:
        for (u32 v : m.normals) {
            w.u32v(v);
        }
        break;
    case kVtan:
        for (u32 v : m.tangents) {
            w.u32v(v);
        }
        break;
    case kVuv0:
        for (u32 v : m.uvs) {
            w.u32v(v);
        }
        break;
    case kVsrc:
        for (u32 v : m.source_vertices) {
            w.u32v(v);
        }
        break;
    default: break;
    }
}

bool read_payload(Reader r, MeshletMesh& m, u32 kind, u32 count) {
    switch (kind) {
    case kQprm:
        for (s32& e : m.quant.exponent) {
            e = static_cast<s32>(r.u32v());
        }
        for (f32& o : m.quant.offset) {
            o = r.f32v();
        }
        for (f32& s : m.quant.step) {
            s = r.f32v();
        }
        for (u32 i = 0; i < 3u; ++i) {
            if (r.u32v() != 0u) {
                return false;
            }
        }
        return true;
    case kSubm:
        m.submeshes.resize(count);
        for (SubmeshRange& s : m.submeshes) {
            s.meshlet_offset = r.u32v();
            s.meshlet_count = r.u32v();
            s.material_index = r.u32v();
            s.triangle_count = r.u32v();
        }
        return true;
    case kMshl:
        m.meshlets.resize(count);
        for (MeshletRecord& rec : m.meshlets) {
            rec.vertex_offset = r.u32v();
            rec.triangle_offset = r.u32v();
            rec.vertex_count = r.u8v();
            rec.triangle_count = r.u8v();
            rec.submesh = r.u16v();
            for (f32& v : rec.center) {
                v = r.f32v();
            }
            rec.radius = r.f32v();
            for (f32& v : rec.cone_apex) {
                v = r.f32v();
            }
            for (f32& v : rec.cone_axis) {
                v = r.f32v();
            }
            rec.cone_cutoff = r.f32v();
            for (i8& v : rec.cone_axis_s8) {
                v = r.i8v();
            }
            rec.cone_cutoff_s8 = r.i8v();
            for (f32& v : rec.aabb_min) {
                v = r.f32v();
            }
            for (f32& v : rec.aabb_max) {
                v = r.f32v();
            }
            for (u32 i = 0; i < 3u; ++i) {
                if (r.u32v() != 0u) {
                    return false;
                }
            }
        }
        return true;
    case kVpos:
        m.positions.resize(static_cast<usize>(count) * 4u);
        for (u16& v : m.positions) {
            v = r.u16v();
        }
        return true;
    default: {
        std::vector<u32>* dst = kind == kMvrt   ? &m.meshlet_vertices
                                : kind == kMtri ? &m.meshlet_triangles
                                : kind == kVnrm ? &m.normals
                                : kind == kVtan ? &m.tangents
                                : kind == kVuv0 ? &m.uvs
                                                : &m.source_vertices;
        dst->resize(count);
        for (u32& v : *dst) {
            v = r.u32v();
        }
        return true;
    }
    }
}

bool finite3(const f32 v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

} // namespace

const char* meshlet_format_error_name(MeshletFormatError error) {
    switch (error) {
    case MeshletFormatError::None: return "none";
    case MeshletFormatError::Truncated: return "truncated";
    case MeshletFormatError::BadMagic: return "bad-magic";
    case MeshletFormatError::UnsupportedVersion: return "unsupported-version";
    case MeshletFormatError::BadHeader: return "bad-header";
    case MeshletFormatError::BadChunkTable: return "bad-chunk-table";
    case MeshletFormatError::MissingChunk: return "missing-chunk";
    case MeshletFormatError::DuplicateChunk: return "duplicate-chunk";
    case MeshletFormatError::BadElementSize: return "bad-element-size";
    case MeshletFormatError::ChecksumMismatch: return "checksum-mismatch";
    case MeshletFormatError::Invalid: return "invalid";
    }
    return "unknown";
}

u64 meshlet_fnv1a64(const u8* data, usize size) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool meshlet_mesh_equal(const MeshletMesh& a, const MeshletMesh& b) {
    auto same = [](const auto& x, const auto& y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return a.version_minor == b.version_minor && a.flags == b.flags && a.max_vertices == b.max_vertices &&
           a.max_triangles == b.max_triangles && a.source_hash == b.source_hash &&
           std::memcmp(&a.quant, &b.quant, sizeof(QuantParams)) == 0 && same(a.submeshes, b.submeshes) &&
           same(a.meshlets, b.meshlets) && same(a.meshlet_vertices, b.meshlet_vertices) &&
           same(a.meshlet_triangles, b.meshlet_triangles) && same(a.positions, b.positions) &&
           same(a.normals, b.normals) && same(a.tangents, b.tangents) && same(a.uvs, b.uvs) &&
           same(a.source_vertices, b.source_vertices);
}

bool validate_meshlet_mesh(const MeshletMesh& m, std::string* error) {
    auto bad = [&](const std::string& message) {
        set_error(error, "fusemeshlet: " + message);
        return false;
    };
    if (m.max_vertices < 3u || m.max_vertices > kMeshletFormatMaxVertices || m.max_triangles < 1u ||
        m.max_triangles > kMeshletFormatMaxTriangles) {
        return bad("meshlet limits out of range");
    }
    const u32 vertexCount = m.vertex_count();
    if (m.positions.size() != static_cast<usize>(vertexCount) * 4u || m.tangents.size() != vertexCount ||
        m.uvs.size() != vertexCount || (!m.source_vertices.empty() && m.source_vertices.size() != vertexCount)) {
        return bad("vertex stream sizes disagree");
    }
    for (u32 a = 0; a < 3u; ++a) {
        const s32 e = m.quant.exponent[a];
        const f32 step = m.quant.step[a];
        const f32 offset = m.quant.offset[a];
        if (e < -126 || e > 127 || step != std::ldexp(1.f, e) || !std::isfinite(offset)) {
            return bad("quantisation step is not 2^exponent");
        }
        const f64 k = static_cast<f64>(offset) / static_cast<f64>(step);
        if (k != std::floor(k) || std::fabs(k) + 65535.0 >= 16777216.0) {
            return bad("quantisation offset breaks the exact-decode contract");
        }
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        if ((m.positions[v * 4u + 3u] & ~kVposTangentNegative) != 0u) {
            return bad("VPOS.w reserved bits set");
        }
    }
    u32 expectMeshlet = 0;
    u64 triangleTotal = 0;
    for (usize s = 0; s < m.submeshes.size(); ++s) {
        const SubmeshRange& sub = m.submeshes[s];
        if (sub.meshlet_offset != expectMeshlet || static_cast<u64>(sub.meshlet_offset) + sub.meshlet_count > m.meshlets.size()) {
            return bad("submesh meshlet ranges do not tile the meshlet table");
        }
        u64 subTriangles = 0;
        for (u32 i = sub.meshlet_offset; i < sub.meshlet_offset + sub.meshlet_count; ++i) {
            if (m.meshlets[i].submesh != s) {
                return bad("meshlet submesh index does not match its range");
            }
            subTriangles += m.meshlets[i].triangle_count;
        }
        if (subTriangles != sub.triangle_count) {
            return bad("submesh triangle count mismatch");
        }
        triangleTotal += subTriangles;
        expectMeshlet += sub.meshlet_count;
    }
    if (expectMeshlet != m.meshlets.size() || triangleTotal != m.meshlet_triangles.size()) {
        return bad("submeshes do not cover every meshlet / triangle");
    }
    if (m.submeshes.size() > 0x10000u) {
        return bad("too many submeshes");
    }
    u64 vertexCursor = 0;
    u64 triangleCursor = 0;
    for (const MeshletRecord& r : m.meshlets) {
        if (r.vertex_count < 1u || r.vertex_count > m.max_vertices || r.triangle_count < 1u ||
            r.triangle_count > m.max_triangles) {
            return bad("meshlet exceeds its vertex / triangle limits");
        }
        if (r.vertex_offset != vertexCursor || r.triangle_offset != triangleCursor) {
            return bad("meshlets are not packed in order");
        }
        vertexCursor += r.vertex_count;
        triangleCursor += r.triangle_count;
        if (vertexCursor > m.meshlet_vertices.size() || triangleCursor > m.meshlet_triangles.size()) {
            return bad("meshlet range past the end of MVRT / MTRI");
        }
        for (u32 t = r.triangle_offset; t < r.triangle_offset + r.triangle_count; ++t) {
            const u32 packed = m.meshlet_triangles[t];
            if ((packed >> 24) != 0u || triangle_index(packed, 0) >= r.vertex_count ||
                triangle_index(packed, 1) >= r.vertex_count || triangle_index(packed, 2) >= r.vertex_count) {
                return bad("micro-index out of range");
            }
        }
        if (!finite3(r.center) || !std::isfinite(r.radius) || r.radius < 0.f || !finite3(r.cone_apex) ||
            !finite3(r.cone_axis) || !std::isfinite(r.cone_cutoff) || !finite3(r.aabb_min) || !finite3(r.aabb_max)) {
            return bad("non-finite or negative bounds");
        }
        for (u32 a = 0; a < 3u; ++a) {
            if (r.aabb_min[a] > r.aabb_max[a]) {
                return bad("inverted meshlet AABB");
            }
        }
    }
    if (vertexCursor != m.meshlet_vertices.size() || triangleCursor != m.meshlet_triangles.size()) {
        return bad("MVRT / MTRI hold entries no meshlet owns");
    }
    for (u32 v : m.meshlet_vertices) {
        if (v >= vertexCount) {
            return bad("meshlet vertex index out of range");
        }
    }
    return true;
}

std::vector<u8> serialize_meshlet_mesh(const MeshletMesh& m) {
    std::vector<u32> kinds;
    for (u32 k = 0; k < kChunkKinds; ++k) {
        if (kChunks[k].required || element_count_of(m, k) > 0u) {
            kinds.push_back(k);
        }
    }
    // Chunk offsets: payloads follow the table in order, each 16-byte aligned.
    std::vector<u64> offsets(kinds.size());
    u64 cursor = kHeaderBytes + static_cast<u64>(kinds.size()) * kChunkEntryBytes;
    for (usize i = 0; i < kinds.size(); ++i) {
        cursor = (cursor + kChunkAlign - 1u) / kChunkAlign * kChunkAlign;
        offsets[i] = cursor;
        cursor += element_count_of(m, kinds[i]) * kChunks[kinds[i]].element_bytes;
    }

    std::vector<u8> out;
    out.reserve(static_cast<usize>(cursor) + kChunkAlign + kTrailerBytes);
    Writer w{out};
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    w.u16v(kMeshletFormatVersionMajor);
    w.u16v(m.version_minor);
    w.u32v(kHeaderBytes);
    w.u32v(m.flags);
    w.u32v(static_cast<u32>(kinds.size()));
    w.u32v(m.vertex_count());
    w.u32v(m.triangle_count());
    w.u32v(static_cast<u32>(m.meshlets.size()));
    w.u32v(static_cast<u32>(m.submeshes.size()));
    w.u16v(static_cast<u16>(m.max_vertices));
    w.u16v(static_cast<u16>(m.max_triangles));
    w.u64v(m.source_hash);
    w.u32v(static_cast<u32>(m.meshlet_vertices.size()));
    for (u32 i = 0; i < 3u; ++i) {
        w.u32v(0u);
    }
    for (usize i = 0; i < kinds.size(); ++i) {
        const u64 count = element_count_of(m, kinds[i]);
        w.u32v(kChunks[kinds[i]].fourcc);
        w.u32v(kChunks[kinds[i]].element_bytes);
        w.u32v(static_cast<u32>(count));
        w.u32v(0u);
        w.u64v(offsets[i]);
        w.u64v(count * kChunks[kinds[i]].element_bytes);
    }
    for (u32 kind : kinds) {
        w.pad_to(kChunkAlign);
        write_payload(w, m, kind);
    }
    w.u64v(meshlet_fnv1a64(out.data(), out.size()));
    return out;
}

bool parse_meshlet_mesh(const u8* data, usize size, MeshletMesh& out, std::string* error, MeshletFormatError* code) {
    out = MeshletMesh{};
    if (code != nullptr) {
        *code = MeshletFormatError::None;
    }
    auto reject = [&](MeshletFormatError kind, const std::string& message) {
        out = MeshletMesh{};
        return fail(error, code, kind, message);
    };
    if (data == nullptr || size < kHeaderBytes + kTrailerBytes) {
        return reject(MeshletFormatError::Truncated, "file shorter than header + trailer");
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        return reject(MeshletFormatError::BadMagic, "magic is not FMLT");
    }
    Reader r{data + 4};
    const u16 major = r.u16v();
    const u16 minor = r.u16v();
    if (major != kMeshletFormatVersionMajor) {
        return reject(MeshletFormatError::UnsupportedVersion,
                      "version " + std::to_string(major) + "." + std::to_string(minor) + " unsupported (reader is 1.x)");
    }
    const u32 headerBytes = r.u32v();
    const u32 flags = r.u32v();
    const u32 chunkCount = r.u32v();
    const u32 vertexCount = r.u32v();
    const u32 triangleCount = r.u32v();
    const u32 meshletCount = r.u32v();
    const u32 submeshCount = r.u32v();
    const u32 maxVertices = r.u16v();
    const u32 maxTriangles = r.u16v();
    const u64 sourceHash = r.u64v();
    const u32 meshletVertexCount = r.u32v();
    const u32 reserved0 = r.u32v();
    const u32 reserved1 = r.u32v();
    const u32 reserved2 = r.u32v();
    if (headerBytes != kHeaderBytes || (reserved0 | reserved1 | reserved2) != 0u) {
        return reject(MeshletFormatError::BadHeader, "header size or reserved fields invalid");
    }
    const u64 tableEnd = kHeaderBytes + static_cast<u64>(chunkCount) * kChunkEntryBytes;
    if (tableEnd + kTrailerBytes > size) {
        return reject(MeshletFormatError::Truncated, "chunk table past the end of the file");
    }
    // The checksum covers everything, so a corrupt table cannot send the reader astray below.
    const u64 storedHash = Reader{data + size - kTrailerBytes}.u64v();
    if (storedHash != meshlet_fnv1a64(data, size - kTrailerBytes)) {
        return reject(MeshletFormatError::ChecksumMismatch, "checksum mismatch");
    }

    u64 seen[kChunkKinds] = {};
    u64 cursor = tableEnd;
    for (u32 c = 0; c < chunkCount; ++c) {
        Reader e{data + kHeaderBytes + static_cast<usize>(c) * kChunkEntryBytes};
        const u32 id = e.u32v();
        const u32 elementBytes = e.u32v();
        const u32 elementCount = e.u32v();
        const u32 chunkReserved = e.u32v();
        const u64 offset = e.u64v();
        const u64 byteSize = e.u64v();
        if (chunkReserved != 0u || offset % kChunkAlign != 0u || offset < cursor ||
            byteSize != static_cast<u64>(elementBytes) * elementCount) {
            return reject(MeshletFormatError::BadChunkTable, "chunk " + std::to_string(c) + " entry malformed");
        }
        if (offset + byteSize > size - kTrailerBytes) {
            return reject(MeshletFormatError::Truncated, "chunk " + std::to_string(c) + " payload past the end");
        }
        for (u64 gap = cursor; gap < offset; ++gap) {
            if (data[gap] != 0u) {
                return reject(MeshletFormatError::BadChunkTable, "non-zero padding between chunks");
            }
        }
        cursor = offset + byteSize;
        u32 kind = kChunkKinds;
        for (u32 k = 0; k < kChunkKinds; ++k) {
            if (kChunks[k].fourcc == id) {
                kind = k;
            }
        }
        if (kind == kChunkKinds) {
            continue; // unknown chunk from a newer minor version
        }
        if (seen[kind] != 0u) {
            return reject(MeshletFormatError::DuplicateChunk, "chunk appears twice");
        }
        seen[kind] = 1u;
        const u32 expectedCount = kind == kQprm   ? 1u
                                  : kind == kSubm ? submeshCount
                                  : kind == kMshl ? meshletCount
                                  : kind == kMvrt ? meshletVertexCount
                                  : kind == kMtri ? triangleCount
                                                  : vertexCount;
        if (elementBytes != kChunks[kind].element_bytes || elementCount != expectedCount) {
            return reject(MeshletFormatError::BadElementSize, "chunk element size or count disagrees with the header");
        }
        if (!read_payload(Reader{data + offset}, out, kind, elementCount)) {
            return reject(MeshletFormatError::Invalid, "reserved payload field set");
        }
    }
    for (u64 tail = cursor; tail < size - kTrailerBytes; ++tail) {
        if (data[tail] != 0u) {
            return reject(MeshletFormatError::BadChunkTable, "trailing bytes after the last chunk");
        }
    }
    for (u32 k = 0; k < kChunkKinds; ++k) {
        if (kChunks[k].required && seen[k] == 0u) {
            return reject(MeshletFormatError::MissingChunk, "required chunk missing");
        }
    }
    out.version_minor = minor;
    out.flags = flags;
    out.max_vertices = maxVertices;
    out.max_triangles = maxTriangles;
    out.source_hash = sourceHash;
    std::string why;
    if (!validate_meshlet_mesh(out, &why)) {
        out = MeshletMesh{};
        set_error(error, why);
        if (code != nullptr) {
            *code = MeshletFormatError::Invalid;
        }
        return false;
    }
    return true;
}

bool write_meshlet_file(const std::string& path, const MeshletMesh& mesh, std::string* error) {
    const std::vector<u8> bytes = serialize_meshlet_mesh(mesh);
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(error, "fusemeshlet: cannot open " + path + " for writing");
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        set_error(error, "fusemeshlet: write failed for " + path);
        return false;
    }
    return true;
}

bool load_meshlet_file(const std::string& path, MeshletMesh& out, std::string* error, MeshletFormatError* code) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out = MeshletMesh{};
        return fail(error, code, MeshletFormatError::Truncated, "cannot read " + path);
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_meshlet_mesh(bytes.data(), bytes.size(), out, error, code);
}

std::string meshlet_sidecar_path(const std::string& fusemesh_path) {
    std::filesystem::path p(fusemesh_path);
    p.replace_extension(kMeshletFileExtension);
    return p.string();
}

} // namespace fuse::renderer::geometry
