// FUSE Remaster W1.5: the .dif reader (FUSE DIF profile; layout in dif.hpp). Clean room, never throws.
#include <fuse/remaster/dif/dif.hpp>

#include <cstring>
#include <string>

namespace fuse::remaster::dif {
namespace {

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : m_b(b) {}

    bool failed() const { return !m_error.empty(); }
    const std::string& error() const { return m_error; }
    std::size_t pos() const { return m_pos; }
    std::size_t left() const { return m_b.size() - m_pos; }

    void fail(const std::string& what) {
        if (m_error.empty()) {
            m_error = what + " (at byte " + std::to_string(m_pos) + ")";
        }
    }
    bool need(std::size_t n, const char* what) {
        if (failed()) {
            return false;
        }
        if (n > left()) {
            fail(std::string("truncated: ") + what);
            return false;
        }
        return true;
    }
    template <typename T>
    T raw(const char* what) {
        T v{};
        if (!need(sizeof(T), what)) {
            return v;
        }
        std::memcpy(&v, m_b.data() + m_pos, sizeof(T)); // little-endian hosts only (x86-64 / ARM64 LE)
        m_pos += sizeof(T);
        return v;
    }
    std::uint8_t u8(const char* w) { return raw<std::uint8_t>(w); }
    std::uint16_t u16(const char* w) { return raw<std::uint16_t>(w); }
    std::uint32_t u32(const char* w) { return raw<std::uint32_t>(w); }
    std::int32_t s32(const char* w) { return raw<std::int32_t>(w); }
    float f32(const char* w) { return raw<float>(w); }
    Vec3 vec3(const char* w) {
        Vec3 v;
        v.x = f32(w);
        v.y = f32(w);
        v.z = f32(w);
        return v;
    }
    void skip(std::size_t n, const char* what) {
        if (need(n, what)) {
            m_pos += n;
        }
    }
    /// A U32 element count whose elements take at least `elemBytes` each: rejects counts the file cannot hold.
    std::uint32_t count(std::size_t elemBytes, const char* what) {
        const std::uint32_t n = u32(what);
        if (failed()) {
            return 0;
        }
        if (elemBytes > 0 && static_cast<std::uint64_t>(n) * elemBytes > left()) {
            fail(std::string("count of ") + what + " (" + std::to_string(n) + ") exceeds the file");
            return 0;
        }
        return n;
    }
    std::string str(const char* what) {
        const std::uint8_t n = u8(what);
        if (!need(n, what)) {
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_b.data() + m_pos), n);
        m_pos += n;
        return s;
    }
    /// Skips one PNG: signature, then chunks (U32 big-endian length, type, data, CRC) through IEND.
    void skipPng(const char* what) {
        static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (!need(8, what)) {
            return;
        }
        if (std::memcmp(m_b.data() + m_pos, sig, 8) != 0) {
            fail(std::string(what) + ": not a PNG");
            return;
        }
        m_pos += 8;
        for (int chunks = 0; chunks < 100000; ++chunks) {
            if (!need(8, what)) {
                return;
            }
            const std::uint8_t* p = m_b.data() + m_pos;
            const std::uint32_t len = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
            const bool iend = std::memcmp(p + 4, "IEND", 4) == 0;
            m_pos += 8;
            skip(static_cast<std::size_t>(len) + 4u, what);
            if (failed() || iend) {
                return;
            }
        }
        fail(std::string(what) + ": too many PNG chunks");
    }

private:
    std::span<const std::uint8_t> m_b;
    std::size_t m_pos = 0;
    std::string m_error;
};

bool readInterior(Reader& r, Interior& it) {
    it.version = r.u32("interior version");
    if (r.failed()) {
        return false;
    }
    if (it.version > kMaxInteriorVersion) {
        r.fail("unsupported interior version " + std::to_string(it.version) + " (supported 0.." +
               std::to_string(kMaxInteriorVersion) + ")");
        return false;
    }
    const std::uint32_t v = it.version;
    it.detailLevel = r.u32("detail level");
    it.minPixels = r.u32("min pixels");
    it.boundsMin = r.vec3("bounding box");
    it.boundsMax = r.vec3("bounding box");
    it.sphereCenter = r.vec3("bounding sphere");
    it.sphereRadius = r.f32("bounding sphere");
    it.hasAlarmState = r.u8("alarm state") != 0;
    r.u32("light state entries");

    std::uint32_t n = r.count(12, "normals");
    it.normals.resize(n);
    for (auto& x : it.normals) x = r.vec3("normals");
    n = r.count(6, "planes");
    it.planes.resize(n);
    for (auto& p : it.planes) {
        p.normalIndex = r.u16("planes");
        p.distance = r.f32("planes");
    }
    n = r.count(12, "points");
    it.points.resize(n);
    for (auto& x : it.points) x = r.vec3("points");
    r.skip(r.count(1, "point visibility"), "point visibility");
    r.skip(std::size_t(r.count(32, "texgen equations")) * 32u, "texgen equations");
    const std::size_t bspNode = v >= 14 ? 10u : 6u;
    r.skip(std::size_t(r.count(bspNode, "BSP nodes")) * bspNode, "BSP nodes");
    r.skip(std::size_t(r.count(6, "BSP solid leaves")) * 6u, "BSP solid leaves");

    r.u8("material list version");
    n = r.count(1, "materials");
    it.materials.resize(n);
    for (auto& m : it.materials) m = r.str("material name");

    n = r.count(4, "windings");
    it.windings.resize(n);
    for (auto& w : it.windings) w = r.u32("windings");
    r.skip(std::size_t(r.count(8, "winding indices")) * 8u, "winding indices");
    if (v >= 12) {
        r.skip(std::size_t(r.count(16, "edges")) * 16u, "edges");
    }
    const std::size_t zone = v >= 12 ? 20u : 12u;
    r.skip(std::size_t(r.count(zone, "zones")) * zone, "zones");
    r.skip(std::size_t(r.count(2, "zone surfaces")) * 2u, "zone surfaces");
    r.skip(std::size_t(r.count(2, "zone portals")) * 2u, "zone portals");
    r.skip(std::size_t(r.count(12, "portals")) * 12u, "portals");

    const bool wide = v >= 13;
    n = r.count(wide ? 53 : 38, "surfaces");
    it.surfaces.resize(n);
    for (auto& s : it.surfaces) {
        s.windingStart = r.u32("surface");
        s.windingCount = wide ? r.u32("surface") : r.u8("surface");
        const std::uint16_t plane = r.u16("surface");
        s.planeIndex = static_cast<std::uint16_t>(plane & ~kPlaneFlipBit);
        s.planeFlipped = (plane & kPlaneFlipBit) != 0;
        s.textureIndex = r.u16("surface");
        s.texGenIndex = r.u32("surface");
        s.surfaceFlags = r.u8("surface");
        s.fanMask = r.u32("surface");
        r.skip(2 + 4 + 4 + 2 + 4, "surface lightmap info");
        r.skip(wide ? 16u : 4u, "surface lightmap rect");
    }
    const std::size_t lmIndex = wide ? 4u : 1u;
    r.skip(std::size_t(r.count(lmIndex, "normal lightmap indices")) * lmIndex, "normal lightmap indices");
    r.skip(std::size_t(r.count(lmIndex, "alarm lightmap indices")) * lmIndex, "alarm lightmap indices");
    const std::size_t nullSurf = wide ? 11u : 8u;
    it.nullSurfaces = r.count(nullSurf, "null surfaces");
    r.skip(std::size_t(it.nullSurfaces) * nullSurf, "null surfaces");

    // Lightmaps: skipped, never decoded (W1.5: "lightmaps ignored").
    it.lightmapsSkipped = r.count(17, "lightmaps");
    for (std::uint32_t i = 0; i < it.lightmapsSkipped && !r.failed(); ++i) {
        r.skipPng("lightmap");
        if (v >= 11) {
            r.skipPng("light direction map");
        }
        r.u8("lightmap keep flag");
    }
    r.skip(std::size_t(r.count(4, "solid leaf surfaces")) * 4u, "solid leaf surfaces");
    r.skip(std::size_t(r.count(16, "animated lights")) * 16u, "animated lights");
    r.skip(std::size_t(r.count(13, "light states")) * 13u, "light states");
    r.skip(std::size_t(r.count(10, "state datas")) * 10u, "state datas");
    const std::uint32_t sdb = r.count(1, "state data buffer");
    r.u32("state data flags");
    r.skip(sdb, "state data buffer");
    r.skip(r.count(1, "name buffer"), "name buffer");
    if (const std::uint32_t subs = r.u32("sub-objects"); subs != 0 && !r.failed()) {
        r.fail("interior sub-objects are not supported (" + std::to_string(subs) + ")");
    }

    const std::size_t hull = v >= 12 ? 53u : 52u;
    n = r.count(hull, "convex hulls");
    it.hulls.resize(n);
    for (auto& h : it.hulls) {
        h.hullStart = r.u32("hull");
        h.hullCount = r.u16("hull");
        h.min.x = r.f32("hull");
        h.max.x = r.f32("hull");
        h.min.y = r.f32("hull");
        h.max.y = r.f32("hull");
        h.min.z = r.f32("hull");
        h.max.z = r.f32("hull");
        h.surfaceStart = r.u32("hull");
        h.surfaceCount = r.u16("hull");
        h.planeStart = r.u32("hull");
        r.skip(12, "hull poly list starts");
        if (v >= 12) {
            r.u8("hull static mesh flag");
        }
    }
    r.skip(r.count(1, "hull emit strings"), "hull emit strings");
    n = r.count(4, "hull indices");
    it.hullIndices.resize(n);
    for (auto& x : it.hullIndices) x = r.u32("hull indices");
    n = r.count(2, "hull plane indices");
    it.hullPlaneIndices.resize(n);
    for (auto& x : it.hullPlaneIndices) x = r.u16("hull plane indices");
    r.skip(std::size_t(r.count(4, "hull emit string indices")) * 4u, "hull emit string indices");
    n = r.count(4, "hull surface indices");
    it.hullSurfaceIndices.resize(n);
    for (auto& x : it.hullSurfaceIndices) x = r.u32("hull surface indices");
    r.skip(std::size_t(r.count(2, "poly list planes")) * 2u, "poly list planes");
    r.skip(std::size_t(r.count(4, "poly list points")) * 4u, "poly list points");
    r.skip(r.count(1, "poly list strings"), "poly list strings");
    r.skip(256u * 8u, "coord bins");
    r.skip(std::size_t(r.count(2, "coord bin indices")) * 2u, "coord bin indices");
    r.u32("coord bin mode");
    r.skip(8, "ambient colours");
    if (v >= 10) {
        if (const std::uint32_t meshes = r.u32("static meshes"); meshes != 0 && !r.failed()) {
            r.fail("interior static meshes are not supported (" + std::to_string(meshes) + ")");
        }
    }
    if (v >= 11) {
        r.skip(std::size_t(r.count(12, "tex normals")) * 12u, "tex normals");
        r.skip(std::size_t(r.count(12, "tex matrices")) * 12u, "tex matrices");
        r.skip(std::size_t(r.count(4, "tex matrix indices")) * 4u, "tex matrix indices");
        if (r.u32("extended lightmap data") != 0) {
            r.skip(8, "extended lightmap data");
        }
    }
    return !r.failed();
}

} // namespace

ReadResult readDif(std::span<const std::uint8_t> bytes) {
    ReadResult out;
    Reader r(bytes);
    out.resource.version = r.u32("resource version");
    if (!r.failed() && out.resource.version != kResourceVersion) {
        out.error = "unsupported interior resource version " + std::to_string(out.resource.version) + " (expected " +
                    std::to_string(kResourceVersion) + ")";
        return out;
    }
    if (r.u8("preview flag") != 0) {
        r.skipPng("preview bitmap");
    }
    // An Interior is far larger than 64 bytes; this bounds the count before allocating.
    const std::uint32_t levels = r.count(64, "detail levels");
    if (!r.failed() && levels == 0) {
        r.fail("no detail levels");
    }
    for (std::uint32_t i = 0; i < levels && !r.failed(); ++i) {
        Interior it;
        if (!readInterior(r, it)) {
            break;
        }
        for (const std::string& e : validate(it)) {
            r.fail("detail level " + std::to_string(i) + ": " + e);
            break;
        }
        out.resource.detailLevels.push_back(std::move(it));
    }
    if (r.failed()) {
        out.error = r.error();
        out.resource.detailLevels.clear();
        return out;
    }
    // Trailing game objects: read leniently (the geometry is complete).
    static const char* trailing[] = {"sub-objects", "triggers", "path followers", "force fields", "AI special nodes"};
    for (const char* name : trailing) {
        if (r.left() < 4) {
            if (r.left() != 0) {
                out.warnings.push_back("trailing bytes after the detail levels ignored");
            }
            break;
        }
        const std::uint32_t n = r.u32(name);
        if (n != 0) {
            out.warnings.push_back(std::string("trailing block '") + name + "' has " + std::to_string(n) +
                                   " entries; it and everything after it are not read");
            break;
        }
    }
    out.ok = true;
    return out;
}

} // namespace fuse::remaster::dif
