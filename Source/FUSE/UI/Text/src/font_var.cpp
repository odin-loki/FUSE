// OpenType font variations: fvar axes and named instances, avar, gvar outline deltas over our own glyf decoder (the
// MINIMAL rasteriser has no variation support), HVAR advances, MVAR metrics and the item variation store (also used
// by the COLRv1 renderer for Var* paints). Every read is bounds-checked against the font file; a malformed table
// leaves the default outlines and metrics.
#include "font_var.hpp"

#include "text_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace fuse::text::detail {
namespace {

constexpr std::size_t kMaxInstances = 256; // per font file; past it the nearest instance is returned
constexpr int kMaxComponentDepth = 8;
constexpr std::size_t kMaxPoints = 1 << 16;

struct Rd {
    std::span<const std::uint8_t> d;
    bool ok(std::uint64_t off, std::uint64_t len) const { return off <= d.size() && len <= d.size() - off; }
    std::uint8_t u8(std::uint64_t p) const { return d[p]; }
    std::uint16_t u16(std::uint64_t p) const { return std::uint16_t((d[p] << 8) | d[p + 1]); }
    std::int16_t i16(std::uint64_t p) const { return std::int16_t(u16(p)); }
    std::uint32_t u32(std::uint64_t p) const {
        return (std::uint32_t(d[p]) << 24) | (std::uint32_t(d[p + 1]) << 16) | (std::uint32_t(d[p + 2]) << 8) | d[p + 3];
    }
    float fixed(std::uint64_t p) const { return float(std::int32_t(u32(p))) / 65536.f; }
};

// Offset (and length) of table `tag` of the face at `start`; 0 when absent or out of range.
std::uint32_t findTable(const Rd& r, std::uint32_t start, const char* tag, std::uint32_t* length = nullptr) {
    if (!r.ok(start, 12)) return 0;
    const std::uint16_t n = r.u16(start + 4);
    if (!r.ok(std::uint64_t(start) + 12, std::uint64_t(n) * 16)) return 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t e = start + 12 + i * 16;
        if (std::memcmp(r.d.data() + e, tag, 4) != 0) continue;
        const std::uint32_t off = r.u32(e + 8), len = r.u32(e + 12);
        if (!r.ok(off, len)) return 0;
        if (length) *length = len;
        return off;
    }
    return 0;
}

// Contribution of one axis to a region / tuple scalar (all values F2DOT14).
float axisFactor(int start, int peak, int end, int c) {
    if (start > peak || peak > end) return 1.f;
    if (start < 0 && end > 0 && peak != 0) return 1.f;
    if (peak == 0) return 1.f;
    if (c < start || c > end) return 0.f;
    if (c == peak) return 1.f;
    if (c < peak) return float(c - start) / float(peak - start);
    return float(end - c) / float(end - peak);
}

} // namespace

// ---- Item variation store ------------------------------------------------------------------------------------------
void ivsScalars(std::span<const std::uint8_t> d, std::uint32_t ivs, std::span<const std::int16_t> norm, std::vector<float>& out) {
    out.clear();
    const Rd r{d};
    if (!ivs || !r.ok(ivs, 8) || r.u16(ivs) != 1) return;
    const std::uint64_t rl = std::uint64_t(ivs) + r.u32(ivs + 2);
    if (!r.ok(rl, 4)) return;
    const std::uint16_t axes = r.u16(rl), regions = r.u16(rl + 2);
    if (!r.ok(rl + 4, std::uint64_t(axes) * regions * 6)) return;
    out.resize(regions);
    for (std::uint32_t k = 0; k < regions; ++k) {
        float s = 1.f;
        for (std::uint32_t a = 0; a < axes && s != 0.f; ++a) {
            const std::uint64_t q = rl + 4 + (std::uint64_t(k) * axes + a) * 6;
            s *= axisFactor(r.i16(q), r.i16(q + 2), r.i16(q + 4), a < norm.size() ? norm[a] : 0);
        }
        out[k] = s;
    }
}

float ivsDelta(std::span<const std::uint8_t> d, std::uint32_t ivs, std::uint32_t outer, std::uint32_t inner,
               std::span<const float> scalars) {
    const Rd r{d};
    if (!ivs || scalars.empty() || !r.ok(ivs, 8)) return 0.f;
    const std::uint16_t count = r.u16(ivs + 6);
    if (outer >= count || !r.ok(std::uint64_t(ivs) + 8, std::uint64_t(count) * 4)) return 0.f;
    const std::uint32_t off = r.u32(ivs + 8 + outer * 4);
    if (!off) return 0.f;
    const std::uint64_t v = std::uint64_t(ivs) + off;
    if (!r.ok(v, 6)) return 0.f;
    const std::uint16_t items = r.u16(v), wdc = r.u16(v + 2), ric = r.u16(v + 4);
    const bool longWords = (wdc & 0x8000u) != 0;
    const std::uint32_t words = std::min<std::uint32_t>(wdc & 0x7FFFu, ric);
    const std::uint32_t row = longWords ? words * 4 + (ric - words) * 2 : words * 2 + (ric - words);
    if (inner >= items || !r.ok(v + 6, std::uint64_t(ric) * 2 + std::uint64_t(items) * row)) return 0.f;
    std::uint64_t p = v + 6 + std::uint64_t(ric) * 2 + std::uint64_t(inner) * row;
    float sum = 0.f;
    for (std::uint32_t k = 0; k < ric; ++k) {
        int delta = 0;
        if (k < words) {
            delta = longWords ? int(std::int32_t(r.u32(p))) : r.i16(p);
            p += longWords ? 4 : 2;
        } else {
            delta = longWords ? r.i16(p) : std::int8_t(r.u8(p));
            p += longWords ? 2 : 1;
        }
        const std::uint16_t region = r.u16(v + 6 + k * 2);
        if (region < scalars.size()) sum += scalars[region] * float(delta);
    }
    return sum;
}

bool deltaSetIndex(std::span<const std::uint8_t> d, std::uint32_t map, std::uint32_t index, std::uint32_t& outer,
                   std::uint32_t& inner) {
    const Rd r{d};
    if (!map || !r.ok(map, 6)) return false;
    const std::uint8_t fmt = r.u8(map), ef = r.u8(map + 1);
    const std::uint32_t count = fmt == 0 ? r.u16(map + 2) : r.u32(map + 2);
    const std::uint64_t data = std::uint64_t(map) + (fmt == 0 ? 4 : 6);
    if (!count) return false;
    const std::uint32_t size = ((ef >> 4) & 3u) + 1, innerBits = (ef & 0xFu) + 1;
    const std::uint64_t at = data + std::uint64_t(std::min(index, count - 1)) * size;
    if (!r.ok(at, size)) return false;
    std::uint32_t e = 0;
    for (std::uint32_t k = 0; k < size; ++k) e = (e << 8) | r.u8(at + k);
    outer = e >> innerBits;
    inner = e & ((1u << innerBits) - 1u);
    return true;
}

// ---- Variation state -----------------------------------------------------------------------------------------------
namespace {

struct VarFile {
    std::span<const std::uint8_t> d;
    std::uint32_t fvar = 0, gvar = 0, hvar = 0, mvar = 0, glyf = 0, loca = 0, hmtx = 0;
    bool longLoca = false;
    std::uint16_t numGlyphs = 0, numHMetrics = 0;
    std::vector<VariationAxis> axes;
    std::vector<NamedInstance> named;
    std::vector<std::vector<std::array<std::int16_t, 2>>> avar; // per axis: (from, to) segment maps
    std::uint32_t avar2Map = 0, avar2Ivs = 0;                    // avar 2: axisIndexMap, ItemVariationStore (absolute)
    std::uint16_t gAxes = 0, gShared = 0, gGlyphs = 0;            // gvar header
    std::uint32_t gSharedOff = 0, gData = 0, gOffsets = 0;
    bool gLong = false;
};

bool parseFile(VarFile& f, std::span<const std::uint8_t> d, std::uint32_t start) {
    const Rd r{d};
    f.d = d;
    std::uint32_t nameLen = 0;
    const std::uint32_t name = findTable(r, start, "name", &nameLen);
    const std::span<const std::uint8_t> nameTable = name ? d.subspan(name, nameLen) : std::span<const std::uint8_t>{};
    f.fvar = findTable(r, start, "fvar");
    if (!f.fvar || !r.ok(f.fvar, 16)) return false;
    const std::uint32_t axesOff = f.fvar + r.u16(f.fvar + 4);
    const std::uint16_t axisCount = r.u16(f.fvar + 8), axisSize = r.u16(f.fvar + 10);
    const std::uint16_t instCount = r.u16(f.fvar + 12), instSize = r.u16(f.fvar + 14);
    if (axisCount == 0 || axisSize < 20 ||
        !r.ok(axesOff, std::uint64_t(axisCount) * axisSize + std::uint64_t(instCount) * instSize))
        return false;
    for (std::uint32_t a = 0; a < axisCount; ++a) {
        const std::uint32_t p = axesOff + a * axisSize;
        VariationAxis ax;
        ax.tag = r.u32(p);
        ax.minValue = r.fixed(p + 4);
        ax.defaultValue = r.fixed(p + 8);
        ax.maxValue = r.fixed(p + 12);
        ax.hidden = (r.u16(p + 16) & 1u) != 0;
        ax.name = fontNameString(nameTable, r.u16(p + 18));
        ax.minValue = std::min(ax.minValue, ax.defaultValue);
        ax.maxValue = std::max(ax.maxValue, ax.defaultValue);
        f.axes.push_back(std::move(ax));
    }
    if (instSize >= 4 + 4u * axisCount)
        for (std::uint32_t k = 0; k < instCount; ++k) {
            const std::uint32_t p = axesOff + axisCount * axisSize + k * instSize;
            NamedInstance ni;
            ni.name = fontNameString(nameTable, r.u16(p));
            for (std::uint32_t a = 0; a < axisCount; ++a) ni.coords.push_back(r.fixed(p + 4 + a * 4));
            f.named.push_back(std::move(ni));
        }
    if (const std::uint32_t avar = findTable(r, start, "avar");
        avar && r.ok(avar, 8) && (r.u16(avar) == 1 || r.u16(avar) == 2) && r.u16(avar + 6) == axisCount) {
        std::uint64_t p = avar + 8;
        for (std::uint32_t a = 0; a < axisCount; ++a) {
            if (!r.ok(p, 2)) break;
            const std::uint16_t n = r.u16(p);
            if (!r.ok(p + 2, std::uint64_t(n) * 4)) break;
            std::vector<std::array<std::int16_t, 2>> m;
            for (std::uint32_t k = 0; k < n; ++k) m.push_back({r.i16(p + 2 + k * 4), r.i16(p + 4 + k * 4)});
            f.avar.push_back(std::move(m));
            p += 2 + std::uint64_t(n) * 4;
        }
        if (f.avar.size() != axisCount) f.avar.clear();
        // avar 2 (OpenType 1.9.1): after the segment maps, Offset32 axisIndexMap and Offset32 varStore (from avar)
        else if (r.u16(avar) == 2 && r.ok(p, 8)) {
            const std::uint32_t map = r.u32(p), ivs = r.u32(p + 4);
            f.avar2Map = map ? avar + map : 0;
            f.avar2Ivs = ivs ? avar + ivs : 0;
        }
    }
    f.hvar = findTable(r, start, "HVAR");
    f.mvar = findTable(r, start, "MVAR");
    f.glyf = findTable(r, start, "glyf");
    f.loca = findTable(r, start, "loca");
    f.hmtx = findTable(r, start, "hmtx");
    if (const std::uint32_t head = findTable(r, start, "head"); head && r.ok(head, 54)) f.longLoca = r.i16(head + 50) != 0;
    if (const std::uint32_t maxp = findTable(r, start, "maxp"); maxp && r.ok(maxp, 6)) f.numGlyphs = r.u16(maxp + 4);
    if (const std::uint32_t hhea = findTable(r, start, "hhea"); hhea && r.ok(hhea, 36)) f.numHMetrics = r.u16(hhea + 34);
    if (const std::uint32_t gvar = findTable(r, start, "gvar"); gvar && r.ok(gvar, 20)) {
        f.gvar = gvar;
        f.gAxes = r.u16(gvar + 4);
        f.gShared = r.u16(gvar + 6);
        f.gSharedOff = gvar + r.u32(gvar + 8);
        f.gGlyphs = r.u16(gvar + 12);
        f.gLong = (r.u16(gvar + 14) & 1u) != 0;
        f.gData = gvar + r.u32(gvar + 16);
        f.gOffsets = gvar + 20;
        if (!r.ok(f.gOffsets, std::uint64_t(f.gGlyphs + 1) * (f.gLong ? 4 : 2)) ||
            !r.ok(f.gSharedOff, std::uint64_t(f.gShared) * f.gAxes * 2))
            f.gvar = 0;
    }
    return true;
}

// Design value -> normalised F2DOT14 (fvar default / min / max, then avar, as HarfBuzz does).
std::int16_t normalise(const VarFile& f, std::size_t a, float v) {
    const VariationAxis& ax = f.axes[a];
    v = std::clamp(v, ax.minValue, ax.maxValue);
    float n = 0.f;
    if (v < ax.defaultValue) n = (v - ax.defaultValue) / (ax.defaultValue - ax.minValue);
    else if (v > ax.defaultValue) n = (v - ax.defaultValue) / (ax.maxValue - ax.defaultValue);
    int q = int(std::lround(n * 16384.f));
    if (a < f.avar.size() && !f.avar[a].empty()) {
        const auto& m = f.avar[a];
        if (m.size() == 1) q = q - m[0][0] + m[0][1];
        else if (q <= m[0][0]) q = q - m[0][0] + m[0][1];
        else {
            std::size_t i = 1;
            while (i < m.size() - 1 && q > m[i][0]) ++i;
            if (q >= m[i][0]) q = q - m[i][0] + m[i][1];
            else if (m[i - 1][0] == m[i][0]) q = m[i - 1][1];
            else
                q = int(std::lround(float(m[i - 1][1]) +
                                    float(m[i][1] - m[i - 1][1]) * float(q - m[i - 1][0]) / float(m[i][0] - m[i - 1][0])));
        }
    }
    return std::int16_t(std::clamp(q, -16384, 16384));
}

struct Pt {
    float x = 0.f, y = 0.f;
};

// avar 2: every axis adds the delta of its item (axisIndexMap, else (0, axis)) evaluated at the coordinates after the
// segment maps, rounded, as HarfBuzz and FreeType do; so one axis can move another.
void applyAvar2(const VarFile& f, std::span<std::int16_t> norm) {
    if (!f.avar2Ivs) return;
    thread_local std::vector<float> scalars;
    ivsScalars(f.d, f.avar2Ivs, norm, scalars);
    if (scalars.empty()) return;
    std::array<std::int16_t, kMaxAxes> out{};
    for (std::size_t a = 0; a < norm.size(); ++a) {
        std::uint32_t outer = 0, inner = std::uint32_t(a);
        if (f.avar2Map && !deltaSetIndex(f.d, f.avar2Map, std::uint32_t(a), outer, inner)) outer = inner = 0xFFFFu;
        const float d = (outer == 0xFFFFu && inner == 0xFFFFu) ? 0.f : ivsDelta(f.d, f.avar2Ivs, outer, inner, scalars);
        out[a] = std::int16_t(std::clamp(int(norm[a]) + int(std::lround(d)), -16384, 16384));
    }
    std::copy(out.begin(), out.begin() + std::ptrdiff_t(norm.size()), norm.begin());
}

} // namespace

struct VarState {
    std::unique_ptr<VarFile> ownFile; // the owner's state holds the parsed tables
    const VarFile* file = nullptr;
    VarState* root = nullptr; // the owner's state: holds the instances
    std::size_t axisCount = 0;
    std::array<float, kMaxAxes> design{};
    std::array<std::int16_t, kMaxAxes> norm{};
    std::vector<float> hvarScalars;
    std::uint32_t hvarIvs = 0, hvarMap = 0;
    std::array<std::uint64_t, 256> advCache{}; // (glyph + 1) << 32 | float bits
    std::uint32_t pathGlyph = ~0u;             // last outline (glyphBox then rasterise ask for the same one)
    bool pathOk = false;
    std::vector<OutlineCmd> path;
    struct Instance {
        std::array<std::int16_t, kMaxAxes> key{};
        std::unique_ptr<Font> font;
    };
    std::vector<Instance> instances;
    std::size_t lastHit = 0;

    std::span<const std::int16_t> coords() const { return {norm.data(), axisCount}; }
};

namespace {

// ---- glyf + gvar -------------------------------------------------------------------------------------------------
struct Points {
    std::vector<Pt> p;
    std::vector<std::uint8_t> on;
    std::vector<std::uint32_t> ends; // last point of each contour (indices into p)
};

bool glyphRange(const VarFile& f, std::uint32_t g, std::uint64_t& at, std::uint32_t& len) {
    const Rd r{f.d};
    if (g >= f.numGlyphs || !f.loca || !f.glyf) return false;
    std::uint32_t a = 0, b = 0;
    if (f.longLoca) {
        if (!r.ok(f.loca + std::uint64_t(g) * 4, 8)) return false;
        a = r.u32(f.loca + g * 4);
        b = r.u32(f.loca + g * 4 + 4);
    } else {
        if (!r.ok(f.loca + std::uint64_t(g) * 2, 4)) return false;
        a = r.u16(f.loca + g * 2) * 2u;
        b = r.u16(f.loca + g * 2 + 2) * 2u;
    }
    if (b < a) return false;
    at = std::uint64_t(f.glyf) + a;
    len = b - a;
    return r.ok(at, len);
}

void hmtxOf(const VarFile& f, std::uint32_t g, int& aw, int& lsb) {
    const Rd r{f.d};
    aw = lsb = 0;
    const std::uint32_t n = f.numHMetrics;
    if (!f.hmtx || !n) return;
    if (g < n) {
        if (!r.ok(f.hmtx + std::uint64_t(g) * 4, 4)) return;
        aw = r.u16(f.hmtx + g * 4);
        lsb = r.i16(f.hmtx + g * 4 + 2);
    } else {
        if (r.ok(f.hmtx + std::uint64_t(n - 1) * 4, 4)) aw = r.u16(f.hmtx + (n - 1) * 4);
        const std::uint64_t q = f.hmtx + std::uint64_t(n) * 4 + std::uint64_t(g - n) * 2;
        if (r.ok(q, 2)) lsb = r.i16(q);
    }
}

// Packed point numbers (gvar "packed point numbers"); `all`: the record means every point.
bool readPoints(const Rd& r, std::uint64_t& p, std::uint64_t end, std::vector<std::uint16_t>& out, bool& all) {
    out.clear();
    all = false;
    if (p >= end) return false;
    std::uint32_t count = r.u8(p++);
    if (count == 0) {
        all = true;
        return true;
    }
    if (count & 0x80u) {
        if (p >= end) return false;
        count = ((count & 0x7Fu) << 8) | r.u8(p++);
    }
    std::uint32_t last = 0;
    while (out.size() < count) {
        if (p >= end) return false;
        const std::uint8_t c = r.u8(p++);
        const bool words = (c & 0x80u) != 0;
        for (std::uint32_t k = 0, run = (c & 0x7Fu) + 1; k < run && out.size() < count; ++k) {
            if (p + (words ? 2 : 1) > end) return false;
            last += words ? r.u16(p) : r.u8(p);
            p += words ? 2 : 1;
            out.push_back(std::uint16_t(last));
        }
    }
    return true;
}

// Packed deltas.
bool readDeltas(const Rd& r, std::uint64_t& p, std::uint64_t end, std::size_t count, std::vector<float>& out) {
    out.clear();
    while (out.size() < count) {
        if (p >= end) return false;
        const std::uint8_t c = r.u8(p++);
        const int size = (c & 0xC0u) == 0xC0u ? 4 : (c & 0x80u) ? 0 : (c & 0x40u) ? 2 : 1;
        for (std::uint32_t k = 0, run = (c & 0x3Fu) + 1; k < run && out.size() < count; ++k) {
            if (p + std::uint64_t(size) > end) return false;
            const int v = size == 4 ? int(std::int32_t(r.u32(p))) : size == 2 ? r.i16(p) : size == 1 ? std::int8_t(r.u8(p)) : 0;
            p += std::uint64_t(size);
            out.push_back(float(v));
        }
    }
    return true;
}

float interp(float c, float c1, float c2, float d1, float d2) {
    if (c1 == c2) return d1 == d2 ? d1 : 0.f;
    if (c1 > c2) {
        std::swap(c1, c2);
        std::swap(d1, d2);
    }
    if (c <= c1) return d1;
    if (c >= c2) return d2;
    return d1 + (c - c1) * (d2 - d1) / (c2 - c1);
}

// Interpolates deltas of untouched points per contour from the touched neighbours (gvar "IUP").
void iup(std::span<const Pt> orig, std::span<const std::uint32_t> ends, std::vector<float>& dx, std::vector<float>& dy,
         const std::vector<std::uint8_t>& touched) {
    std::size_t start = 0;
    for (const std::uint32_t endIdx : ends) {
        const std::size_t end = endIdx;
        if (end >= orig.size() || end < start) break;
        std::size_t first = end + 1, count = 0;
        for (std::size_t i = start; i <= end; ++i)
            if (touched[i]) {
                if (first > end) first = i;
                ++count;
            }
        if (count == 1) {
            for (std::size_t i = start; i <= end; ++i) {
                dx[i] = dx[first];
                dy[i] = dy[first];
            }
        } else if (count > 1) {
            const std::size_t n = end - start + 1;
            std::size_t i = first;
            do { // from each touched point to the next (cyclic), interpolate the untouched run between them
                std::size_t j = start + (i - start + 1) % n;
                while (!touched[j]) j = start + (j - start + 1) % n;
                for (std::size_t k = start + (i - start + 1) % n; k != j; k = start + (k - start + 1) % n) {
                    dx[k] = interp(orig[k].x, orig[i].x, orig[j].x, dx[i], dx[j]);
                    dy[k] = interp(orig[k].y, orig[i].y, orig[j].y, dy[i], dy[j]);
                }
                i = j;
            } while (i != first);
        }
        start = end + 1;
    }
}

// Adds glyph g's gvar deltas at `norm` to `pts` (its points then the 4 phantom points). `ends` are the contour ends
// relative to pts for IUP (simple glyphs); composites pass none (unreferenced components do not move).
void applyGvar(const VarFile& f, std::span<const std::int16_t> norm, std::uint32_t g, std::span<Pt> pts,
               std::span<const std::uint32_t> ends) {
    const Rd r{f.d};
    if (!f.gvar || g >= f.gGlyphs) return;
    const std::uint64_t o = f.gOffsets + std::uint64_t(g) * (f.gLong ? 4 : 2);
    const std::uint32_t a = f.gLong ? r.u32(o) : r.u16(o) * 2u, b = f.gLong ? r.u32(o + 4) : r.u16(o + 2) * 2u;
    const std::uint64_t base = std::uint64_t(f.gData) + a, end = std::uint64_t(f.gData) + b;
    if (b <= a + 4 || !r.ok(base, b - a)) return;
    const std::uint16_t tvc = r.u16(base);
    std::uint64_t data = base + r.u16(base + 2), hdr = base + 4;
    thread_local std::vector<std::uint16_t> shared, priv;
    thread_local std::vector<float> dx, dy, ix, iy;
    thread_local std::vector<std::uint8_t> touched;
    thread_local std::vector<Pt> orig;
    bool sharedAll = false;
    shared.clear();
    if ((tvc & 0x8000u) && !readPoints(r, data, end, shared, sharedAll)) return;
    const std::size_t n = pts.size();
    const std::uint32_t axes = f.gAxes;
    orig.assign(pts.begin(), pts.end()); // IUP interpolates over the default positions
    for (std::uint32_t t = 0, count = tvc & 0x0FFFu; t < count; ++t) {
        if (hdr + 4 > end) return;
        const std::uint16_t size = r.u16(hdr), idx = r.u16(hdr + 2);
        hdr += 4;
        std::uint64_t peak = 0, istart = 0, iend = 0;
        if (idx & 0x8000u) {
            peak = hdr;
            hdr += 2ull * axes;
        } else {
            if ((idx & 0x0FFFu) >= f.gShared) return;
            peak = f.gSharedOff + std::uint64_t(idx & 0x0FFFu) * axes * 2;
        }
        const bool inter = (idx & 0x4000u) != 0;
        if (inter) {
            istart = hdr;
            iend = hdr + 2ull * axes;
            hdr += 4ull * axes;
        }
        if (hdr > end || !r.ok(peak, 2ull * axes)) return;
        float s = 1.f;
        for (std::uint32_t ax = 0; ax < axes && s != 0.f; ++ax) {
            const int pk = r.i16(peak + ax * 2);
            const int st = inter ? r.i16(istart + ax * 2) : std::min(0, pk), en = inter ? r.i16(iend + ax * 2) : std::max(0, pk);
            s *= axisFactor(st, pk, en, ax < norm.size() ? norm[ax] : 0);
        }
        std::uint64_t p = data;
        data += size;
        if (data > end) return;
        if (s == 0.f) continue;
        const std::vector<std::uint16_t>* list = &shared;
        bool all = sharedAll;
        if ((idx & 0x2000u) && !readPoints(r, p, data, priv, all)) continue;
        if (idx & 0x2000u) list = &priv;
        const std::size_t m = all ? n : list->size();
        if (!readDeltas(r, p, data, m, dx) || !readDeltas(r, p, data, m, dy)) continue;
        if (all) {
            for (std::size_t i = 0; i < n; ++i) {
                pts[i].x += s * dx[i];
                pts[i].y += s * dy[i];
            }
            continue;
        }
        if (ends.empty()) {
            for (std::size_t k = 0; k < m; ++k)
                if ((*list)[k] < n) {
                    pts[(*list)[k]].x += s * dx[k];
                    pts[(*list)[k]].y += s * dy[k];
                }
            continue;
        }
        ix.assign(n, 0.f);
        iy.assign(n, 0.f);
        touched.assign(n, 0);
        for (std::size_t k = 0; k < m; ++k)
            if (const std::size_t i = (*list)[k]; i < n) {
                ix[i] = dx[k];
                iy[i] = dy[k];
                touched[i] = 1;
            }
        iup(std::span<const Pt>(orig.data(), n >= 4 ? n - 4 : 0), ends, ix, iy, touched);
        for (std::size_t i = 0; i < n; ++i) {
            pts[i].x += s * ix[i];
            pts[i].y += s * iy[i];
        }
    }
}

// Appends the varied points of glyph g (components resolved and placed) to `out`; phantom points in `ph`.
bool loadPoints(const VarFile& f, std::span<const std::int16_t> norm, std::uint32_t g, int depth, Points& out,
                std::array<Pt, 4>& ph) {
    const Rd r{f.d};
    std::uint64_t at = 0;
    std::uint32_t len = 0;
    if (depth > kMaxComponentDepth || !glyphRange(f, g, at, len)) return false;
    int aw = 0, lsb = 0;
    hmtxOf(f, g, aw, lsb);
    const int contours = len >= 10 ? r.i16(at) : 0;
    const float xMin = len >= 10 ? float(r.i16(at + 2)) : 0.f;
    ph = {Pt{xMin - float(lsb), 0.f}, Pt{xMin - float(lsb) + float(aw), 0.f}, Pt{}, Pt{}};
    thread_local std::vector<Pt> tmp;
    if (contours >= 0) { // simple glyph (or empty)
        const std::size_t base = out.p.size(), endsBase = out.ends.size();
        if (contours > 0) {
            const std::uint64_t end = at + len;
            std::uint64_t p = at + 10;
            if (p + std::uint64_t(contours) * 2 + 2 > end) return false;
            std::uint32_t prev = 0;
            for (int c = 0; c < contours; ++c) {
                const std::uint32_t e = r.u16(p + std::uint64_t(c) * 2);
                if ((c && e < prev) || e >= kMaxPoints) return false;
                prev = e;
                out.ends.push_back(std::uint32_t(base) + e);
            }
            const std::uint32_t nPts = prev + 1;
            if (base + nPts > kMaxPoints) return false;
            p += std::uint64_t(contours) * 2;
            p += 2 + r.u16(p);
            out.p.resize(base + nPts);
            out.on.resize(base + nPts);
            for (std::uint32_t i = 0; i < nPts;) { // flags (bit 3: repeat)
                if (p >= end) return false;
                const std::uint8_t fl = r.u8(p++);
                std::uint32_t rep = 1;
                if (fl & 8u) {
                    if (p >= end) return false;
                    rep += r.u8(p++);
                }
                for (; rep && i < nPts; --rep, ++i) out.on[base + i] = fl;
            }
            for (int axis = 0; axis < 2; ++axis) {
                const std::uint8_t shortBit = axis ? 4u : 2u, sameBit = axis ? 32u : 16u;
                int v = 0;
                for (std::uint32_t i = 0; i < nPts; ++i) {
                    const std::uint8_t fl = out.on[base + i];
                    if (fl & shortBit) {
                        if (p >= end) return false;
                        v += (fl & sameBit) ? r.u8(p) : -int(r.u8(p));
                        ++p;
                    } else if (!(fl & sameBit)) {
                        if (p + 2 > end) return false;
                        v += r.i16(p);
                        p += 2;
                    }
                    (axis ? out.p[base + i].y : out.p[base + i].x) = float(v);
                }
            }
            for (std::uint32_t i = 0; i < nPts; ++i) out.on[base + i] &= 1u;
        }
        tmp.assign(out.p.begin() + std::ptrdiff_t(base), out.p.end());
        tmp.insert(tmp.end(), ph.begin(), ph.end());
        thread_local std::vector<std::uint32_t> ends;
        ends.clear();
        for (std::size_t k = endsBase; k < out.ends.size(); ++k) ends.push_back(out.ends[k] - std::uint32_t(base));
        applyGvar(f, norm, g, tmp, ends);
        std::copy(tmp.begin(), tmp.end() - 4, out.p.begin() + std::ptrdiff_t(base));
        std::copy(tmp.end() - 4, tmp.end(), ph.begin());
        return true;
    }
    // composite: component offsets are the glyph's "points" for gvar
    struct Comp {
        std::uint16_t flags = 0, glyph = 0;
        float a1 = 0.f, a2 = 0.f;
        float xx = 1.f, yx = 0.f, xy = 0.f, yy = 1.f;
    };
    std::vector<Comp> comps;
    const std::uint64_t end = at + len;
    std::uint64_t p = at + 10;
    for (;;) {
        if (p + 4 > end || comps.size() > 256) return false;
        Comp c;
        c.flags = r.u16(p);
        c.glyph = r.u16(p + 2);
        p += 4;
        const bool xy = (c.flags & 2u) != 0;
        if (c.flags & 1u) {
            if (p + 4 > end) return false;
            c.a1 = xy ? float(r.i16(p)) : float(r.u16(p));
            c.a2 = xy ? float(r.i16(p + 2)) : float(r.u16(p + 2));
            p += 4;
        } else {
            if (p + 2 > end) return false;
            c.a1 = xy ? float(std::int8_t(r.u8(p))) : float(r.u8(p));
            c.a2 = xy ? float(std::int8_t(r.u8(p + 1))) : float(r.u8(p + 1));
            p += 2;
        }
        const auto f2 = [&](std::uint64_t q) { return float(r.i16(q)) / 16384.f; };
        if (c.flags & 8u) {
            if (p + 2 > end) return false;
            c.xx = c.yy = f2(p);
            p += 2;
        } else if (c.flags & 0x40u) {
            if (p + 4 > end) return false;
            c.xx = f2(p);
            c.yy = f2(p + 2);
            p += 4;
        } else if (c.flags & 0x80u) {
            if (p + 8 > end) return false;
            c.xx = f2(p);
            c.yx = f2(p + 2);
            c.xy = f2(p + 4);
            c.yy = f2(p + 6);
            p += 8;
        }
        comps.push_back(c);
        if (!(c.flags & 0x20u)) break;
    }
    std::vector<Pt> offs(comps.size() + 4);
    for (std::size_t k = 0; k < comps.size(); ++k) offs[k] = {comps[k].a1, comps[k].a2};
    std::copy(ph.begin(), ph.end(), offs.end() - 4);
    applyGvar(f, norm, g, offs, {});
    std::copy(offs.end() - 4, offs.end(), ph.begin());
    const std::size_t compositeBase = out.p.size();
    for (std::size_t k = 0; k < comps.size(); ++k) {
        const Comp& c = comps[k];
        const std::size_t base = out.p.size();
        std::array<Pt, 4> cph;
        if (!loadPoints(f, norm, c.glyph, depth + 1, out, cph)) continue;
        for (std::size_t i = base; i < out.p.size(); ++i) {
            const Pt q = out.p[i];
            out.p[i] = {c.xx * q.x + c.xy * q.y, c.yx * q.x + c.yy * q.y};
        }
        Pt off{};
        if (c.flags & 2u) {
            off = offs[k];
            if ((c.flags & 0x800u) && !(c.flags & 0x1000u)) // SCALED_COMPONENT_OFFSET
                off = {c.xx * off.x + c.xy * off.y, c.yx * off.x + c.yy * off.y};
        } else { // point matching: parent point a1 meets child point a2
            const std::size_t pi = compositeBase + std::size_t(c.a1), ci = base + std::size_t(c.a2);
            if (pi < base && ci < out.p.size()) off = {out.p[pi].x - out.p[ci].x, out.p[pi].y - out.p[ci].y};
        }
        for (std::size_t i = base; i < out.p.size(); ++i) {
            out.p[i].x += off.x;
            out.p[i].y += off.y;
        }
        if (c.flags & 0x200u) ph = cph; // USE_MY_METRICS
    }
    return true;
}

// Varied outline of glyph g in font units, origin at the (varied) left phantom point.
bool buildOutline(VarState* v, std::uint32_t g, std::vector<OutlineCmd>& path, float* advance) {
    path.clear();
    thread_local Points pts;
    pts.p.clear();
    pts.on.clear();
    pts.ends.clear();
    std::array<Pt, 4> ph{};
    if (!loadPoints(*v->file, v->coords(), g, 0, pts, ph)) return false;
    if (advance) *advance = ph[1].x - ph[0].x;
    const float sx = -ph[0].x;
    std::size_t start = 0;
    for (const std::uint32_t endIdx : pts.ends) {
        const std::size_t end = endIdx;
        if (end >= pts.p.size() || end < start) break;
        const std::size_t n = end - start + 1;
        auto P = [&](std::size_t k) { return Pt{pts.p[start + k % n].x + sx, pts.p[start + k % n].y}; };
        auto on = [&](std::size_t k) { return pts.on[start + k % n] != 0; };
        // start on an on-curve point (or the midpoint of two off-curve ones), then visit the rest in order
        Pt first;
        std::size_t from = 0, count = n;
        if (on(0)) {
            first = P(0);
            from = 1;
            count = n - 1;
        } else if (on(n - 1)) {
            first = P(n - 1);
            count = n - 1;
        } else {
            first = {(P(0).x + P(n - 1).x) * 0.5f, (P(0).y + P(n - 1).y) * 0.5f};
        }
        path.push_back({OutlineCmd::Move, first.x, first.y});
        bool haveCtl = false;
        Pt ctl;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t k = from + i;
            const Pt q = P(k);
            if (on(k)) {
                if (haveCtl) path.push_back({OutlineCmd::Quad, q.x, q.y, ctl.x, ctl.y});
                else path.push_back({OutlineCmd::Line, q.x, q.y});
                haveCtl = false;
            } else {
                if (haveCtl) {
                    const Pt mid{(ctl.x + q.x) * 0.5f, (ctl.y + q.y) * 0.5f};
                    path.push_back({OutlineCmd::Quad, mid.x, mid.y, ctl.x, ctl.y});
                }
                ctl = q;
                haveCtl = true;
            }
        }
        if (haveCtl) path.push_back({OutlineCmd::Quad, first.x, first.y, ctl.x, ctl.y});
        else path.push_back({OutlineCmd::Line, first.x, first.y});
        start = end + 1;
    }
    return true;
}

const std::vector<OutlineCmd>* cachedOutline(VarState* v, std::uint32_t g) {
    if (v->pathGlyph != g) {
        v->pathGlyph = g;
        v->pathOk = buildOutline(v, g, v->path, nullptr);
    }
    return v->pathOk && !v->path.empty() ? &v->path : nullptr;
}

bool pathBox(const std::vector<OutlineCmd>& path, float scale, int& x0, int& y0, int& x1, int& y1) {
    float b[4] = {1e30f, 1e30f, -1e30f, -1e30f};
    for (const OutlineCmd& c : path) {
        const bool ctl = c.kind == OutlineCmd::Quad || c.kind == OutlineCmd::Cubic;
        b[0] = std::min({b[0], c.x, ctl ? c.cx : c.x});
        b[1] = std::min({b[1], c.y, ctl ? c.cy : c.y});
        b[2] = std::max({b[2], c.x, ctl ? c.cx : c.x});
        b[3] = std::max({b[3], c.y, ctl ? c.cy : c.y});
    }
    if (b[2] < b[0]) return false;
    x0 = int(std::floor(b[0] * scale));
    y0 = int(std::floor(-b[3] * scale));
    x1 = int(std::ceil(b[2] * scale));
    y1 = int(std::ceil(-b[1] * scale));
    return true;
}

// ---- hooks -------------------------------------------------------------------------------------------------------
float hookAdvance(VarState* v, std::uint32_t g) {
    std::uint64_t& slot = v->advCache[g & 255u];
    if ((slot >> 32) == std::uint64_t(g) + 1) return std::bit_cast<float>(std::uint32_t(slot));
    const VarFile& f = *v->file;
    float adv = 0.f;
    if (v->hvarIvs) {
        int aw = 0, lsb = 0;
        hmtxOf(f, g, aw, lsb);
        std::uint32_t outer = 0, inner = g;
        if (v->hvarMap && !deltaSetIndex(f.d, v->hvarMap, g, outer, inner)) outer = inner = 0xFFFFu;
        adv = float(aw) + ivsDelta(f.d, v->hvarIvs, outer, inner, v->hvarScalars);
    } else { // no HVAR: the varied phantom points
        thread_local std::vector<OutlineCmd> path;
        if (!buildOutline(v, g, path, &adv)) {
            int aw = 0, lsb = 0;
            hmtxOf(f, g, aw, lsb);
            adv = float(aw);
        }
    }
    slot = ((std::uint64_t(g) + 1) << 32) | std::bit_cast<std::uint32_t>(adv);
    return adv;
}

bool hookOutline(VarState* v, std::uint32_t g, std::vector<OutlineCmd>& out) {
    const std::vector<OutlineCmd>* p = cachedOutline(v, g);
    if (p) out = *p;
    else out.clear();
    return p != nullptr;
}

void hookGlyphBox(VarState* v, std::uint32_t g, float scale, int& x0, int& y0, int& x1, int& y1) {
    x0 = y0 = x1 = y1 = 0;
    if (const std::vector<OutlineCmd>* p = cachedOutline(v, g)) pathBox(*p, scale, x0, y0, x1, y1);
}

void hookRasterise(VarState* v, std::uint32_t g, float scale, std::uint8_t* dst, int w, int h, int stride) {
    const std::vector<OutlineCmd>* p = cachedOutline(v, g);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!p || w <= 0 || h <= 0 || !pathBox(*p, scale, x0, y0, x1, y1)) return;
    thread_local std::vector<float> cov;
    cov.assign(std::size_t(w) * std::size_t(h), 0.f);
    rasterisePath(*p, Affine{scale, 0, 0, -scale, double(-x0), double(-y0)}, cov.data(), w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            dst[std::size_t(y) * std::size_t(stride) + std::size_t(x)] =
                std::uint8_t(std::clamp(cov[std::size_t(y) * std::size_t(w) + std::size_t(x)], 0.f, 1.f) * 255.f + 0.5f);
}

// Derived per-coordinate data (HVAR region scalars, caches).
void derive(VarState* v) {
    const VarFile& f = *v->file;
    const Rd r{f.d};
    v->hvarIvs = v->hvarMap = 0;
    v->hvarScalars.clear();
    if (f.hvar && r.ok(f.hvar, 12) && r.u32(f.hvar + 4)) {
        v->hvarIvs = f.hvar + r.u32(f.hvar + 4);
        if (const std::uint32_t m = r.u32(f.hvar + 8)) v->hvarMap = f.hvar + m;
        ivsScalars(f.d, v->hvarIvs, v->coords(), v->hvarScalars);
    }
    v->advCache.fill(0);
    v->pathGlyph = ~0u;
}

// MVAR deltas of the hhea / OS/2 ascender, descender and line gap (font units).
void mvarDeltas(const VarState* v, float& dAscent, float& dDescent, float& dLineGap) {
    dAscent = dDescent = dLineGap = 0.f;
    const VarFile& f = *v->file;
    const Rd r{f.d};
    if (!f.mvar || !r.ok(f.mvar, 12) || !r.u16(f.mvar + 10)) return;
    const std::uint16_t recSize = r.u16(f.mvar + 6), count = r.u16(f.mvar + 8);
    const std::uint32_t ivs = f.mvar + r.u16(f.mvar + 10);
    if (recSize < 8 || !r.ok(std::uint64_t(f.mvar) + 12, std::uint64_t(recSize) * count)) return;
    std::vector<float> scalars;
    ivsScalars(f.d, ivs, v->coords(), scalars);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t p = f.mvar + 12 + k * recSize;
        const std::uint32_t tag = r.u32(p);
        float* dst = tag == fontTag("hasc") ? &dAscent : tag == fontTag("hdsc") ? &dDescent : tag == fontTag("hlgp") ? &dLineGap : nullptr;
        if (dst) *dst = ivsDelta(f.d, ivs, r.u16(p + 4), r.u16(p + 6), scalars);
    }
}

// The font's variation state, made on first use (nullptr: not a variable font, or unusable tables).
VarState* ensureVar(const Font& font) {
    VarState*& v = FontAccess::var(font);
    if (!v) {
        if (!font.isVariable()) return nullptr;
        v = new VarState;
        v->root = v;
        auto file = std::make_unique<VarFile>();
        if (parseFile(*file, FontAccess::file(font), FontAccess::fontStart(font))) {
            v->file = file.get();
            v->ownFile = std::move(file);
            v->axisCount = std::min(v->file->axes.size(), kMaxAxes);
            for (std::size_t a = 0; a < v->axisCount; ++a) v->design[a] = v->file->axes[a].defaultValue;
            derive(v);
        }
    }
    return v->axisCount ? v : nullptr;
}

// Design and normalised coordinates of `settings` over v's own.
void resolve(const VarState* v, std::span<const FontVariation> settings, std::array<float, kMaxAxes>& design,
             std::array<std::int16_t, kMaxAxes>& key) {
    design = v->design;
    key = {};
    for (const FontVariation& s : settings)
        for (std::size_t a = 0; a < v->axisCount; ++a)
            if (v->file->axes[a].tag == s.tag)
                design[a] = std::clamp(s.value, v->file->axes[a].minValue, v->file->axes[a].maxValue);
    for (std::size_t a = 0; a < v->axisCount; ++a) key[a] = normalise(*v->file, a, design[a]);
    applyAvar2(*v->file, std::span<std::int16_t>(key.data(), v->axisCount));
}

const Font& instanceAt(const Font& from, VarState* v, const std::array<float, kMaxAxes>& design,
                       const std::array<std::int16_t, kMaxAxes>& key) {
    VarState* root = v->root;
    auto& inst = root->instances;
    if (root->lastHit < inst.size() && inst[root->lastHit].key == key) return *inst[root->lastHit].font;
    for (std::size_t i = 0; i < inst.size(); ++i)
        if (inst[i].key == key) {
            root->lastHit = i;
            return *inst[i].font;
        }
    if (inst.size() >= kMaxInstances) { // full: the nearest existing instance
        std::size_t best = 0;
        int bestCost = 0x7FFFFFFF;
        for (std::size_t i = 0; i < inst.size(); ++i) {
            int cost = 0;
            for (std::size_t a = 0; a < root->axisCount; ++a) cost += std::abs(int(inst[i].key[a]) - int(key[a]));
            if (cost < bestCost) {
                bestCost = cost;
                best = i;
            }
        }
        return *inst[best].font;
    }
    auto* nv = new VarState;
    nv->file = root->file;
    nv->root = root;
    nv->axisCount = root->axisCount;
    nv->design = design;
    nv->norm = key;
    derive(nv);
    float dA = 0.f, dD = 0.f, dG = 0.f;
    mvarDeltas(nv, dA, dD, dG);
    inst.push_back({key, FontAccess::makeInstance(from, nv, nv->coords(), dA, dD, dG)});
    root->lastHit = inst.size() - 1;
    return *inst.back().font;
}

} // namespace
} // namespace fuse::text::detail

namespace fuse::text {
using detail::VarState;

namespace {
std::span<const VariationAxis> apiAxes(const Font& f) {
    const VarState* v = detail::ensureVar(f);
    return v ? std::span<const VariationAxis>(v->file->axes) : std::span<const VariationAxis>{};
}

std::span<const NamedInstance> apiNamed(const Font& f) {
    const VarState* v = detail::ensureVar(f);
    return v ? std::span<const NamedInstance>(v->file->named) : std::span<const NamedInstance>{};
}

std::span<const float> apiCoords(const Font& f) {
    const VarState* v = detail::ensureVar(f);
    return v ? std::span<const float>(v->design.data(), v->axisCount) : std::span<const float>{};
}

const Font& apiInstance(const Font& f, std::span<const FontVariation> settings) {
    VarState* v = detail::ensureVar(f);
    if (!v || settings.empty()) return f;
    std::array<float, detail::kMaxAxes> design;
    std::array<std::int16_t, detail::kMaxAxes> key;
    detail::resolve(v, settings, design, key);
    if (key == v->norm) return f;
    return detail::instanceAt(f, v, design, key);
}

void apiSetVariation(Font& f, std::span<const FontVariation> settings) {
    VarState* v = detail::ensureVar(f);
    if (!v || v->root != v) return; // instances are fixed
    detail::resolve(v, settings, v->design, v->norm);
    detail::derive(v);
    float dA = 0.f, dD = 0.f, dG = 0.f;
    detail::mvarDeltas(v, dA, dD, dG);
    detail::FontAccess::setVaried(f, v->coords(), dA, dD, dG);
}
} // namespace

void enableVariableFonts() {
    detail::VarHooks& h = detail::varHooks();
    if (h.destroy) return;
    h.destroy = [](VarState* v) { delete v; };
    h.advance = &detail::hookAdvance;
    h.outline = &detail::hookOutline;
    h.glyphBox = &detail::hookGlyphBox;
    h.rasterise = &detail::hookRasterise;
    h.axes = &apiAxes;
    h.named = &apiNamed;
    h.coords = &apiCoords;
    h.instance = &apiInstance;
    h.setVariation = &apiSetVariation;
    detail::namedInstanceLister() = &detail::catalogNamedInstances;
}

} // namespace fuse::text
