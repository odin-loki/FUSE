// WP-9.2 3DGS .ply loader / writer: see include/fuse/renderer/gsplat/gsplat_ply.hpp.
#include <fuse/renderer/gsplat/gsplat_ply.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace fuse::renderer::gsplat {

namespace {

enum class Scalar : u8 { None, I8, U8, I16, U16, I32, U32, F32, F64 };

Scalar parseScalar(const std::string& t) {
    if (t == "char" || t == "int8") return Scalar::I8;
    if (t == "uchar" || t == "uint8") return Scalar::U8;
    if (t == "short" || t == "int16") return Scalar::I16;
    if (t == "ushort" || t == "uint16") return Scalar::U16;
    if (t == "int" || t == "int32") return Scalar::I32;
    if (t == "uint" || t == "uint32") return Scalar::U32;
    if (t == "float" || t == "float32") return Scalar::F32;
    if (t == "double" || t == "float64") return Scalar::F64;
    return Scalar::None;
}

u32 scalarBytes(Scalar s) {
    switch (s) {
    case Scalar::I8:
    case Scalar::U8: return 1u;
    case Scalar::I16:
    case Scalar::U16: return 2u;
    case Scalar::I32:
    case Scalar::U32:
    case Scalar::F32: return 4u;
    case Scalar::F64: return 8u;
    case Scalar::None: break;
    }
    return 0u;
}

struct Property {
    std::string name;
    Scalar type = Scalar::None;
    Scalar countType = Scalar::None; ///< list properties
    bool list = false;
};

struct Element {
    std::string name;
    u64 count = 0;
    std::vector<Property> props;
};

enum class Format { Ascii, BinaryLe, BinaryBe };

/// Reads one binary scalar (host is little endian on every supported target; big endian swaps).
f64 readBinary(const u8* p, Scalar s, bool swap) {
    u8 b[8];
    const u32 n = scalarBytes(s);
    for (u32 i = 0; i < n; ++i) {
        b[i] = swap ? p[n - 1u - i] : p[i];
    }
    switch (s) {
    case Scalar::I8: { std::int8_t v; std::memcpy(&v, b, 1); return v; }
    case Scalar::U8: return b[0];
    case Scalar::I16: { std::int16_t v; std::memcpy(&v, b, 2); return v; }
    case Scalar::U16: { u16 v; std::memcpy(&v, b, 2); return v; }
    case Scalar::I32: { i32 v; std::memcpy(&v, b, 4); return v; }
    case Scalar::U32: { u32 v; std::memcpy(&v, b, 4); return v; }
    case Scalar::F32: { f32 v; std::memcpy(&v, b, 4); return v; }
    case Scalar::F64: { f64 v; std::memcpy(&v, b, 8); return v; }
    case Scalar::None: break;
    }
    return 0.0;
}

f32 sigmoid(f32 x) { return 1.f / (1.f + std::exp(-x)); }

GsPlyResult fail(const std::string& message) {
    GsPlyResult r;
    r.error = message;
    return r;
}

/// Destination slot of a vertex property (-1 = skipped). Slots: 0..2 xyz, 3..5 f_dc, 6..50 f_rest,
/// 51 opacity, 52..54 scale, 55..58 rot.
int slotOf(const std::string& name, u32& restCount) {
    if (name == "x") return 0;
    if (name == "y") return 1;
    if (name == "z") return 2;
    if (name.rfind("f_dc_", 0) == 0) {
        const int i = std::atoi(name.c_str() + 5);
        return i >= 0 && i < 3 ? 3 + i : -1;
    }
    if (name.rfind("f_rest_", 0) == 0) {
        const int i = std::atoi(name.c_str() + 7);
        if (i < 0 || i >= 45) return -1;
        restCount = restCount > static_cast<u32>(i) + 1u ? restCount : static_cast<u32>(i) + 1u;
        return 6 + i;
    }
    if (name == "opacity") return 51;
    if (name.rfind("scale_", 0) == 0) {
        const int i = std::atoi(name.c_str() + 6);
        return i >= 0 && i < 3 ? 52 + i : -1;
    }
    if (name.rfind("rot_", 0) == 0) {
        const int i = std::atoi(name.c_str() + 4);
        return i >= 0 && i < 4 ? 55 + i : -1;
    }
    return -1;
}

constexpr int kSlots = 59;

void activate(const f32 (&raw)[kSlots], u32 degree, GsSplat& s) {
    s = GsSplat{};
    for (u32 i = 0; i < 3u; ++i) {
        s.position[i] = raw[i];
        s.sh[i] = raw[3u + i];
        s.scale[i] = std::exp(raw[52u + i]);
    }
    const u32 coeffs = (degree + 1u) * (degree + 1u);
    for (u32 c = 0; c < 3u; ++c) {
        for (u32 k = 1; k < coeffs; ++k) {
            s.sh[k * 3u + c] = raw[6u + c * (coeffs - 1u) + (k - 1u)];
        }
    }
    s.opacity = sigmoid(raw[51]);
    const f32 w = raw[55], x = raw[56], y = raw[57], z = raw[58];
    const f32 len = std::sqrt(w * w + x * x + y * y + z * z);
    if (len > 0.f && std::isfinite(len)) {
        s.rotation[0] = w / len;
        s.rotation[1] = x / len;
        s.rotation[2] = y / len;
        s.rotation[3] = z / len;
    }
}

} // namespace

GsPlyResult gs_load_ply(const u8* data, usize size, GsAsset& out) {
    out = GsAsset{};
    if (data == nullptr || size < 4u || std::memcmp(data, "ply", 3) != 0) {
        return fail("not a .ply file");
    }
    // Header: lines up to "end_header\n".
    usize pos = 0;
    auto nextLine = [&](std::string& line) {
        if (pos >= size) return false;
        usize end = pos;
        while (end < size && data[end] != '\n') ++end;
        line.assign(reinterpret_cast<const char*>(data) + pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = end < size ? end + 1u : end;
        return true;
    };
    std::string line;
    Format format = Format::Ascii;
    bool haveFormat = false;
    bool ended = false;
    std::vector<Element> elements;
    nextLine(line); // "ply"
    while (nextLine(line)) {
        std::istringstream ls(line);
        std::string word;
        ls >> word;
        if (word == "format") {
            std::string f;
            ls >> f;
            if (f == "ascii") format = Format::Ascii;
            else if (f == "binary_little_endian") format = Format::BinaryLe;
            else if (f == "binary_big_endian") format = Format::BinaryBe;
            else return fail("unknown format " + f);
            haveFormat = true;
        } else if (word == "element") {
            Element e;
            ls >> e.name >> e.count;
            elements.push_back(e);
        } else if (word == "property") {
            if (elements.empty()) return fail("property before element");
            Property p;
            std::string type;
            ls >> type;
            if (type == "list") {
                std::string ct, it;
                ls >> ct >> it >> p.name;
                p.list = true;
                p.countType = parseScalar(ct);
                p.type = parseScalar(it);
                if (p.countType == Scalar::None) return fail("bad list count type");
            } else {
                p.type = parseScalar(type);
                ls >> p.name;
            }
            if (p.type == Scalar::None) return fail("unknown property type " + type);
            elements.back().props.push_back(p);
        } else if (word == "end_header") {
            ended = true;
            break;
        }
    }
    if (!ended || !haveFormat) return fail("truncated header");

    const Element* vertex = nullptr;
    for (const Element& e : elements) {
        if (e.name == "vertex") vertex = &e;
    }
    if (vertex == nullptr) return fail("no vertex element");
    u32 restCount = 0;
    std::vector<int> slots;
    bool has[kSlots] = {};
    for (const Property& p : vertex->props) {
        const int s = p.list ? -1 : slotOf(p.name, restCount);
        slots.push_back(s);
        if (s >= 0) has[s] = true;
    }
    for (int s : {0, 1, 2, 3, 4, 5, 51, 52, 53, 54, 55, 56, 57, 58}) {
        if (!has[s]) return fail("missing a required 3DGS vertex property");
    }
    u32 degree = 0;
    if (restCount == 0u) degree = 0;
    else if (restCount == 9u) degree = 1;
    else if (restCount == 24u) degree = 2;
    else if (restCount == 45u) degree = 3;
    else return fail("f_rest count is not 0, 9, 24 or 45");
    for (u32 i = 0; i < restCount; ++i) {
        if (!has[6u + i]) return fail("f_rest properties are not contiguous");
    }
    if (vertex->count > (1ull << 31)) return fail("too many vertices");
    out.shDegree = degree;
    out.splats.resize(static_cast<usize>(vertex->count));

    const bool swap = format == Format::BinaryBe;
    if (format == Format::Ascii) {
        const std::string body(reinterpret_cast<const char*>(data) + pos, size - pos);
        std::istringstream in(body);
        for (const Element& e : elements) {
            for (u64 row = 0; row < e.count; ++row) {
                f32 raw[kSlots] = {};
                for (usize pi = 0; pi < e.props.size(); ++pi) {
                    const Property& p = e.props[pi];
                    f64 v = 0;
                    if (p.list) {
                        u64 n = 0;
                        if (!(in >> n)) return fail("truncated ascii body");
                        for (u64 k = 0; k < n; ++k) {
                            if (!(in >> v)) return fail("truncated ascii body");
                        }
                        continue;
                    }
                    if (!(in >> v)) return fail("truncated ascii body");
                    if (&e == vertex && slots[pi] >= 0) raw[slots[pi]] = static_cast<f32>(v);
                }
                if (&e == vertex) activate(raw, degree, out.splats[static_cast<usize>(row)]);
            }
        }
    } else {
        for (const Element& e : elements) {
            for (u64 row = 0; row < e.count; ++row) {
                f32 raw[kSlots] = {};
                for (usize pi = 0; pi < e.props.size(); ++pi) {
                    const Property& p = e.props[pi];
                    if (p.list) {
                        const u32 cb = scalarBytes(p.countType);
                        if (pos + cb > size) return fail("truncated binary body");
                        const f64 n = readBinary(data + pos, p.countType, swap);
                        pos += cb;
                        const u64 bytes = static_cast<u64>(n) * scalarBytes(p.type);
                        if (n < 0 || pos + bytes > size) return fail("truncated binary body");
                        pos += static_cast<usize>(bytes);
                        continue;
                    }
                    const u32 b = scalarBytes(p.type);
                    if (pos + b > size) return fail("truncated binary body");
                    if (&e == vertex && slots[pi] >= 0) {
                        raw[slots[pi]] = static_cast<f32>(readBinary(data + pos, p.type, swap));
                    }
                    pos += b;
                }
                if (&e == vertex) activate(raw, degree, out.splats[static_cast<usize>(row)]);
            }
        }
    }
    GsPlyResult r;
    r.ok = true;
    return r;
}

GsPlyResult gs_load_ply_file(const char* path, GsAsset& out) {
    out = GsAsset{};
    std::FILE* f = path != nullptr ? std::fopen(path, "rb") : nullptr;
    if (f == nullptr) return fail("cannot open file");
    std::vector<u8> bytes;
    u8 chunk[65536];
    for (;;) {
        const usize n = std::fread(chunk, 1, sizeof(chunk), f);
        bytes.insert(bytes.end(), chunk, chunk + n);
        if (n < sizeof(chunk)) break;
    }
    std::fclose(f);
    return gs_load_ply(bytes.data(), bytes.size(), out);
}

std::vector<u8> gs_write_ply(const GsAsset& asset) {
    const u32 degree = asset.shDegree > kGsMaxShDegree ? kGsMaxShDegree : asset.shDegree;
    const u32 coeffs = (degree + 1u) * (degree + 1u);
    const u32 rest = 3u * (coeffs - 1u);
    std::string header = "ply\nformat binary_little_endian 1.0\nelement vertex " + std::to_string(asset.splats.size()) + "\n";
    for (const char* n : {"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"}) {
        header += std::string("property float ") + n + "\n";
    }
    for (u32 i = 0; i < rest; ++i) header += "property float f_rest_" + std::to_string(i) + "\n";
    for (const char* n : {"opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"}) {
        header += std::string("property float ") + n + "\n";
    }
    header += "end_header\n";
    std::vector<u8> out(header.begin(), header.end());
    std::vector<f32> row;
    for (const GsSplat& s : asset.splats) {
        row.clear();
        row.insert(row.end(), {s.position[0], s.position[1], s.position[2], 0.f, 0.f, 0.f, s.sh[0], s.sh[1], s.sh[2]});
        for (u32 c = 0; c < 3u; ++c) {
            for (u32 k = 1; k < coeffs; ++k) row.push_back(s.sh[k * 3u + c]);
        }
        const f32 o = s.opacity <= 0.f ? 1e-7f : (s.opacity >= 1.f ? 1.f - 1e-7f : s.opacity);
        row.push_back(std::log(o / (1.f - o)));
        for (u32 i = 0; i < 3u; ++i) row.push_back(std::log(s.scale[i]));
        for (u32 i = 0; i < 4u; ++i) row.push_back(s.rotation[i]);
        const usize at = out.size();
        out.resize(at + row.size() * 4u);
        std::memcpy(out.data() + at, row.data(), row.size() * 4u);
    }
    return out;
}

} // namespace fuse::renderer::gsplat
