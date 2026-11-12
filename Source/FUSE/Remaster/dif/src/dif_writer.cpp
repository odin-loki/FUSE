// FUSE Remaster W1.5: the .dif fixture writer (FUSE DIF profile; layout in dif.hpp). Byte-deterministic: the
// output depends only on the model and the options. Used to generate synthetic fixtures at test time.
#include <fuse/remaster/dif/dif.hpp>

#include <array>
#include <cstring>

namespace fuse::remaster::dif {
namespace {

class Writer {
public:
    std::vector<std::uint8_t> bytes;

    template <typename T>
    void raw(T v) {
        std::uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        bytes.insert(bytes.end(), b, b + sizeof(T));
    }
    void u8(std::uint32_t v) { raw(static_cast<std::uint8_t>(v)); }
    void u16(std::uint32_t v) { raw(static_cast<std::uint16_t>(v)); }
    void u32(std::uint32_t v) { raw(v); }
    void s32(std::int32_t v) { raw(v); }
    void f32(float v) { raw(v); }
    void vec3(const Vec3& v) {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    void zeros(std::size_t n) { bytes.insert(bytes.end(), n, std::uint8_t{0}); }
    void be32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) {
            bytes.push_back(static_cast<std::uint8_t>(v >> s));
        }
    }
    void chunk(const char type[4], const std::vector<std::uint8_t>& data) {
        be32(static_cast<std::uint32_t>(data.size()));
        const std::size_t start = bytes.size();
        bytes.insert(bytes.end(), type, type + 4);
        bytes.insert(bytes.end(), data.begin(), data.end());
        be32(crc32(bytes.data() + start, bytes.size() - start));
    }
    /// A small, well-formed PNG (IHDR + IDAT with a stored deflate block + IEND) of `w` x `h` grey pixels.
    void png(std::uint32_t w, std::uint32_t h, std::uint8_t grey) {
        static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        bytes.insert(bytes.end(), sig, sig + 8);
        std::vector<std::uint8_t> ihdr;
        auto be = [](std::vector<std::uint8_t>& d, std::uint32_t v) {
            for (int s = 24; s >= 0; s -= 8) d.push_back(static_cast<std::uint8_t>(v >> s));
        };
        be(ihdr, w);
        be(ihdr, h);
        ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0}); // 8-bit greyscale
        chunk("IHDR", ihdr);
        std::vector<std::uint8_t> raw;
        for (std::uint32_t y = 0; y < h; ++y) {
            raw.push_back(0); // filter none
            raw.insert(raw.end(), w, grey);
        }
        std::vector<std::uint8_t> z = {0x78, 0x01, 0x01}; // zlib header, final stored block
        const auto len = static_cast<std::uint16_t>(raw.size());
        z.push_back(static_cast<std::uint8_t>(len));
        z.push_back(static_cast<std::uint8_t>(len >> 8));
        z.push_back(static_cast<std::uint8_t>(~len));
        z.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(~len) >> 8));
        z.insert(z.end(), raw.begin(), raw.end());
        std::uint32_t a = 1, b = 0; // adler32
        for (std::uint8_t c : raw) {
            a = (a + c) % 65521u;
            b = (b + a) % 65521u;
        }
        be(z, (b << 16) | a);
        chunk("IDAT", z);
        chunk("IEND", {});
    }

private:
    static std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
        static const std::array<std::uint32_t, 256> table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
};

void writeInterior(Writer& w, const Interior& it, const WriteOptions& opt) {
    const std::uint32_t v = it.version;
    const bool wide = v >= 13;
    w.u32(v);
    w.u32(it.detailLevel);
    w.u32(it.minPixels);
    w.vec3(it.boundsMin);
    w.vec3(it.boundsMax);
    w.vec3(it.sphereCenter);
    w.f32(it.sphereRadius);
    w.u8(it.hasAlarmState ? 1 : 0);
    w.u32(0); // light state entries

    w.u32(static_cast<std::uint32_t>(it.normals.size()));
    for (const Vec3& n : it.normals) w.vec3(n);
    w.u32(static_cast<std::uint32_t>(it.planes.size()));
    for (const Plane& p : it.planes) {
        w.u16(p.normalIndex);
        w.f32(p.distance);
    }
    w.u32(static_cast<std::uint32_t>(it.points.size()));
    for (const Vec3& p : it.points) w.vec3(p);
    w.u32(static_cast<std::uint32_t>(it.points.size())); // point visibility: all visible
    w.bytes.insert(w.bytes.end(), it.points.size(), std::uint8_t{0xFF});
    w.u32(0); // texgen equations
    w.u32(0); // BSP nodes
    w.u32(1); // one solid leaf holding every surface
    w.u32(0);
    w.u16(static_cast<std::uint32_t>(it.surfaces.size()));

    w.u8(1); // material list version
    w.u32(static_cast<std::uint32_t>(it.materials.size()));
    for (const std::string& m : it.materials) {
        w.u8(static_cast<std::uint32_t>(m.size()));
        w.bytes.insert(w.bytes.end(), m.begin(), m.end());
    }

    w.u32(static_cast<std::uint32_t>(it.windings.size()));
    for (std::uint32_t x : it.windings) w.u32(x);
    w.u32(static_cast<std::uint32_t>(it.surfaces.size())); // winding indices mirror the surfaces
    for (const Surface& s : it.surfaces) {
        w.u32(s.windingStart);
        w.u32(s.windingCount);
    }
    if (v >= 12) {
        w.u32(0); // edges
    }
    w.u32(1); // one zone holding every surface
    w.u16(0);
    w.u16(0);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(it.surfaces.size()));
    if (v >= 12) {
        w.u32(0);
        w.u32(0);
    }
    w.u32(static_cast<std::uint32_t>(it.surfaces.size())); // zone surfaces
    for (std::size_t i = 0; i < it.surfaces.size(); ++i) w.u16(static_cast<std::uint32_t>(i));
    w.u32(0); // zone portal list
    w.u32(0); // portals

    w.u32(static_cast<std::uint32_t>(it.surfaces.size()));
    for (std::size_t i = 0; i < it.surfaces.size(); ++i) {
        const Surface& s = it.surfaces[i];
        w.u32(s.windingStart);
        if (wide) w.u32(s.windingCount);
        else w.u8(s.windingCount);
        w.u16(static_cast<std::uint32_t>(s.planeIndex | (s.planeFlipped ? kPlaneFlipBit : 0)));
        w.u16(s.textureIndex);
        w.u32(s.texGenIndex);
        w.u8(s.surfaceFlags);
        w.u32(s.fanMask);
        // lightmap info: final word, texgen distances, light count, state info start, map rect
        w.u16(0);
        w.f32(0.f);
        w.f32(0.f);
        w.u16(0);
        w.u32(0);
        const std::uint32_t lm = opt.dummyLightmaps ? static_cast<std::uint32_t>(i % opt.dummyLightmaps) : 0;
        if (wide) {
            w.u32(lm * 4);
            w.u32(0);
            w.u32(4);
            w.u32(4);
        } else {
            w.u8(lm * 4);
            w.u8(0);
            w.u8(4);
            w.u8(4);
        }
    }
    const std::uint32_t lmIndex = opt.dummyLightmaps ? static_cast<std::uint32_t>(it.surfaces.size()) : 0;
    for (int pass = 0; pass < 2; ++pass) { // normal + alarm lightmap indices
        w.u32(pass == 0 ? lmIndex : 0);
        for (std::uint32_t i = 0; pass == 0 && i < lmIndex; ++i) {
            const std::uint32_t idx = i % opt.dummyLightmaps;
            if (wide) w.u32(idx);
            else w.u8(idx);
        }
    }
    w.u32(0); // null surfaces
    w.u32(opt.dummyLightmaps);
    for (std::uint32_t i = 0; i < opt.dummyLightmaps; ++i) {
        w.png(4, 4, static_cast<std::uint8_t>(0x40 + 0x20 * (i % 4)));
        if (v >= 11) w.png(4, 4, 0x80);
        w.u8(1);
    }
    w.u32(0); // solid leaf surfaces (the leaf above starts at 0 and lists none explicitly) [profile]
    w.u32(0); // animated lights
    w.u32(0); // light states
    w.u32(0); // state datas
    w.u32(0); // state data buffer
    w.u32(0); //   flags
    w.u32(0); // name buffer
    w.u32(0); // sub-objects

    w.u32(static_cast<std::uint32_t>(it.hulls.size()));
    for (const ConvexHull& h : it.hulls) {
        w.u32(h.hullStart);
        w.u16(h.hullCount);
        w.f32(h.min.x);
        w.f32(h.max.x);
        w.f32(h.min.y);
        w.f32(h.max.y);
        w.f32(h.min.z);
        w.f32(h.max.z);
        w.u32(h.surfaceStart);
        w.u16(h.surfaceCount);
        w.u32(h.planeStart);
        w.zeros(12); // poly list starts
        if (v >= 12) w.u8(0);
    }
    w.u32(0); // hull emit strings
    w.u32(static_cast<std::uint32_t>(it.hullIndices.size()));
    for (std::uint32_t x : it.hullIndices) w.u32(x);
    w.u32(static_cast<std::uint32_t>(it.hullPlaneIndices.size()));
    for (std::uint16_t x : it.hullPlaneIndices) w.u16(x);
    w.u32(0); // hull emit string indices
    w.u32(static_cast<std::uint32_t>(it.hullSurfaceIndices.size()));
    for (std::uint32_t x : it.hullSurfaceIndices) w.u32(x);
    w.u32(0); // poly list planes
    w.u32(0); // poly list points
    w.u32(0); // poly list strings
    for (std::uint32_t i = 0; i < 256; ++i) { // coord bins: every bin lists all hulls
        w.u32(0);
        w.u32(static_cast<std::uint32_t>(it.hulls.size()));
    }
    w.u32(static_cast<std::uint32_t>(it.hulls.size()));
    for (std::size_t i = 0; i < it.hulls.size(); ++i) w.u16(static_cast<std::uint32_t>(i));
    w.u32(0); // coord bin mode
    w.bytes.insert(w.bytes.end(), {32, 32, 32, 255, 0, 0, 0, 255}); // base / alarm ambient
    if (v >= 10) w.u32(0); // static meshes
    if (v >= 11) {
        w.u32(0); // tex normals
        w.u32(0); // tex matrices
        w.u32(0); // tex matrix indices
        w.u32(0); // extended lightmap data
    }
}

} // namespace

std::vector<std::uint8_t> writeDif(const InteriorResource& resource, const WriteOptions& options) {
    if (resource.detailLevels.empty()) {
        return {};
    }
    for (const Interior& it : resource.detailLevels) {
        if (!validate(it).empty()) {
            return {};
        }
        if (it.version < 13) { // narrow (U8) winding counts
            for (const Surface& s : it.surfaces) {
                if (s.windingCount > 255) return {};
            }
        }
        for (const std::string& m : it.materials) {
            if (m.size() > 255) return {};
        }
    }
    Writer w;
    w.u32(resource.version);
    w.u8(options.preview ? 1 : 0);
    if (options.preview) {
        w.png(8, 8, 0x7F);
    }
    w.u32(static_cast<std::uint32_t>(resource.detailLevels.size()));
    for (const Interior& it : resource.detailLevels) {
        writeInterior(w, it, options);
    }
    for (int i = 0; i < 5; ++i) {
        w.u32(0); // sub-objects, triggers, path followers, force fields, AI special nodes
    }
    return std::move(w.bytes);
}

} // namespace fuse::remaster::dif
