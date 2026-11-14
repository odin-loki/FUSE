// FUSE Remaster W1.5: DIF model helpers: validation, triangulation, areas, the box-brush fixture builder and the
// JSON summary (see dif.hpp).
#include <fuse/remaster/dif/dif.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::remaster::dif {
namespace {

bool inRange(std::uint64_t start, std::uint64_t count, std::size_t size) { return start + count <= size; }

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(Vec3 a, Vec3 b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20 || c >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

} // namespace

std::vector<std::string> validate(const Interior& it) {
    std::vector<std::string> e;
    for (std::size_t i = 0; i < it.planes.size(); ++i) {
        if (it.planes[i].normalIndex >= it.normals.size()) {
            e.push_back("plane " + std::to_string(i) + ": normal index out of range");
            return e;
        }
    }
    for (std::size_t i = 0; i < it.windings.size(); ++i) {
        if (it.windings[i] >= it.points.size()) {
            e.push_back("winding " + std::to_string(i) + ": point index out of range");
            return e;
        }
    }
    for (std::size_t i = 0; i < it.surfaces.size(); ++i) {
        const Surface& s = it.surfaces[i];
        const std::string w = "surface " + std::to_string(i) + ": ";
        if (s.windingCount < 3 || !inRange(s.windingStart, s.windingCount, it.windings.size())) {
            e.push_back(w + "winding range out of range or below 3 points");
        } else if (s.planeIndex >= it.planes.size()) {
            e.push_back(w + "plane index out of range");
        } else if (s.textureIndex >= it.materials.size()) {
            e.push_back(w + "texture index out of range");
        }
        if (!e.empty()) {
            return e;
        }
    }
    for (std::size_t i = 0; i < it.hulls.size(); ++i) {
        const ConvexHull& h = it.hulls[i];
        if (!inRange(h.hullStart, h.hullCount, it.hullIndices.size()) ||
            !inRange(h.surfaceStart, h.surfaceCount, it.hullSurfaceIndices.size()) ||
            !inRange(h.planeStart, h.surfaceCount, it.hullPlaneIndices.size())) {
            e.push_back("hull " + std::to_string(i) + ": index range out of range");
            return e;
        }
    }
    if (std::any_of(it.hullIndices.begin(), it.hullIndices.end(), [&](std::uint32_t x) { return x >= it.points.size(); }) ||
        std::any_of(it.hullPlaneIndices.begin(), it.hullPlaneIndices.end(),
                    [&](std::uint16_t x) { return (x & ~kPlaneFlipBit) >= it.planes.size(); }) ||
        std::any_of(it.hullSurfaceIndices.begin(), it.hullSurfaceIndices.end(),
                    [&](std::uint32_t x) { return x >= it.surfaces.size(); })) {
        e.push_back("hull index tables: index out of range");
    }
    return e;
}

std::vector<Triangle> triangulate(const Interior& it) {
    std::vector<Triangle> out;
    if (!validate(it).empty()) {
        return out;
    }
    for (std::size_t si = 0; si < it.surfaces.size(); ++si) {
        const Surface& s = it.surfaces[si];
        const std::uint32_t* w = it.windings.data() + s.windingStart;
        for (std::uint32_t k = 0; k + 2 < s.windingCount; ++k) {
            const bool odd = (k & 1u) != 0;
            Triangle t;
            t.a = it.points[odd ? w[k + 1] : w[k]];
            t.b = it.points[odd ? w[k] : w[k + 1]];
            t.c = it.points[w[k + 2]];
            t.surface = static_cast<std::uint32_t>(si);
            t.material = s.textureIndex;
            out.push_back(t);
        }
    }
    return out;
}

double triangleArea(const Triangle& t) {
    const Vec3 c = cross(sub(t.b, t.a), sub(t.c, t.a));
    return 0.5 * std::sqrt(dot(c, c));
}

double totalTriangleArea(const Interior& it) {
    double a = 0.0;
    for (const Triangle& t : triangulate(it)) {
        a += triangleArea(t);
    }
    return a;
}

std::size_t brushCount(const Interior& it) { return it.hulls.size(); }

void addBoxBrush(Interior& it, Vec3 mn, Vec3 mx, const std::string& material) {
    const bool first = it.points.empty();
    auto mat = std::find(it.materials.begin(), it.materials.end(), material);
    const auto matIndex = static_cast<std::uint16_t>(mat - it.materials.begin());
    if (mat == it.materials.end()) {
        it.materials.push_back(material);
    }
    const auto base = static_cast<std::uint32_t>(it.points.size());
    for (std::uint32_t i = 0; i < 8; ++i) { // corner bits: x = 1, y = 2, z = 4
        it.points.push_back({(i & 1u) ? mx.x : mn.x, (i & 2u) ? mx.y : mn.y, (i & 4u) ? mx.z : mn.z});
    }
    struct Face {
        Vec3 n;
        std::uint32_t c[4]; // a cycle around the face
    };
    const Face faces[6] = {{{-1, 0, 0}, {0, 2, 6, 4}}, {{1, 0, 0}, {1, 3, 7, 5}}, {{0, -1, 0}, {0, 1, 5, 4}},
                           {{0, 1, 0}, {2, 3, 7, 6}},  {{0, 0, -1}, {0, 1, 3, 2}}, {{0, 0, 1}, {4, 5, 7, 6}}};
    ConvexHull hull;
    hull.hullStart = static_cast<std::uint32_t>(it.hullIndices.size());
    hull.hullCount = 8;
    hull.min = mn;
    hull.max = mx;
    hull.surfaceStart = static_cast<std::uint32_t>(it.hullSurfaceIndices.size());
    hull.surfaceCount = 6;
    hull.planeStart = static_cast<std::uint32_t>(it.hullPlaneIndices.size());
    for (std::uint32_t i = 0; i < 8; ++i) {
        it.hullIndices.push_back(base + i);
    }
    for (const Face& f : faces) {
        auto nIt = std::find(it.normals.begin(), it.normals.end(), f.n);
        const auto nIndex = static_cast<std::uint16_t>(nIt - it.normals.begin());
        if (nIt == it.normals.end()) {
            it.normals.push_back(f.n);
        }
        const Vec3 p0 = it.points[base + f.c[0]];
        const Plane plane{nIndex, static_cast<float>(-dot(f.n, p0))}; // n . p + d = 0
        auto pIt = std::find(it.planes.begin(), it.planes.end(), plane);
        const auto pIndex = static_cast<std::uint16_t>(pIt - it.planes.begin());
        if (pIt == it.planes.end()) {
            it.planes.push_back(plane);
        }
        std::uint32_t cyc[4] = {f.c[0], f.c[1], f.c[2], f.c[3]};
        const Vec3 a = it.points[base + cyc[0]], b = it.points[base + cyc[1]], c = it.points[base + cyc[2]];
        if (dot(cross(sub(b, a), sub(c, a)), f.n) < 0.0) { // counter-clockwise seen from outside
            std::swap(cyc[1], cyc[3]);
        }
        Surface s;
        s.windingStart = static_cast<std::uint32_t>(it.windings.size());
        s.windingCount = 4;
        for (std::uint32_t k : {cyc[0], cyc[1], cyc[3], cyc[2]}) { // strip order of the quad
            it.windings.push_back(base + k);
        }
        s.planeIndex = pIndex;
        s.textureIndex = matIndex;
        s.fanMask = 0xF;
        it.hullSurfaceIndices.push_back(static_cast<std::uint32_t>(it.surfaces.size()));
        it.hullPlaneIndices.push_back(pIndex);
        it.surfaces.push_back(s);
    }
    it.hulls.push_back(hull);
    if (first) {
        it.boundsMin = mn;
        it.boundsMax = mx;
    } else {
        it.boundsMin = {std::min(it.boundsMin.x, mn.x), std::min(it.boundsMin.y, mn.y), std::min(it.boundsMin.z, mn.z)};
        it.boundsMax = {std::max(it.boundsMax.x, mx.x), std::max(it.boundsMax.y, mx.y), std::max(it.boundsMax.z, mx.z)};
    }
    it.sphereCenter = {(it.boundsMin.x + it.boundsMax.x) * 0.5f, (it.boundsMin.y + it.boundsMax.y) * 0.5f,
                       (it.boundsMin.z + it.boundsMax.z) * 0.5f};
    const Vec3 half = sub(it.boundsMax, it.sphereCenter);
    it.sphereRadius = static_cast<float>(std::sqrt(dot(half, half)));
}

std::string summaryJson(const InteriorResource& r) {
    std::string out = "{\n  \"schema\": \"fuse.remaster.dif-summary/1\",\n  \"resource_version\": " +
                      std::to_string(r.version) + ",\n  \"detail_levels\": [";
    for (std::size_t i = 0; i < r.detailLevels.size(); ++i) {
        const Interior& it = r.detailLevels[i];
        const std::vector<Triangle> tris = triangulate(it);
        double area = 0.0;
        for (const Triangle& t : tris) {
            area += triangleArea(t);
        }
        char areaText[64];
        std::snprintf(areaText, sizeof(areaText), "%.6f", area);
        out += i ? ",\n    {" : "\n    {";
        out += "\"version\": " + std::to_string(it.version) + ", \"detail_level\": " + std::to_string(it.detailLevel) +
               ", \"brushes\": " + std::to_string(brushCount(it)) + ", \"surfaces\": " + std::to_string(it.surfaces.size()) +
               ", \"triangles\": " + std::to_string(tris.size()) + ", \"area\": " + areaText +
               ", \"points\": " + std::to_string(it.points.size()) + ", \"planes\": " + std::to_string(it.planes.size()) +
               ", \"lightmaps_skipped\": " + std::to_string(it.lightmapsSkipped) + ", \"materials\": [";
        for (std::size_t m = 0; m < it.materials.size(); ++m) {
            out += (m ? ", " : "") + jsonString(it.materials[m]);
        }
        out += "]}";
    }
    out += r.detailLevels.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

} // namespace fuse::remaster::dif
