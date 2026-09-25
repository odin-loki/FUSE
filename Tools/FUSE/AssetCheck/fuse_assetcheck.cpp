// fuse_assetcheck: FUSE_ASSET_PLAN §5.3 validation gates (Wave 0 task W0.6). CPU only.
//
//   fuse_assetcheck check <asset.json> [--gates g1,g2,...]   run gates on one asset descriptor
//   fuse_assetcheck fixture-test --gate <gate> --scratch DIR  write the gate's passing and failing
//                                                             fixtures to DIR, check them from disk
//   fuse_assetcheck list                                      print the gates
//
// Exit codes: 0 = every selected gate passed (fixture-test: pass fixture passed and every failing
// fixture was rejected by that gate); 1 = violations; 2 = usage / unreadable input / fixture-test broken.
//
// Asset descriptor (JSON, paths relative to the descriptor):
//   { "schema": 1, "id": "<class>/<biome|style>/<name>_<variant>", "class": "rock", "category": "boulder",
//     "units": "m", "up": "z", "tiling": false, "dag": false, "impostor": false, "hero": false,
//     "texel_density": 512,                                    (optional override, px/m)
//     "material": {"metal": "none|mixed|full", "emissive": false},
//     "textures": [{"map": "albedo|normal|orm|height|emissive", "path": "<base>_<map>.png",
//                   "colour_space": "srgb|linear", "format": "BC1|BC4|BC5|BC6H|BC7"}],
//     "lods": [{"path": "<base>_lod0.fusemesh", "error": 0.0}, ...] }
// Textures are source images (PNG / TGA / JPEG via stb_image); ORM packs occlusion (R), roughness (G),
// metal (B). Meshes are .fusemesh FMSH v1 (Tools/FUSE/Cook/src/mesh_cook.cpp layout) or Wavefront OBJ.
//
// Gates (§5.3 names; the rules are §1.2 budgets, §1.3 formats and §1.6 calibration):
//   naming          id = <class>/<biome|style>/<name>_<variant>; textures <base>_<map>.*; LODs <base>_lod<N>.*
//   budget_mesh     LOD0 triangles vs the §1.2 class budget (T0, or DAG source when "dag"); LOD count
//   lod_chain       every LOD reduces triangles by >= 40 %; screen-space error recorded and strictly increasing
//   budget_texture  power of two, resolution <= class maximum (one more mip for "hero"), format per map (§1.3)
//   texel_density   per UV island px/m vs class target (§1.2 texel density rule) within +-50 %
//   normal_map      decoded vectors unit length +-0.05, mean Z > 0.7, OpenGL (+Y) convention (integrability)
//   color_space     albedo / emissive sRGB, data maps linear; albedo 30-240 sRGB (dielectric), 180-255
//                   (metal); no baked AO / lighting (block luminance vs AO correlation < 0.5)
//   pbr_sanity      roughness non-constant (std >= 0.03); metal mask binary-ish and consistent with the
//                   material; emissive map only on emissive materials
//   uv_bounds       UVs finite, inside [0,1] unless "tiling"; no collapsed (zero-area) UV triangles
//   geometry        units = m; real-world height vs category range; pivot at base; up axis declared;
//                   no degenerate / non-finite triangles

#include "../Lint/fuse_json_mini.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(FUSE_ASSETCHECK_HAS_STB)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#include "stb_image.h"
#endif

namespace fs = std::filesystem;
using fuse::tools::JsonValue;

namespace {

using Violations = std::vector<std::string>;

const std::vector<std::string>& gateNames() {
    static const std::vector<std::string> g = {"naming",       "budget_mesh", "lod_chain",   "budget_texture", "texel_density",
                                               "normal_map",   "color_space", "pbr_sanity",  "uv_bounds",      "geometry"};
    return g;
}

// ---- §1.2 class table ----------------------------------------------------------------------------------

struct ClassBudget {
    const char* name;
    std::uint32_t triT0;   ///< LOD0 triangles, T0 upper bound
    std::uint32_t triDag;  ///< LOD0 triangles when a cluster DAG streams the asset (0 = no DAG row)
    std::uint32_t minLods; ///< LOD levels including LOD0 (§1.2 "LODs" column)
    std::uint32_t maxTex;  ///< largest texture side at the Standard tier
    double density;        ///< px/m target at LOD0; 0 = n/a (atlas-shared / tiled)
};

const ClassBudget* findClass(const std::string& name) {
    static const ClassBudget table[] = {
        {"hero_character", 70000, 120000, 4, 2048, 1024}, {"crowd_human", 15000, 0, 3, 1024, 512},
        {"large_creature", 100000, 250000, 4, 2048, 512}, {"small_fauna", 3000, 0, 2, 512, 0},
        {"tree", 60000, 0, 3, 2048, 512},                 {"shrub", 8000, 0, 2, 2048, 0},
        {"grass", 40, 0, 1, 1024, 0},                     {"rock", 10000, 50000, 3, 1024, 512},
        {"cliff", 30000, 200000, 4, 1024, 0},             {"arch", 5000, 20000, 3, 2048, 512},
        {"building", 100000, 500000, 4, 2048, 512},       {"prop", 3000, 0, 2, 2048, 512},
        {"handheld", 10000, 0, 2, 1024, 1024},            {"vehicle", 60000, 0, 4, 2048, 512},
        {"decal", 8, 0, 1, 2048, 512},                    {"mat", 0, 0, 0, 2048, 0},
        {"atlas", 0, 0, 0, 2048, 0},                      {"terrain", 0, 0, 0, 2048, 0},
    };
    for (const ClassBudget& c : table) {
        if (name == c.name) {
            return &c;
        }
    }
    return nullptr;
}

/// Real-world height ranges along the up axis, metres (§1.6 "Scale").
struct CategoryRange {
    const char* name;
    double minH, maxH;
};

const CategoryRange* findCategory(const std::string& name) {
    static const CategoryRange table[] = {
        {"door", 2.0, 2.4},      {"chair", 0.7, 1.2},     {"table", 0.6, 1.2},     {"crate", 0.3, 1.5},
        {"barrel", 0.6, 1.3},    {"human", 1.4, 2.1},     {"boulder", 0.3, 6.0},   {"pebble", 0.01, 0.3},
        {"tree", 2.0, 60.0},     {"shrub", 0.2, 4.0},     {"grass", 0.02, 2.5},    {"handheld", 0.05, 2.0},
        {"vehicle", 1.0, 4.5},   {"building", 3.0, 300.0}, {"wall", 1.0, 12.0},     {"bird", 0.05, 1.2},
        {"large_creature", 1.0, 20.0}, {"cliff", 5.0, 400.0}, {"decal", 0.0, 10.0},
    };
    for (const CategoryRange& c : table) {
        if (name == c.name) {
            return &c;
        }
    }
    return nullptr;
}

// ---- inputs ---------------------------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba; ///< 8-bit RGBA
    [[nodiscard]] const std::uint8_t* at(int x, int y) const { return &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4u]; }
};

struct Mesh {
    std::vector<double> pos; ///< xyz per vertex
    std::vector<double> uv;  ///< uv per vertex
    std::vector<std::uint32_t> idx;
    [[nodiscard]] size_t tris() const { return idx.size() / 3u; }
    [[nodiscard]] size_t verts() const { return pos.size() / 3u; }
};

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const fs::path& p, const std::string& bytes) {
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::uint32_t getU32(const std::uint8_t* d) {
    return std::uint32_t(d[0]) | (std::uint32_t(d[1]) << 8) | (std::uint32_t(d[2]) << 16) | (std::uint32_t(d[3]) << 24);
}

float getF32(const std::uint8_t* d) {
    const std::uint32_t bits = getU32(d);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::uint64_t fnv1a64(const std::uint8_t* data, size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= std::uint64_t(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr size_t kFmshHeader = 4u + 5u * 4u + 6u * 4u;
constexpr size_t kFmshSubmesh = 16u;

/// .fusemesh FMSH v1 (mesh_cook.cpp): header, submeshes, positions, normals, uv0, u32 indices, FNV-1a trailer.
bool loadFusemesh(const fs::path& p, Mesh& m, std::string& err) {
    const std::string s = readAll(p);
    const auto* d = reinterpret_cast<const std::uint8_t*>(s.data());
    if (s.size() < kFmshHeader + 8u || std::memcmp(d, "FMSH", 4) != 0) {
        err = "not a .fusemesh (magic / size)";
        return false;
    }
    const std::uint32_t version = getU32(d + 4);
    if (version != 1u) {
        err = "FMSH version " + std::to_string(version) + " not supported by fuse_assetcheck yet (v1 only)";
        return false;
    }
    const std::uint32_t vc = getU32(d + 12), ic = getU32(d + 16), sc = getU32(d + 20);
    const std::uint64_t expected = kFmshHeader + std::uint64_t(sc) * kFmshSubmesh + std::uint64_t(vc) * 32u + std::uint64_t(ic) * 4u + 8u;
    if (expected != s.size()) {
        err = "FMSH size mismatch";
        return false;
    }
    std::uint64_t trailer = 0;
    for (int i = 7; i >= 0; --i) {
        trailer = (trailer << 8) | d[s.size() - 8u + size_t(i)];
    }
    if (trailer != fnv1a64(d, s.size() - 8u)) {
        err = "FMSH checksum mismatch";
        return false;
    }
    const std::uint8_t* c = d + kFmshHeader + size_t(sc) * kFmshSubmesh;
    m.pos.resize(size_t(vc) * 3u);
    for (double& x : m.pos) {
        x = getF32(c);
        c += 4;
    }
    c += size_t(vc) * 12u; // normals
    m.uv.resize(size_t(vc) * 2u);
    for (double& x : m.uv) {
        x = getF32(c);
        c += 4;
    }
    m.idx.resize(ic);
    for (std::uint32_t& i : m.idx) {
        i = getU32(c);
        c += 4;
        if (i >= vc) {
            err = "FMSH index out of range";
            return false;
        }
    }
    if (ic % 3u != 0u) {
        err = "FMSH index count not a multiple of 3";
        return false;
    }
    return true;
}

/// Wavefront OBJ: v / vt / f (polygons fan-triangulated); each distinct v/vt pair is one vertex.
bool loadObj(const fs::path& p, Mesh& m, std::string& err) {
    std::ifstream in(p);
    if (!in) {
        err = "cannot open";
        return false;
    }
    std::vector<double> vp, vt;
    std::map<std::pair<long, long>, std::uint32_t> remap;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            double x = 0, y = 0, z = 0;
            ls >> x >> y >> z;
            vp.insert(vp.end(), {x, y, z});
        } else if (tag == "vt") {
            double u = 0, v = 0;
            ls >> u >> v;
            vt.insert(vt.end(), {u, v});
        } else if (tag == "f") {
            std::vector<std::uint32_t> face;
            std::string tok;
            while (ls >> tok) {
                long vi = 0, ti = 0;
                const size_t s1 = tok.find('/');
                vi = std::strtol(tok.substr(0, s1).c_str(), nullptr, 10);
                if (s1 != std::string::npos) {
                    const size_t s2 = tok.find('/', s1 + 1);
                    ti = std::strtol(tok.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1).c_str(), nullptr, 10);
                }
                if (vi < 0) vi = long(vp.size() / 3u) + vi + 1;
                if (ti < 0) ti = long(vt.size() / 2u) + ti + 1;
                if (vi < 1 || size_t(vi) > vp.size() / 3u || ti < 0 || size_t(ti) > vt.size() / 2u) {
                    err = "OBJ face index out of range";
                    return false;
                }
                const auto key = std::make_pair(vi, ti);
                auto it = remap.find(key);
                if (it == remap.end()) {
                    const std::uint32_t id = std::uint32_t(m.pos.size() / 3u);
                    m.pos.insert(m.pos.end(), {vp[size_t(vi - 1) * 3u], vp[size_t(vi - 1) * 3u + 1u], vp[size_t(vi - 1) * 3u + 2u]});
                    if (ti > 0) m.uv.insert(m.uv.end(), {vt[size_t(ti - 1) * 2u], vt[size_t(ti - 1) * 2u + 1u]});
                    else m.uv.insert(m.uv.end(), {0.0, 0.0});
                    it = remap.emplace(key, id).first;
                }
                face.push_back(it->second);
            }
            for (size_t k = 2; k < face.size(); ++k) {
                m.idx.insert(m.idx.end(), {face[0], face[k - 1], face[k]});
            }
        }
    }
    return true;
}

bool loadMesh(const fs::path& p, Mesh& m, std::string& err) {
    const std::string ext = p.extension().string();
    if (ext == ".fusemesh") return loadFusemesh(p, m, err);
    if (ext == ".obj") return loadObj(p, m, err);
    err = "unsupported mesh extension '" + ext + "' (.fusemesh v1 or .obj)";
    return false;
}

bool loadImage(const fs::path& p, Image& img, std::string& err) {
#if defined(FUSE_ASSETCHECK_HAS_STB)
    const std::string bytes = readAll(p);
    if (bytes.empty()) {
        err = "cannot read";
        return false;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (data == nullptr) {
        err = std::string("stb_image: ") + stbi_failure_reason();
        return false;
    }
    img.w = w;
    img.h = h;
    img.rgba.assign(data, data + size_t(w) * size_t(h) * 4u);
    stbi_image_free(data);
    return true;
#else
    // Fallback without stb: uncompressed 24/32-bit TGA only.
    const std::string s = readAll(p);
    const auto* d = reinterpret_cast<const std::uint8_t*>(s.data());
    if (s.size() < 18u || d[2] != 2u || (d[16] != 24u && d[16] != 32u)) {
        err = "only uncompressed 24/32-bit TGA without stb_image";
        return false;
    }
    img.w = d[12] | (d[13] << 8);
    img.h = d[14] | (d[15] << 8);
    const int bpp = d[16] / 8;
    const bool topLeft = (d[17] & 0x20u) != 0u;
    const size_t off = 18u + d[0];
    if (s.size() < off + size_t(img.w) * size_t(img.h) * size_t(bpp)) {
        err = "TGA truncated";
        return false;
    }
    img.rgba.resize(size_t(img.w) * size_t(img.h) * 4u);
    for (int y = 0; y < img.h; ++y) {
        const int sy = topLeft ? y : img.h - 1 - y;
        for (int x = 0; x < img.w; ++x) {
            const std::uint8_t* px = d + off + (size_t(sy) * size_t(img.w) + size_t(x)) * size_t(bpp);
            std::uint8_t* o = &img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4u];
            o[0] = px[2];
            o[1] = px[1];
            o[2] = px[0];
            o[3] = bpp == 4 ? px[3] : 255u;
        }
    }
    return true;
#endif
}

/// Uncompressed top-left-origin TGA (what the fixtures write; stb_image reads it back).
std::string encodeTga(const Image& img) {
    std::string s(18, '\0');
    s[2] = 2;
    s[12] = char(img.w & 0xff);
    s[13] = char(img.w >> 8);
    s[14] = char(img.h & 0xff);
    s[15] = char(img.h >> 8);
    s[16] = 32;
    s[17] = char(0x28); // 8 alpha bits, top-left origin
    s.reserve(18u + img.rgba.size());
    for (size_t i = 0; i < img.rgba.size(); i += 4u) {
        s.push_back(char(img.rgba[i + 2]));
        s.push_back(char(img.rgba[i + 1]));
        s.push_back(char(img.rgba[i + 0]));
        s.push_back(char(img.rgba[i + 3]));
    }
    return s;
}

// ---- asset model ----------------------------------------------------------------------------------------

struct Texture {
    std::string map, path, colourSpace, format;
    Image img;
    bool loaded = false;
};

struct Lod {
    std::string path;
    bool hasError = false;
    double error = 0.0;
    Mesh mesh;
    bool loaded = false;
};

struct Asset {
    fs::path file;
    JsonValue desc;
    std::string id, cls, category, units, up, metal;
    bool tiling = false, dag = false, impostor = false, hero = false, emissive = false;
    double densityOverride = 0.0;
    std::vector<Texture> textures;
    std::vector<Lod> lods;
    Violations loadErrors;

    [[nodiscard]] const Texture* tex(const std::string& map) const {
        for (const Texture& t : textures) {
            if (t.map == map && t.loaded) return &t;
        }
        return nullptr;
    }
};

bool loadAsset(const fs::path& file, Asset& a, std::string& err) {
    a.file = file;
    std::string perr;
    if (!fs::is_regular_file(file) || !fuse::tools::parseJson(readAll(file), a.desc, perr)) {
        err = file.generic_string() + ": cannot read asset descriptor " + perr;
        return false;
    }
    const JsonValue& d = a.desc;
    a.id = d["id"].asString();
    a.cls = d["class"].asString();
    a.category = d["category"].asString();
    a.units = d["units"].asString();
    a.up = d["up"].asString();
    a.tiling = d["tiling"].asBool();
    a.dag = d["dag"].asBool();
    a.impostor = d["impostor"].asBool();
    a.hero = d["hero"].asBool();
    a.densityOverride = d["texel_density"].asNumber(0.0);
    a.metal = d["material"]["metal"].asString("mixed");
    a.emissive = d["material"]["emissive"].asBool();
    const fs::path dir = file.parent_path();
    for (const JsonValue& t : d["textures"].arr) {
        Texture tx;
        tx.map = t["map"].asString();
        tx.path = t["path"].asString();
        tx.colourSpace = t["colour_space"].asString();
        tx.format = t["format"].asString();
        std::string e;
        tx.loaded = loadImage(dir / tx.path, tx.img, e);
        if (!tx.loaded) a.loadErrors.push_back("texture '" + tx.path + "': " + e);
        a.textures.push_back(std::move(tx));
    }
    for (const JsonValue& l : d["lods"].arr) {
        Lod lod;
        lod.path = l["path"].asString();
        lod.hasError = l["error"].isNumber();
        lod.error = l["error"].asNumber();
        std::string e;
        lod.loaded = loadMesh(dir / lod.path, lod.mesh, e);
        if (!lod.loaded) a.loadErrors.push_back("mesh '" + lod.path + "': " + e);
        a.lods.push_back(std::move(lod));
    }
    return true;
}

// ---- helpers --------------------------------------------------------------------------------------------

double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double linearToSrgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

std::string fmt(double v, int prec = 3) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

bool isPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

std::array<double, 3> vtx(const Mesh& m, std::uint32_t i) { return {m.pos[i * 3u], m.pos[i * 3u + 1u], m.pos[i * 3u + 2u]}; }

double triArea3(const Mesh& m, size_t t) {
    const auto a = vtx(m, m.idx[t * 3u]), b = vtx(m, m.idx[t * 3u + 1u]), c = vtx(m, m.idx[t * 3u + 2u]);
    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    return 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
}

double triAreaUv(const Mesh& m, size_t t) {
    const std::uint32_t i0 = m.idx[t * 3u], i1 = m.idx[t * 3u + 1u], i2 = m.idx[t * 3u + 2u];
    const double ax = m.uv[i0 * 2u], ay = m.uv[i0 * 2u + 1u];
    const double bx = m.uv[i1 * 2u] - ax, by = m.uv[i1 * 2u + 1u] - ay;
    const double cx = m.uv[i2 * 2u] - ax, cy = m.uv[i2 * 2u + 1u] - ay;
    return 0.5 * std::fabs(bx * cy - by * cx);
}

std::string stemOf(const std::string& path) { return fs::path(path).stem().string(); }

// ---- gates ----------------------------------------------------------------------------------------------

void gateNaming(const Asset& a, Violations& v) {
    static const std::regex idRe(R"([a-z][a-z0-9_]*/[a-z][a-z0-9_]*/[a-z][a-z0-9_]*_[a-z0-9]+)");
    if (!std::regex_match(a.id, idRe)) {
        v.push_back("id '" + a.id + "' is not <class>/<biome|style>/<name>_<variant> (lowercase, [a-z0-9_])");
        return;
    }
    const std::string first = a.id.substr(0, a.id.find('/'));
    if (first != a.cls) {
        v.push_back("id class segment '" + first + "' does not match class '" + a.cls + "'");
    }
    const std::string base = a.id.substr(a.id.rfind('/') + 1u);
    static const std::set<std::string> maps = {"albedo", "normal", "orm", "height", "emissive"};
    for (const Texture& t : a.textures) {
        if (!maps.count(t.map)) {
            v.push_back("texture '" + t.path + "' map type '" + t.map + "' is not albedo|normal|orm|height|emissive");
        } else if (stemOf(t.path) != base + "_" + t.map) {
            v.push_back("texture file '" + t.path + "' should be named '" + base + "_" + t.map + ".<ext>'");
        }
    }
    for (size_t i = 0; i < a.lods.size(); ++i) {
        if (stemOf(a.lods[i].path) != base + "_lod" + std::to_string(i)) {
            v.push_back("LOD file '" + a.lods[i].path + "' should be named '" + base + "_lod" + std::to_string(i) + ".<ext>'");
        }
    }
}

void gateBudgetMesh(const Asset& a, Violations& v) {
    const ClassBudget* c = findClass(a.cls);
    if (c == nullptr) {
        v.push_back("unknown asset class '" + a.cls + "' (no §1.2 budget row)");
        return;
    }
    if (c->minLods == 0u) {
        return; // material / atlas / terrain layer: no mesh budget
    }
    if (a.lods.empty() || !a.lods[0].loaded) {
        v.push_back("LOD0 missing or unreadable");
        return;
    }
    const std::uint32_t budget = (a.dag && c->triDag != 0u) ? c->triDag : c->triT0;
    const size_t tris = a.lods[0].mesh.tris();
    if (tris > budget) {
        v.push_back("LOD0 has " + std::to_string(tris) + " triangles, over the '" + a.cls + "' budget of " +
                    std::to_string(budget) + (a.dag ? " (DAG source)" : " (T0)"));
    }
    const std::uint32_t need = a.impostor && c->minLods > 1u ? c->minLods - 1u : c->minLods;
    if (a.lods.size() < need) {
        v.push_back("LOD missing: " + std::to_string(a.lods.size()) + " LOD level(s), class '" + a.cls + "' needs " +
                    std::to_string(need) + (a.impostor ? " (with impostor)" : ""));
    }
}

void gateLodChain(const Asset& a, Violations& v) {
    for (size_t i = 0; i < a.lods.size(); ++i) {
        const Lod& l = a.lods[i];
        if (!l.hasError) {
            v.push_back("LOD" + std::to_string(i) + " has no recorded screen-space 'error'");
        }
        if (i == 0u) {
            if (l.hasError && l.error < 0.0) v.push_back("LOD0 error is negative");
            continue;
        }
        const Lod& p = a.lods[i - 1u];
        if (l.loaded && p.loaded && double(l.mesh.tris()) > 0.6 * double(p.mesh.tris())) {
            v.push_back("LOD" + std::to_string(i) + " has " + std::to_string(l.mesh.tris()) + " triangles, not reducing >= 40 % from LOD" +
                        std::to_string(i - 1u) + " (" + std::to_string(p.mesh.tris()) + ")");
        }
        if (l.hasError && p.hasError && !(l.error > p.error)) {
            v.push_back("LOD error not monotonic: LOD" + std::to_string(i) + " error " + fmt(l.error, 4) + " <= LOD" +
                        std::to_string(i - 1u) + " error " + fmt(p.error, 4));
        }
    }
}

void gateBudgetTexture(const Asset& a, Violations& v) {
    const ClassBudget* c = findClass(a.cls);
    const std::uint32_t maxSide = (c ? c->maxTex : 2048u) * (a.hero ? 2u : 1u);
    static const std::map<std::string, std::set<std::string>> formats = {
        {"albedo", {"BC7", "BC1"}}, {"normal", {"BC5"}}, {"orm", {"BC7", "BC1"}}, {"height", {"BC4"}}, {"emissive", {"BC7", "BC1", "BC6H"}}};
    for (const Texture& t : a.textures) {
        if (!t.loaded) continue;
        if (!isPow2(t.img.w) || !isPow2(t.img.h)) {
            v.push_back("texture '" + t.path + "' is " + std::to_string(t.img.w) + "x" + std::to_string(t.img.h) + ", not a power of two");
        }
        if (std::uint32_t(std::max(t.img.w, t.img.h)) > maxSide) {
            v.push_back("texture '" + t.path + "' is " + std::to_string(std::max(t.img.w, t.img.h)) + " px, over the '" + a.cls +
                        "' maximum of " + std::to_string(maxSide));
        }
        const auto it = formats.find(t.map);
        if (it != formats.end() && !it->second.count(t.format)) {
            v.push_back("texture '" + t.path + "' (" + t.map + ") format '" + t.format + "' not allowed by §1.3" +
                        (t.map == "normal" ? " (normal maps are BC5, never BC7/BC1)" : ""));
        }
        if (t.map == "normal" && t.colourSpace != "linear") {
            v.push_back("normal map '" + t.path + "' must be linear, not '" + t.colourSpace + "'");
        }
    }
}

void gateTexelDensity(const Asset& a, Violations& v) {
    const ClassBudget* c = findClass(a.cls);
    const double target = a.densityOverride > 0.0 ? a.densityOverride : (c ? c->density : 0.0);
    if (target <= 0.0 || a.tiling) {
        return; // atlas-shared / tiled classes: density comes from the tiling material
    }
    const Texture* t = a.tex("albedo");
    if (t == nullptr) {
        v.push_back("no albedo texture to measure texel density against");
        return;
    }
    if (a.lods.empty() || !a.lods[0].loaded) {
        v.push_back("LOD0 missing: cannot measure texel density");
        return;
    }
    const Mesh& m = a.lods[0].mesh;
    // UV islands: connected components over shared vertex indices (seams split vertices in cooked meshes).
    std::vector<std::uint32_t> parent(m.verts());
    std::iota(parent.begin(), parent.end(), 0u);
    std::function<std::uint32_t(std::uint32_t)> find = [&](std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (size_t tri = 0; tri < m.tris(); ++tri) {
        const std::uint32_t r0 = find(m.idx[tri * 3u]);
        parent[find(m.idx[tri * 3u + 1u])] = r0;
        parent[find(m.idx[tri * 3u + 2u])] = r0;
    }
    std::map<std::uint32_t, std::pair<double, double>> islands; // root -> (world area, uv area)
    double total = 0.0;
    for (size_t tri = 0; tri < m.tris(); ++tri) {
        auto& isl = islands[find(m.idx[tri * 3u])];
        const double a3 = triArea3(m, tri);
        isl.first += a3;
        isl.second += triAreaUv(m, tri);
        total += a3;
    }
    const double res = double(std::max(t->img.w, t->img.h));
    size_t idx = 0, outliers = 0;
    for (const auto& [root, areas] : islands) {
        (void)root;
        ++idx;
        if (areas.first < 0.01 * total || areas.first <= 0.0) continue; // slivers do not decide the gate
        const double density = res * std::sqrt(areas.second / areas.first);
        if (density < 0.5 * target || density > 1.5 * target) {
            ++outliers;
            v.push_back("UV island " + std::to_string(idx) + " (" + fmt(areas.first, 4) + " m2) has " + fmt(density, 1) +
                        " px/m, outside " + fmt(target, 0) + " px/m +-50 %");
        }
    }
    (void)outliers;
}

void gateNormalMap(const Asset& a, Violations& v) {
    for (const Texture& t : a.textures) {
        if (t.map != "normal" || !t.loaded) continue;
        const Image& im = t.img;
        size_t bad = 0, n = 0;
        double sumZ = 0.0;
        std::vector<double> p(size_t(im.w) * size_t(im.h)), qgl(p.size());
        for (int y = 0; y < im.h; ++y) {
            for (int x = 0; x < im.w; ++x) {
                const std::uint8_t* px = im.at(x, y);
                const double nx = px[0] / 127.5 - 1.0, ny = px[1] / 127.5 - 1.0, nz = px[2] / 127.5 - 1.0;
                const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (std::fabs(len - 1.0) > 0.05) ++bad;
                sumZ += nz;
                ++n;
                const double z = std::max(nz, 0.05);
                p[size_t(y) * size_t(im.w) + size_t(x)] = -nx / z;  // dh/dx
                qgl[size_t(y) * size_t(im.w) + size_t(x)] = ny / z; // dh/drow under +Y (OpenGL): +Y points to row 0
            }
        }
        if (n == 0u) continue;
        const double badFrac = double(bad) / double(n);
        if (badFrac > 0.01) {
            v.push_back("normal map '" + t.path + "': " + fmt(100.0 * badFrac, 1) + " % of texels are not unit length (+-0.05)");
        }
        const double meanZ = sumZ / double(n);
        if (meanZ <= 0.7) {
            v.push_back("normal map '" + t.path + "': mean Z " + fmt(meanZ) + " <= 0.7 (not a tangent-space normal map?)");
        }
        // Convention: an integrable height field has d(dh/dx)/drow == d(dh/drow)/dx. With the green channel
        // read as +Y the residual vanishes; a DirectX (-Y) map fits the flipped reading instead.
        double rGl = 0.0, rDx = 0.0;
        for (int y = 0; y + 1 < im.h; ++y) {
            for (int x = 0; x + 1 < im.w; ++x) {
                const size_t i = size_t(y) * size_t(im.w) + size_t(x);
                const double pRow = p[i + size_t(im.w)] - p[i];
                const double qCol = qgl[i + 1u] - qgl[i];
                rGl += std::fabs(pRow - qCol);
                rDx += std::fabs(pRow + qCol);
            }
        }
        const double pixels = double(std::max(1, (im.w - 1) * (im.h - 1)));
        if ((rGl + rDx) / pixels > 0.004 && rGl > 1.5 * rDx) {
            v.push_back("normal map '" + t.path + "': green channel fits the DirectX (-Y) convention (integrability residual +Y " +
                        fmt(rGl / pixels, 4) + " vs -Y " + fmt(rDx / pixels, 4) + "); FUSE uses OpenGL (+Y)");
        }
    }
}

/// Mean over a grid x grid block partition of channel `ch` (as linear luminance when ch < 0).
std::vector<double> blockMeans(const Image& im, int ch, int grid) {
    std::vector<double> sum(size_t(grid) * size_t(grid), 0.0), cnt(sum.size(), 0.0);
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            const std::uint8_t* px = im.at(x, y);
            double val;
            if (ch < 0) {
                val = 0.2126 * srgbToLinear(px[0] / 255.0) + 0.7152 * srgbToLinear(px[1] / 255.0) + 0.0722 * srgbToLinear(px[2] / 255.0);
            } else {
                val = px[ch] / 255.0;
            }
            const size_t b = size_t(y * grid / im.h) * size_t(grid) + size_t(x * grid / im.w);
            sum[b] += val;
            cnt[b] += 1.0;
        }
    }
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = cnt[i] > 0.0 ? sum[i] / cnt[i] : 0.0;
    return sum;
}

double stddev(const std::vector<double>& x) {
    if (x.empty()) return 0.0;
    const double m = std::accumulate(x.begin(), x.end(), 0.0) / double(x.size());
    double s = 0.0;
    for (double e : x) s += (e - m) * (e - m);
    return std::sqrt(s / double(x.size()));
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / double(a.size());
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / double(b.size());
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : 0.0;
}

void gateColorSpace(const Asset& a, Violations& v) {
    for (const Texture& t : a.textures) {
        const bool wantSrgb = t.map == "albedo" || t.map == "emissive";
        const std::string want = wantSrgb ? "srgb" : "linear";
        if (t.colourSpace != want) {
            v.push_back("texture '" + t.path + "' (" + t.map + ") declares colour space '" + t.colourSpace + "', must be '" + want + "'");
        }
    }
    const Texture* alb = a.tex("albedo");
    if (alb == nullptr) return;
    const Texture* orm = a.tex("orm");
    const Image& im = alb->img;
    size_t n = 0, lowD = 0, highD = 0, nMetal = 0, lowM = 0;
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            const std::uint8_t* px = im.at(x, y);
            if (px[3] < 128u) continue; // cutout
            const double lum = 0.2126 * srgbToLinear(px[0] / 255.0) + 0.7152 * srgbToLinear(px[1] / 255.0) + 0.0722 * srgbToLinear(px[2] / 255.0);
            const double s = 255.0 * linearToSrgb(lum);
            bool metal = a.metal == "full";
            if (orm != nullptr && a.metal == "mixed") {
                metal = orm->img.at(x * orm->img.w / im.w, y * orm->img.h / im.h)[2] >= 128u;
            }
            if (metal) {
                ++nMetal;
                if (s < 180.0) ++lowM;
            } else {
                ++n;
                if (s < 30.0) ++lowD;
                if (s > 240.0) ++highD;
            }
        }
    }
    if (n > 0u && double(lowD + highD) / double(n) > 0.02) {
        v.push_back("albedo '" + alb->path + "': " + fmt(100.0 * double(lowD) / double(n), 1) + " % of dielectric texels below 30 sRGB and " +
                    fmt(100.0 * double(highD) / double(n), 1) + " % above 240 (§1.6 allows 30-240)");
    }
    if (nMetal > 0u && double(lowM) / double(nMetal) > 0.02) {
        v.push_back("albedo '" + alb->path + "': " + fmt(100.0 * double(lowM) / double(nMetal), 1) +
                    " % of metal texels below 180 sRGB (§1.6 metal albedo 180-255)");
    }
    // Baked lighting: low-frequency albedo luminance must not follow the AO channel.
    if (orm != nullptr) {
        const std::vector<double> ao = blockMeans(orm->img, 0, 16);
        const std::vector<double> lum = blockMeans(im, -1, 16);
        if (stddev(ao) > 0.02) {
            const double r = correlation(lum, ao);
            if (r >= 0.5) {
                v.push_back("albedo '" + alb->path + "' correlates with the AO channel (r = " + fmt(r) + " >= 0.5): baked AO / lighting in albedo");
            }
        }
    }
}

void gatePbrSanity(const Asset& a, Violations& v) {
    if (const Texture* orm = a.tex("orm")) {
        std::vector<double> rough;
        size_t grey = 0, on = 0, n = 0;
        for (int y = 0; y < orm->img.h; ++y) {
            for (int x = 0; x < orm->img.w; ++x) {
                const std::uint8_t* px = orm->img.at(x, y);
                rough.push_back(px[1] / 255.0);
                const double m = px[2] / 255.0;
                if (m > 0.1 && m < 0.9) ++grey;
                if (m >= 0.5) ++on;
                ++n;
            }
        }
        const double sd = stddev(rough);
        if (sd < 0.03) {
            const double mean = std::accumulate(rough.begin(), rough.end(), 0.0) / double(rough.size());
            v.push_back("ORM '" + orm->path + "': roughness is constant (mean " + fmt(mean) + ", std " + fmt(sd, 4) + " < 0.03)");
        }
        const double greyFrac = double(grey) / double(n), onFrac = double(on) / double(n);
        if (greyFrac > 0.05) {
            v.push_back("ORM '" + orm->path + "': metal mask not binary (" + fmt(100.0 * greyFrac, 1) + " % of texels in 0.1-0.9)");
        }
        if (a.metal == "none" && onFrac > 0.01) {
            v.push_back("ORM '" + orm->path + "': material declares metal 'none' but " + fmt(100.0 * onFrac, 1) + " % of the metal mask is set");
        }
        if (a.metal == "full" && onFrac < 0.99) {
            v.push_back("ORM '" + orm->path + "': material declares metal 'full' but the metal mask covers only " + fmt(100.0 * onFrac, 1) + " %");
        }
    }
    if (const Texture* em = a.tex("emissive")) {
        size_t lit = 0;
        for (size_t i = 0; i < em->img.rgba.size(); i += 4u) {
            if (std::max({em->img.rgba[i], em->img.rgba[i + 1], em->img.rgba[i + 2]}) > 8u) ++lit;
        }
        if (!a.emissive && lit > 0u) {
            v.push_back("emissive map '" + em->path + "' on a material not declared emissive");
        }
    }
}

void gateUvBounds(const Asset& a, Violations& v) {
    for (size_t li = 0; li < a.lods.size(); ++li) {
        const Lod& l = a.lods[li];
        if (!l.loaded) continue;
        size_t nonFinite = 0, outside = 0, collapsed = 0;
        for (size_t i = 0; i < l.mesh.uv.size(); ++i) {
            const double u = l.mesh.uv[i];
            if (!std::isfinite(u)) ++nonFinite;
            else if (!a.tiling && (u < -1e-4 || u > 1.0 + 1e-4)) ++outside;
        }
        for (size_t t = 0; t < l.mesh.tris(); ++t) {
            if (triAreaUv(l.mesh, t) < 1e-12) ++collapsed;
        }
        const std::string where = "LOD" + std::to_string(li) + " '" + l.path + "'";
        if (nonFinite > 0u) v.push_back(where + ": " + std::to_string(nonFinite) + " non-finite UV components");
        if (outside > 0u) v.push_back(where + ": " + std::to_string(outside) + " UV components outside [0,1] on a non-tiling asset");
        if (l.mesh.tris() > 0u && double(collapsed) / double(l.mesh.tris()) > 0.01) {
            v.push_back(where + ": " + std::to_string(collapsed) + " of " + std::to_string(l.mesh.tris()) +
                        " triangles have zero UV area (breaks tangent frames and texel density)");
        }
    }
}

void gateGeometry(const Asset& a, Violations& v) {
    if (a.units != "m") {
        v.push_back("units '" + a.units + "': FUSE content is authored with 1 unit = 1 m");
    }
    if (a.up != "z" && a.up != "y") {
        v.push_back("up axis '" + a.up + "' not declared as 'z' or 'y'");
        return;
    }
    const int up = a.up == "z" ? 2 : 1;
    for (size_t li = 0; li < a.lods.size(); ++li) {
        const Lod& l = a.lods[li];
        if (!l.loaded || l.mesh.verts() == 0u) continue;
        const std::string where = "LOD" + std::to_string(li) + " '" + l.path + "'";
        size_t nonFinite = 0, degenerate = 0;
        double lo = 1e300, hi = -1e300;
        for (size_t i = 0; i < l.mesh.verts(); ++i) {
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(l.mesh.pos[i * 3u + size_t(k)])) ++nonFinite;
            }
            lo = std::min(lo, l.mesh.pos[i * 3u + size_t(up)]);
            hi = std::max(hi, l.mesh.pos[i * 3u + size_t(up)]);
        }
        for (size_t t = 0; t < l.mesh.tris(); ++t) {
            const std::uint32_t* tri = &l.mesh.idx[t * 3u];
            if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2] || !(triArea3(l.mesh, t) > 1e-10)) ++degenerate;
        }
        if (nonFinite > 0u) v.push_back(where + ": " + std::to_string(nonFinite) + " non-finite position components");
        if (degenerate > 0u) v.push_back(where + ": " + std::to_string(degenerate) + " degenerate / zero-area triangles");
        if (li != 0u) continue;
        const double height = hi - lo;
        const CategoryRange* cat = findCategory(a.category);
        if (cat == nullptr) {
            v.push_back("unknown category '" + a.category + "' (no real-world scale range)");
        } else if (height < cat->minH || height > cat->maxH) {
            v.push_back(where + ": height " + fmt(height) + " m outside the '" + a.category + "' range " + fmt(cat->minH, 2) + "-" +
                        fmt(cat->maxH, 2) + " m (wrong units or scale?)");
        }
        if (std::fabs(lo) > std::max(0.01, 0.02 * height)) {
            v.push_back(where + ": pivot not at base (lowest point at " + fmt(lo) + " m along +" + a.up + ")");
        }
    }
}

using GateFn = void (*)(const Asset&, Violations&);

GateFn gateFn(const std::string& g) {
    static const std::map<std::string, GateFn> fns = {
        {"naming", gateNaming},         {"budget_mesh", gateBudgetMesh}, {"lod_chain", gateLodChain},
        {"budget_texture", gateBudgetTexture}, {"texel_density", gateTexelDensity}, {"normal_map", gateNormalMap},
        {"color_space", gateColorSpace}, {"pbr_sanity", gatePbrSanity}, {"uv_bounds", gateUvBounds},
        {"geometry", gateGeometry}};
    const auto it = fns.find(g);
    return it == fns.end() ? nullptr : it->second;
}

/// Runs `gates` on the asset; returns violations per gate (load errors are reported under "load").
std::map<std::string, Violations> runGates(const Asset& a, const std::vector<std::string>& gates) {
    std::map<std::string, Violations> out;
    if (!a.loadErrors.empty()) out["load"] = a.loadErrors;
    for (const std::string& g : gates) {
        Violations v;
        gateFn(g)(a, v);
        out[g] = v;
    }
    return out;
}

// ---- fixtures -------------------------------------------------------------------------------------------

struct Rng {
    std::uint32_t s;
    double next() {
        s = s * 1664525u + 1013904223u;
        return double(s >> 8) / double(1u << 24);
    }
};

std::uint8_t q8(double x) { return std::uint8_t(std::clamp(std::lround(x * 255.0), 0L, 255L)); }

Image makeImage(int w, int h, const std::function<std::array<double, 4>(int, int)>& f) {
    Image im;
    im.w = w;
    im.h = h;
    im.rgba.resize(size_t(w) * size_t(h) * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto c = f(x, y);
            std::uint8_t* o = &im.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4u];
            for (int k = 0; k < 4; ++k) o[k] = q8(c[size_t(k)]);
        }
    }
    return im;
}

/// Albedo: block noise (8 px blocks) in sRGB 0.38-0.66, alpha 1.
Image fixtureAlbedo(int size, double lo = 0.38, double hi = 0.66) {
    Rng r{42u};
    std::vector<double> blocks(size_t(size / 8) * size_t(size / 8));
    for (double& b : blocks) b = lo + (hi - lo) * r.next();
    return makeImage(size, size, [&](int x, int y) {
        const double g = blocks[size_t(y / 8) * size_t(size / 8) + size_t(x / 8)];
        return std::array<double, 4>{g * 1.02, g, g * 0.97, 1.0};
    });
}

/// ORM: AO 0.7-1, roughness 0.5-0.9 (independent noise), metal = `metal`.
Image fixtureOrm(int size, double metal) {
    Rng r{7u};
    return makeImage(size, size, [&](int, int) { return std::array<double, 4>{0.7 + 0.3 * r.next(), 0.5 + 0.4 * r.next(), metal, 1.0}; });
}

/// Normal map from h = A sin(kx) sin(ky) + B cos(k2 x): OpenGL convention (+Y toward row 0), optionally
/// with the green channel flipped (DirectX) and / or vectors scaled (not unit length).
Image fixtureNormal(int size, bool flipGreen, double scale) {
    const double k = 2.0 * 3.14159265358979 / 64.0, amp = 6.0;
    auto h = [&](double x, double y) { return amp * std::sin(k * x) * std::sin(k * y) + 2.0 * std::cos(0.5 * k * x); };
    return makeImage(size, size, [&](int x, int y) {
        const double hx = (h(x + 1, y) - h(x - 1, y)) * 0.5;
        const double hr = (h(x, y + 1) - h(x, y - 1)) * 0.5;
        double nx = -hx, ny = hr, nz = 1.0;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx /= len;
        ny /= len;
        nz /= len;
        if (flipGreen) ny = -ny;
        return std::array<double, 4>{0.5 + 0.5 * nx * scale, 0.5 + 0.5 * ny * scale, 0.5 + 0.5 * nz * scale, 1.0};
    });
}

/// Box of side `side` metres, base on z = 0, each face an n x n grid; six UV islands of `island` UV
/// units laid out on a 3 x 2 grid of cells (0.3 apart).
Mesh fixtureBox(double side, int n, double island) {
    Mesh m;
    // face: origin, u axis, v axis (outward winding irrelevant for the gates)
    const double s = side;
    const std::array<std::array<double, 9>, 6> faces = {{{0, 0, 0, s, 0, 0, 0, s, 0},
                                                          {0, 0, s, s, 0, 0, 0, s, 0},
                                                          {0, 0, 0, s, 0, 0, 0, 0, s},
                                                          {0, s, 0, s, 0, 0, 0, 0, s},
                                                          {0, 0, 0, 0, s, 0, 0, 0, s},
                                                          {s, 0, 0, 0, s, 0, 0, 0, s}}};
    for (int f = 0; f < 6; ++f) {
        const auto& F = faces[size_t(f)];
        const double u0 = 0.02 + 0.33 * (f % 3), v0 = 0.02 + 0.5 * (f / 3);
        const std::uint32_t base = std::uint32_t(m.verts());
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                const double a = double(i) / n, b = double(j) / n;
                m.pos.insert(m.pos.end(), {F[0] + a * F[3] + b * F[6], F[1] + a * F[4] + b * F[7], F[2] + a * F[5] + b * F[8]});
                m.uv.insert(m.uv.end(), {u0 + a * island, v0 + b * island});
            }
        }
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const std::uint32_t q = base + std::uint32_t(j * (n + 1) + i);
                m.idx.insert(m.idx.end(), {q, q + 1u, q + std::uint32_t(n) + 2u, q, q + std::uint32_t(n) + 2u, q + std::uint32_t(n) + 1u});
            }
        }
    }
    return m;
}

std::string encodeFusemesh(const Mesh& m) {
    std::string out;
    auto u32 = [&](std::uint32_t x) {
        for (int i = 0; i < 4; ++i) out.push_back(char((x >> (8 * i)) & 0xffu));
    };
    auto f32 = [&](double d) {
        const float f = float(d);
        std::uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        u32(b);
    };
    out += "FMSH";
    u32(1u);
    u32(0u);
    u32(std::uint32_t(m.verts()));
    u32(std::uint32_t(m.idx.size()));
    u32(1u);
    std::array<double, 3> lo = {1e30, 1e30, 1e30}, hi = {-1e30, -1e30, -1e30};
    for (size_t i = 0; i < m.verts(); ++i) {
        for (size_t k = 0; k < 3u; ++k) {
            lo[k] = std::min(lo[k], m.pos[i * 3u + k]);
            hi[k] = std::max(hi[k], m.pos[i * 3u + k]);
        }
    }
    for (double x : lo) f32(x);
    for (double x : hi) f32(x);
    u32(0u);
    u32(std::uint32_t(m.idx.size()));
    u32(0u);
    u32(0u);
    for (double x : m.pos) f32(x);
    for (size_t i = 0; i < m.verts() * 3u; ++i) f32(0.0);
    for (double x : m.uv) f32(x);
    for (std::uint32_t i : m.idx) u32(i);
    const std::uint64_t h = fnv1a64(reinterpret_cast<const std::uint8_t*>(out.data()), out.size());
    for (int i = 0; i < 8; ++i) out.push_back(char((h >> (8 * i)) & 0xffu));
    return out;
}

std::string encodeObj(const Mesh& m) {
    std::ostringstream o;
    o.precision(9);
    for (size_t i = 0; i < m.verts(); ++i) o << "v " << m.pos[i * 3u] << ' ' << m.pos[i * 3u + 1u] << ' ' << m.pos[i * 3u + 2u] << '\n';
    for (size_t i = 0; i < m.verts(); ++i) o << "vt " << m.uv[i * 2u] << ' ' << m.uv[i * 2u + 1u] << '\n';
    for (size_t t = 0; t < m.tris(); ++t) {
        o << 'f';
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t i = m.idx[t * 3u + size_t(k)] + 1u;
            o << ' ' << i << '/' << i;
        }
        o << '\n';
    }
    return o.str();
}

/// A fixture = descriptor fields + in-memory textures + LOD meshes; written to disk, then checked from disk.
struct Fixture {
    std::string id = "rock/alps/boulder_a", cls = "rock", category = "boulder", units = "m", up = "z", metal = "none";
    bool emissive = false;
    struct Tex {
        std::string map, file, cs, format;
        Image img;
    };
    struct L {
        std::string file;
        Mesh mesh;
        double error;
        bool hasError = true;
    };
    std::vector<Tex> textures;
    std::vector<L> lods;

    Tex& tex(const std::string& map) {
        for (Tex& t : textures) {
            if (t.map == map) return t;
        }
        return textures.front();
    }
};

/// The passing asset: 0.5 m boulder box, 3 LODs (768 / 432 / 192 triangles, LOD2 as OBJ), 1024^2 albedo
/// (0.25 UV islands -> 512 px/m), 256^2 OpenGL normal map, 256^2 ORM (dielectric, varied roughness).
Fixture goodFixture() {
    Fixture f;
    f.textures.push_back({"albedo", "boulder_a_albedo.tga", "srgb", "BC7", fixtureAlbedo(1024)});
    f.textures.push_back({"normal", "boulder_a_normal.tga", "linear", "BC5", fixtureNormal(256, false, 1.0)});
    f.textures.push_back({"orm", "boulder_a_orm.tga", "linear", "BC7", fixtureOrm(256, 0.0)});
    f.lods.push_back({"boulder_a_lod0.fusemesh", fixtureBox(0.5, 8, 0.25), 0.0});
    f.lods.push_back({"boulder_a_lod1.fusemesh", fixtureBox(0.5, 6, 0.25), 0.002});
    f.lods.push_back({"boulder_a_lod2.obj", fixtureBox(0.5, 4, 0.25), 0.01});
    return f;
}

fs::path writeFixture(const Fixture& f, const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ostringstream d;
    d << "{\n  \"schema\": 1,\n  \"id\": \"" << f.id << "\",\n  \"class\": \"" << f.cls << "\",\n  \"category\": \"" << f.category
      << "\",\n  \"units\": \"" << f.units << "\",\n  \"up\": \"" << f.up << "\",\n  \"tiling\": false,\n"
      << "  \"material\": {\"metal\": \"" << f.metal << "\", \"emissive\": " << (f.emissive ? "true" : "false") << "},\n  \"textures\": [";
    for (size_t i = 0; i < f.textures.size(); ++i) {
        const auto& t = f.textures[i];
        writeAll(dir / t.file, encodeTga(t.img));
        d << (i ? ",\n" : "\n") << "    {\"map\": \"" << t.map << "\", \"path\": \"" << t.file << "\", \"colour_space\": \"" << t.cs
          << "\", \"format\": \"" << t.format << "\"}";
    }
    d << "\n  ],\n  \"lods\": [";
    for (size_t i = 0; i < f.lods.size(); ++i) {
        const auto& l = f.lods[i];
        writeAll(dir / l.file, fs::path(l.file).extension() == ".obj" ? encodeObj(l.mesh) : encodeFusemesh(l.mesh));
        d << (i ? ",\n" : "\n") << "    {\"path\": \"" << l.file << "\"";
        if (l.hasError) d << ", \"error\": " << l.error;
        d << "}";
    }
    d << "\n  ]\n}\n";
    writeAll(dir / "asset.json", d.str());
    return dir / "asset.json";
}

struct FailCase {
    std::string name;
    std::string expect; ///< substring the gate must report
    std::function<void(Fixture&)> mutate;
};

std::vector<FailCase> failCases(const std::string& gate) {
    std::vector<FailCase> c;
    if (gate == "naming") {
        c.push_back({"bad_id", "is not <class>", [](Fixture& f) { f.id = "Rock/Alps/Boulder"; }});
        c.push_back({"class_mismatch", "does not match class", [](Fixture& f) { f.id = "tree/alps/boulder_a"; }});
        c.push_back({"texture_suffix", "should be named 'boulder_a_albedo", [](Fixture& f) { f.tex("albedo").file = "boulder_a_diffuse.tga"; }});
        c.push_back({"lod_suffix", "should be named 'boulder_a_lod1", [](Fixture& f) { f.lods[1].file = "boulder_a_low.fusemesh"; }});
    } else if (gate == "budget_mesh") {
        c.push_back({"over_budget", "over the 'rock' budget", [](Fixture& f) {
                         f.lods[0].mesh = fixtureBox(0.5, 42, 0.25); // 10584 triangles > 10k
                         f.lods[1].mesh = fixtureBox(0.5, 30, 0.25);
                         f.lods[2].mesh = fixtureBox(0.5, 20, 0.25);
                     }});
        c.push_back({"lod_missing", "LOD missing", [](Fixture& f) { f.lods.pop_back(); }});
    } else if (gate == "lod_chain") {
        c.push_back({"weak_reduction", "not reducing >= 40 %", [](Fixture& f) { f.lods[1].mesh = fixtureBox(0.5, 7, 0.25); }});
        c.push_back({"error_not_monotonic", "not monotonic", [](Fixture& f) { f.lods[2].error = 0.001; }});
        c.push_back({"error_missing", "no recorded screen-space", [](Fixture& f) { f.lods[1].hasError = false; }});
    } else if (gate == "budget_texture") {
        c.push_back({"over_resolution", "over the 'rock' maximum", [](Fixture& f) {
                         f.tex("orm").img = makeImage(2048, 8, [](int, int) { return std::array<double, 4>{1, 0.6, 0, 1}; });
                     }});
        c.push_back({"not_pow2", "not a power of two", [](Fixture& f) { f.tex("orm").img = fixtureOrm(200, 0.0); }});
        c.push_back({"bc7_normal", "never BC7", [](Fixture& f) { f.tex("normal").format = "BC7"; }});
        c.push_back({"srgb_normal", "must be linear", [](Fixture& f) { f.tex("normal").cs = "srgb"; }});
    } else if (gate == "texel_density") {
        c.push_back({"island_outlier", "outside 512 px/m", [](Fixture& f) {
                         Mesh& m = f.lods[0].mesh; // shrink the first face's island to 0.05 UV (~102 px/m)
                         const size_t perFace = m.verts() / 6u;
                         for (size_t i = 0; i < perFace; ++i) {
                             m.uv[i * 2u] = 0.02 + (m.uv[i * 2u] - 0.02) * 0.2;
                             m.uv[i * 2u + 1u] = 0.02 + (m.uv[i * 2u + 1u] - 0.02) * 0.2;
                         }
                     }});
        c.push_back({"low_res_texture", "outside 512 px/m", [](Fixture& f) { f.tex("albedo").img = fixtureAlbedo(256); }});
    } else if (gate == "normal_map") {
        c.push_back({"directx_green", "DirectX (-Y)", [](Fixture& f) { f.tex("normal").img = fixtureNormal(256, true, 1.0); }});
        c.push_back({"not_unit", "not unit length", [](Fixture& f) { f.tex("normal").img = fixtureNormal(256, false, 0.8); }});
        c.push_back({"low_z", "mean Z", [](Fixture& f) {
                         f.tex("normal").img = makeImage(64, 64, [](int, int) { return std::array<double, 4>{0.5 + 0.5 * 0.8, 0.5, 0.5 + 0.5 * 0.6, 1}; });
                     }});
    } else if (gate == "color_space") {
        c.push_back({"too_dark", "below 30 sRGB", [](Fixture& f) { f.tex("albedo").img = fixtureAlbedo(1024, 0.02, 0.08); }});
        c.push_back({"too_bright", "above 240", [](Fixture& f) { f.tex("albedo").img = fixtureAlbedo(1024, 0.97, 1.0); }});
        c.push_back({"linear_albedo", "must be 'srgb'", [](Fixture& f) { f.tex("albedo").cs = "linear"; }});
        c.push_back({"srgb_orm", "must be 'linear'", [](Fixture& f) { f.tex("orm").cs = "srgb"; }});
        c.push_back({"dark_metal", "metal texels below 180", [](Fixture& f) {
                         f.metal = "full";
                         f.tex("orm").img = fixtureOrm(256, 1.0);
                     }});
        c.push_back({"baked_ao", "baked AO", [](Fixture& f) {
                         // AO varies in large blobs and the albedo is darkened by it.
                         auto ao = [](int x, int y, int size) {
                             return 0.55 + 0.45 * (0.5 + 0.5 * std::sin(6.2831853 * x / size * 2.0) * std::cos(6.2831853 * y / size * 1.5));
                         };
                         f.tex("orm").img = makeImage(256, 256, [&](int x, int y) {
                             Rng r{std::uint32_t(x * 131 + y)};
                             return std::array<double, 4>{ao(x, y, 256), 0.5 + 0.4 * r.next(), 0, 1};
                         });
                         f.tex("albedo").img = makeImage(1024, 1024, [&](int x, int y) {
                             const double g = 0.3 + 0.4 * ao(x, y, 1024);
                             return std::array<double, 4>{g, g, g, 1};
                         });
                     }});
    } else if (gate == "pbr_sanity") {
        c.push_back({"constant_roughness", "roughness is constant", [](Fixture& f) {
                         f.tex("orm").img = makeImage(256, 256, [](int, int) { return std::array<double, 4>{1, 0.5, 0, 1}; });
                     }});
        c.push_back({"grey_metal", "not binary", [](Fixture& f) {
                         f.metal = "mixed";
                         f.tex("orm").img = fixtureOrm(256, 0.5);
                     }});
        c.push_back({"metal_on_dielectric", "declares metal 'none'", [](Fixture& f) { f.tex("orm").img = fixtureOrm(256, 1.0); }});
        c.push_back({"stray_emissive", "not declared emissive", [](Fixture& f) {
                         f.textures.push_back({"emissive", "boulder_a_emissive.tga", "srgb", "BC7",
                                               makeImage(64, 64, [](int x, int) { return std::array<double, 4>{x < 8 ? 1.0 : 0.0, 0.4, 0, 1}; })});
                     }});
    } else if (gate == "uv_bounds") {
        c.push_back({"outside_unit_square", "outside [0,1]", [](Fixture& f) {
                         for (double& u : f.lods[1].mesh.uv) u += 1.2;
                     }});
        c.push_back({"collapsed_uvs", "zero UV area", [](Fixture& f) {
                         Mesh& m = f.lods[0].mesh;
                         for (size_t i = 0; i < m.verts() / 6u; ++i) {
                             m.uv[i * 2u] = 0.1;
                             m.uv[i * 2u + 1u] = 0.1;
                         }
                     }});
        c.push_back({"nan_uv", "non-finite UV", [](Fixture& f) { f.lods[0].mesh.uv[3] = std::nan(""); }});
    } else if (gate == "geometry") {
        c.push_back({"centimetres", "outside the 'boulder' range", [](Fixture& f) {
                         for (auto& l : f.lods)
                             for (double& x : l.mesh.pos) x *= 100.0; // authored in cm: 50 m boulder
                     }});
        c.push_back({"units_declared_cm", "1 unit = 1 m", [](Fixture& f) { f.units = "cm"; }});
        c.push_back({"pivot_centre", "pivot not at base", [](Fixture& f) {
                         for (auto& l : f.lods)
                             for (size_t i = 2; i < l.mesh.pos.size(); i += 3u) l.mesh.pos[i] -= 0.25;
                     }});
        c.push_back({"degenerate", "degenerate", [](Fixture& f) {
                         Mesh& m = f.lods[0].mesh;
                         m.idx.insert(m.idx.end(), {0u, 1u, 1u, 0u, 1u, 2u});
                     }});
        c.push_back({"door_too_small", "outside the 'door' range", [](Fixture& f) { f.category = "door"; }});
    }
    return c;
}

int fixtureTest(const std::string& gate, const fs::path& scratch) {
    bool ok = true;
    const fs::path root = scratch / gate;
    // Passing fixture: every gate passes (not only this one), read back from disk.
    const fs::path goodFile = writeFixture(goodFixture(), root / "pass");
    Asset good;
    std::string err;
    if (!loadAsset(goodFile, good, err)) {
        std::fprintf(stderr, "fixture-test %s: %s\n", gate.c_str(), err.c_str());
        return 2;
    }
    const auto gr = runGates(good, gateNames());
    for (const auto& [g, vs] : gr) {
        for (const std::string& s : vs) {
            std::fprintf(stderr, "  pass fixture: unexpected [%s] %s\n", g.c_str(), s.c_str());
            ok = false;
        }
    }
    std::printf("fuse_assetcheck %s: passing fixture %s\n", gate.c_str(), gr.at(gate).empty() && ok ? "passes" : "FAILED");
    const auto cases = failCases(gate);
    if (cases.empty()) {
        std::fprintf(stderr, "fixture-test %s: no failing fixtures defined\n", gate.c_str());
        return 2;
    }
    for (const FailCase& fc : cases) {
        Fixture f = goodFixture();
        fc.mutate(f);
        const fs::path file = writeFixture(f, root / ("fail_" + fc.name));
        Asset a;
        if (!loadAsset(file, a, err)) {
            std::fprintf(stderr, "fixture-test %s/%s: %s\n", gate.c_str(), fc.name.c_str(), err.c_str());
            ok = false;
            continue;
        }
        const auto r = runGates(a, {gate});
        const Violations& vs = r.at(gate);
        const bool hit = std::any_of(vs.begin(), vs.end(), [&](const std::string& s) { return s.find(fc.expect) != std::string::npos; });
        for (const std::string& s : vs) std::printf("  fail_%s: [%s] %s\n", fc.name.c_str(), gate.c_str(), s.c_str());
        if (r.count("load")) {
            for (const std::string& s : r.at("load")) std::fprintf(stderr, "  fail_%s: load error %s\n", fc.name.c_str(), s.c_str());
            ok = false;
        }
        if (!hit) {
            std::fprintf(stderr, "  failing fixture '%s' was not rejected with '%s'\n", fc.name.c_str(), fc.expect.c_str());
            ok = false;
        }
        std::printf("fuse_assetcheck %s: failing fixture %s %s\n", gate.c_str(), fc.name.c_str(), hit ? "rejected" : "NOT rejected");
        std::error_code ec;
        fs::remove_all(root / ("fail_" + fc.name), ec);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("fuse_assetcheck fixture-test %s: %s\n", gate.c_str(), ok ? "ok" : "FAILED");
    return ok ? 0 : 2;
}

int usage() {
    std::fprintf(stderr,
                 "usage: fuse_assetcheck check <asset.json> [--gates g1,g2]\n"
                 "       fuse_assetcheck fixture-test --gate <gate> --scratch DIR\n"
                 "       fuse_assetcheck list\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "list") {
        for (const std::string& g : gateNames()) std::printf("%s\n", g.c_str());
        return 0;
    }
    std::vector<std::string> gates = gateNames();
    std::string gate, asset;
    fs::path scratch;
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--gate" && i + 1 < argc) gate = argv[++i];
        else if (k == "--scratch" && i + 1 < argc) scratch = argv[++i];
        else if (k == "--gates" && i + 1 < argc) {
            gates.clear();
            std::stringstream ss(argv[++i]);
            std::string g;
            while (std::getline(ss, g, ',')) gates.push_back(g);
        } else if (asset.empty() && k.rfind("--", 0) != 0) asset = k;
        else return usage();
    }
    for (const std::string& g : gates) {
        if (gateFn(g) == nullptr) {
            std::fprintf(stderr, "unknown gate '%s'\n", g.c_str());
            return 2;
        }
    }
    if (cmd == "fixture-test") {
        if (gate.empty() || gateFn(gate) == nullptr || scratch.empty()) return usage();
        return fixtureTest(gate, scratch);
    }
    if (cmd != "check" || asset.empty()) return usage();
    Asset a;
    std::string err;
    if (!loadAsset(asset, a, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    size_t total = 0;
    for (const auto& [g, vs] : runGates(a, gates)) {
        std::printf("%-15s %s\n", g.c_str(), vs.empty() ? "PASS" : "FAIL");
        for (const std::string& s : vs) std::printf("  %s\n", s.c_str());
        total += vs.size();
    }
    std::printf("fuse_assetcheck %s: %zu violation(s)\n", a.id.c_str(), total);
    return total == 0u ? 0 : 1;
}
