// .fuseicons loader and resampler (see icons.hpp).
#include "fuse/ui/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace fuse::ui {

namespace {
struct Reader {
    std::span<const u8> b;
    std::size_t at = 0;
    bool ok = true;
    bool take(void* p, std::size_t n) {
        if (!ok || n > b.size() - at) { ok = false; return false; }
        std::memcpy(p, b.data() + at, n);
        at += n;
        return true;
    }
    u16 u16_() { u16 v = 0; take(&v, 2); return v; }
    u32 u32_() { u32 v = 0; take(&v, 4); return v; }
};

// 1D area resample of `n` lines of `src` (length s, stride ss, step between lines ls) to length d.
void resample1d(const f32* src, int s, int ss, int ls, f32* dst, int d, int ds, int ld, int n) {
    const f32 k = f32(s) / f32(d);
    for (int line = 0; line < n; ++line) {
        const f32* in = src + std::ptrdiff_t(line) * ls;
        f32* out = dst + std::ptrdiff_t(line) * ld;
        for (int i = 0; i < d; ++i) {
            const f32 a = f32(i) * k, b = f32(i + 1) * k;
            f32 acc = 0.f;
            for (int j = int(std::floor(a)); j < int(std::ceil(b)) && j < s; ++j) {
                const f32 w = std::min(b, f32(j + 1)) - std::max(a, f32(j));
                if (w > 0.f) acc += in[std::ptrdiff_t(j) * ss] * w;
            }
            out[std::ptrdiff_t(i) * ds] = acc / k;
        }
    }
}
} // namespace

std::expected<IconSet, std::string> IconSet::fromBytes(std::span<const u8> bytes) {
    Reader r{bytes};
    char magic[4] = {};
    if (!r.take(magic, 4) || std::memcmp(magic, kIconPackMagic, 4) != 0) return std::unexpected("not an icon pack (bad magic)");
    const u32 version = r.u32_();
    if (version != 1 && version != kIconPackVersion) return std::unexpected("icon pack: unsupported version");
    IconSet s;
    s.m_atlasW = r.u16_();
    s.m_atlasH = r.u16_();
    const u16 nSizes = r.u16_();
    if (!r.ok || nSizes == 0 || nSizes > 64) return std::unexpected("icon pack: bad size list");
    s.m_sizes.resize(nSizes);
    for (u16& z : s.m_sizes) {
        z = r.u16_();
        if (z == 0 || z > 1024) return std::unexpected("icon pack: bad size");
    }
    const u32 n = r.u32_();
    if (!r.ok || std::size_t(n) * (6 + 12 * std::size_t(nSizes)) > bytes.size() - r.at) return std::unexpected("icon pack: bad icon count");
    s.m_names.resize(n);
    s.m_hashes.resize(n);
    s.m_layerStart.assign(1, 0u);
    for (u32 i = 0; i < n && r.ok; ++i) {
        s.m_hashes[i] = r.u32_();
        const u16 len = r.u16_();
        if (!r.ok || len == 0 || len > bytes.size() - r.at) return std::unexpected("icon pack: bad name");
        s.m_names[i].assign(reinterpret_cast<const char*>(bytes.data() + r.at), len);
        r.at += len;
        if (hashId(s.m_names[i]) != s.m_hashes[i]) return std::unexpected("icon pack: name hash mismatch");
        if (i && !(s.m_names[i - 1] < s.m_names[i])) return std::unexpected("icon pack: names not sorted / unique");
        const u16 layers = version == 1 ? u16(1) : r.u16_();
        if (!r.ok || layers == 0 || layers > 255 || std::size_t(layers) * (4 + 12 * std::size_t(nSizes)) > bytes.size() - r.at)
            return std::unexpected("icon pack: bad layer count");
        for (u16 l = 0; l < layers; ++l) {
            s.m_layerColors.push_back(version == 1 ? 0u : r.u32_());
            for (u16 k = 0; k < nSizes; ++k) {
                Entry e;
                e.u = r.u16_(); e.v = r.u16_(); e.w = r.u16_(); e.h = r.u16_(); e.ox = r.u16_(); e.oy = r.u16_();
                const bool fits = std::size_t(e.u) + e.w <= std::size_t(s.m_atlasW) && std::size_t(e.v) + e.h <= std::size_t(s.m_atlasH) &&
                                  std::size_t(e.ox) + e.w <= s.m_sizes[k] && std::size_t(e.oy) + e.h <= s.m_sizes[k];
                if (r.ok && !fits) return std::unexpected("icon pack: entry outside atlas / cell");
                s.m_entries.push_back(e);
            }
        }
        s.m_layerStart.push_back(s.m_layerStart.back() + layers);
    }
    const std::size_t atlasBytes = std::size_t(s.m_atlasW) * std::size_t(s.m_atlasH);
    if (!r.ok || bytes.size() - r.at != atlasBytes) return std::unexpected("icon pack: truncated or corrupt");
    s.m_atlas.assign(bytes.begin() + std::ptrdiff_t(r.at), bytes.end());
    s.m_byHash.resize(n);
    for (u32 i = 0; i < n; ++i) s.m_byHash[i] = i;
    std::sort(s.m_byHash.begin(), s.m_byHash.end(), [&](u32 a, u32 b) { return s.m_hashes[a] < s.m_hashes[b]; });
    for (u32 i = 1; i < n; ++i)
        if (s.m_hashes[s.m_byHash[i]] == s.m_hashes[s.m_byHash[i - 1]]) return std::unexpected("icon pack: duplicate name hash");
    return s;
}

std::expected<IconSet, std::string> IconSet::fromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("cannot open " + path);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return fromBytes(bytes);
}

i32 IconSet::find(std::string_view name) const {
    const auto it = std::lower_bound(m_names.begin(), m_names.end(), name, [](const std::string& a, std::string_view b) { return a < b; });
    return it != m_names.end() && *it == name ? i32(it - m_names.begin()) : -1;
}

i32 IconSet::findHash(u32 h) const {
    const auto it = std::lower_bound(m_byHash.begin(), m_byHash.end(), h, [&](u32 i, u32 v) { return m_hashes[i] < v; });
    return it != m_byHash.end() && m_hashes[*it] == h ? i32(*it) : -1;
}

bool IconSet::cooked(u32 icon, int size, IconBitmap& out, u32 layer) const {
    if (icon >= count() || layer >= layerCount(icon)) return false;
    for (std::size_t k = 0; k < m_sizes.size(); ++k) {
        if (m_sizes[k] != size) continue;
        const Entry& e = entry(icon, u32(k), layer);
        out = {m_atlas.data() + std::size_t(e.v) * std::size_t(m_atlasW) + e.u, e.w, e.h, m_atlasW, e.ox, e.oy};
        return true;
    }
    return false;
}

void IconSet::render(u32 icon, int size, std::vector<u8>& out, u32 layer) const {
    out.assign(std::size_t(std::max(size, 0)) * std::size_t(std::max(size, 0)), 0);
    if (size <= 0 || icon >= count() || layer >= layerCount(icon)) return;
    // source: the smallest cooked size >= size, else the largest
    std::size_t best = 0;
    for (std::size_t k = 0; k < m_sizes.size(); ++k) {
        const bool kOk = m_sizes[k] >= size, bOk = m_sizes[best] >= size;
        if ((kOk && (!bOk || m_sizes[k] < m_sizes[best])) || (!kOk && !bOk && m_sizes[k] > m_sizes[best])) best = k;
    }
    const int s = m_sizes[best];
    const Entry& e = entry(icon, u32(best), layer);
    if (s == size) {
        for (int y = 0; y < e.h; ++y)
            std::memcpy(&out[std::size_t(y + e.oy) * std::size_t(size) + e.ox], &m_atlas[std::size_t(e.v + y) * std::size_t(m_atlasW) + e.u], e.w);
        return;
    }
    std::vector<f32> cell(std::size_t(s) * std::size_t(s), 0.f), tmp(std::size_t(size) * std::size_t(s)), dst(std::size_t(size) * std::size_t(size));
    for (int y = 0; y < e.h; ++y)
        for (int x = 0; x < e.w; ++x)
            cell[std::size_t(y + e.oy) * std::size_t(s) + std::size_t(x + e.ox)] = f32(m_atlas[std::size_t(e.v + y) * std::size_t(m_atlasW) + std::size_t(e.u + x)]);
    resample1d(cell.data(), s, 1, s, tmp.data(), size, 1, size, s);        // rows: s x s -> size wide, s tall
    resample1d(tmp.data(), s, size, 1, dst.data(), size, size, 1, size);   // columns: -> size x size
    for (std::size_t i = 0; i < dst.size(); ++i) out[i] = u8(std::clamp(std::lround(dst[i]), 0l, 255l));
}

} // namespace fuse::ui
