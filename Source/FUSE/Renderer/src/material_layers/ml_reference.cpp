// Asset plan W0.7: CPU side of layered materials. See include/fuse/renderer/material_layers/ml_reference.hpp.
#include <fuse/renderer/material_layers/ml_reference.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::renderer::material_layers {

namespace {

f64 srgbToLinear(f64 c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
f64 linearToSrgb(f64 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

u8 toByte(f64 v) {
    const f64 c = std::clamp(v, 0.0, 1.0);
    return static_cast<u8>(std::lround(c * 255.0));
}

u32 pack(u8 r, u8 g, u8 b, u8 a) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}

// Periodic (tileable) value noise and fBm, f64: deterministic across compilers (only +, *, floor).
f64 lattice(i32 x, i32 y, i32 period, u32 seed) {
    const u32 px = static_cast<u32>(((x % period) + period) % period);
    const u32 py = static_cast<u32>(((y % period) + period) % period);
    return static_cast<f64>(ml_hash(px * 0x8da6b343u ^ py * 0xd8163841u ^ seed) >> 8) / 16777216.0;
}

f64 valueNoise(f64 x, f64 y, i32 period, u32 seed) {
    const f64 fx = std::floor(x), fy = std::floor(y);
    const f64 tx = x - fx, ty = y - fy;
    const f64 ux = tx * tx * (3.0 - 2.0 * tx), uy = ty * ty * (3.0 - 2.0 * ty);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy);
    const f64 a = lattice(ix, iy, period, seed), b = lattice(ix + 1, iy, period, seed);
    const f64 c = lattice(ix, iy + 1, period, seed), d = lattice(ix + 1, iy + 1, period, seed);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

/// fBm over the unit square (u, v in [0, 1)), `base` lattice cells at the first octave; tileable.
f64 fbm(f64 u, f64 v, i32 base, u32 octaves, u32 seed) {
    f64 sum = 0.0, amp = 0.5, norm = 0.0;
    i32 period = base;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(u * period, v * period, period, seed + o * 1013u);
        norm += amp;
        amp *= 0.5;
        period *= 2;
    }
    return sum / norm;
}

struct Kind {
    const char* name;
    i32 base;
    u32 octaves;
    u32 seed;
    f64 color[3];  ///< linear
    f64 variation; ///< albedo variation
    f64 bump;      ///< normal strength
    f64 rough[2];  ///< roughness range
};

constexpr Kind kKinds[] = {
    {"stone", 4, 5, 11u, {0.32, 0.29, 0.25}, 0.55, 3.0, {0.55, 0.9}},
    {"moss", 8, 4, 23u, {0.10, 0.22, 0.05}, 0.5, 2.0, {0.75, 0.95}},
    {"snow", 4, 3, 37u, {0.82, 0.84, 0.88}, 0.06, 0.8, {0.3, 0.6}},
    {"grain", 16, 3, 41u, {0.5, 0.5, 0.5}, 0.35, 4.0, {0.5, 0.5}},
    {"checker", 8, 1, 0u, {0.5, 0.5, 0.5}, 0.8, 0.0, {0.5, 0.5}},
};

const Kind* findKind(std::string_view id, bool& normal) {
    for (const Kind& k : kKinds) {
        const std::string a = std::string(k.name) + "_albedo";
        const std::string n = std::string(k.name) + "_normal";
        if (id == a) {
            normal = false;
            return &k;
        }
        if (id == n) {
            normal = true;
            return &k;
        }
    }
    return nullptr;
}

f64 kindHeight(const Kind& k, f64 u, f64 v) {
    if (std::strcmp(k.name, "checker") == 0) {
        const i32 cx = static_cast<i32>(std::floor(u * 8.0)), cy = static_cast<i32>(std::floor(v * 8.0));
        return ((cx + cy) & 1) != 0 ? 1.0 : 0.0;
    }
    const f64 h = fbm(u, v, k.base, k.octaves, k.seed);
    if (std::strcmp(k.name, "stone") == 0) {
        // Stone: blobs (cells) on top of the fBm, so repetition is visible.
        const f64 cells = fbm(u, v, 3, 1, k.seed + 7u);
        return std::clamp((h - 0.5) * 1.8 + (cells > 0.55 ? 0.25 : -0.1) + 0.5, 0.0, 1.0);
    }
    return h;
}

// --- PNG ------------------------------------------------------------------------------------------------------------
u32 crc32(const u8* data, usize n, u32 crc = 0xFFFFFFFFu) {
    for (usize i = 0; i < n; ++i) {
        crc ^= data[i];
        for (u32 b = 0; b < 8u; ++b) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return crc;
}

void be32(std::vector<u8>& o, u32 v) {
    o.push_back(static_cast<u8>(v >> 24));
    o.push_back(static_cast<u8>(v >> 16));
    o.push_back(static_cast<u8>(v >> 8));
    o.push_back(static_cast<u8>(v));
}

u32 rd32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

void chunk(std::vector<u8>& o, const char* type, const std::vector<u8>& data) {
    be32(o, static_cast<u32>(data.size()));
    const usize start = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    be32(o, crc32(o.data() + start, o.size() - start) ^ 0xFFFFFFFFu);
}

} // namespace

void ml_build_lut(f32 (&lut)[kMlLutEntries]) {
    for (u32 i = 0; i < 256u; ++i) {
        lut[i] = static_cast<f32>(srgbToLinear(static_cast<f64>(i) / 255.0));
        lut[256u + i] = static_cast<f32>(static_cast<f64>(i) / 255.0);
    }
}

MlLibrary::MlLibrary() { ml_build_lut(m_lut); }

u32 MlLibrary::addTexture(std::string_view id, u32 width, u32 height, u32 flags, const std::vector<u32>& texels) {
    MlTexture t{};
    t.offset = static_cast<u32>(m_texels.size());
    t.width = width;
    t.height = height;
    t.flags = flags;
    m_texels.insert(m_texels.end(), texels.begin(), texels.begin() + static_cast<std::ptrdiff_t>(width * height));
    f64 sum[4] = {};
    const MlView v = view();
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const MlF4 c = ml_texel(v, t, x, y);
            sum[0] += c.x;
            sum[1] += c.y;
            sum[2] += c.z;
            sum[3] += c.w;
        }
    }
    for (u32 c = 0; c < 4u; ++c) {
        t.mean[c] = static_cast<f32>(sum[c] / static_cast<f64>(width * height));
    }
    m_textures.push_back(t);
    m_textureIds.emplace_back(id);
    return static_cast<u32>(m_textures.size() - 1u);
}

u32 MlLibrary::findTexture(std::string_view id) const {
    for (usize i = 0; i < m_textureIds.size(); ++i) {
        if (m_textureIds[i] == id) {
            return static_cast<u32>(i);
        }
    }
    return kMlNoTexture;
}

bool MlLibrary::addBuiltinTexture(std::string_view id, u32 size) {
    if (findTexture(id) != kMlNoTexture) {
        return true;
    }
    std::vector<u32> texels;
    u32 flags = 0;
    if (!ml_make_builtin_texture(id, size, texels, flags)) {
        return false;
    }
    addTexture(id, size, size, flags, texels);
    return true;
}

FuseMatResult MlLibrary::addMaterial(const FuseMat& m, u32* index) {
    for (const std::string* id : {&m.textures.albedo, &m.textures.normal, &m.detail.albedo, &m.detail.normal}) {
        if (!id->empty()) {
            addBuiltinTexture(*id);
        }
    }
    for (const FuseMatLayer& l : m.layers) {
        for (const std::string* id : {&l.textures.albedo, &l.textures.normal}) {
            if (!id->empty()) {
                addBuiltinTexture(*id);
            }
        }
    }
    MlMaterial g{};
    FuseMatResult r = resolve_fusemat(m, [this](std::string_view id) { return findTexture(id); }, g);
    if (r.ok) {
        const u32 i = addMaterial(g);
        if (index != nullptr) {
            *index = i;
        }
    }
    return r;
}

u32 MlLibrary::addMaterial(const MlMaterial& m) {
    m_materials.push_back(m);
    return static_cast<u32>(m_materials.size() - 1u);
}

MlView MlLibrary::view() const {
    MlView v{};
    v.materials = m_materials.data();
    v.materialCount = static_cast<u32>(m_materials.size());
    v.textures = m_textures.data();
    v.textureCount = static_cast<u32>(m_textures.size());
    v.texels = m_texels.data();
    v.lut = m_lut;
    return v;
}

const std::vector<std::string>& ml_builtin_texture_ids() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (const Kind& k : kKinds) {
            v.push_back(std::string(k.name) + "_albedo");
            v.push_back(std::string(k.name) + "_normal");
        }
        return v;
    }();
    return ids;
}

bool ml_make_builtin_texture(std::string_view id, u32 size, std::vector<u32>& texels, u32& flags) {
    bool normal = false;
    const Kind* k = findKind(id, normal);
    if (k == nullptr || size == 0u) {
        return false;
    }
    const bool detail = std::strcmp(k->name, "grain") == 0 || std::strcmp(k->name, "checker") == 0;
    flags = (!normal && !detail) ? static_cast<u32>(kMlTexSrgb) : 0u;
    texels.assign(static_cast<usize>(size) * size, 0u);
    const f64 inv = 1.0 / size;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f64 u = (x + 0.5) * inv, v = (y + 0.5) * inv;
            const f64 h = kindHeight(*k, u, v);
            if (!normal) {
                const f64 tint = fbm(u, v, k->base * 2, 2, k->seed + 99u);
                const f64 s = 1.0 + k->variation * (h - 0.5) * 2.0;
                f64 c[3];
                for (u32 i = 0; i < 3u; ++i) {
                    c[i] = std::clamp(k->color[i] * s * (0.9 + 0.2 * tint), 0.0, 1.0);
                    if (flags != 0u) {
                        c[i] = linearToSrgb(c[i]);
                    }
                }
                texels[y * size + x] = pack(toByte(c[0]), toByte(c[1]), toByte(c[2]), toByte(h));
                continue;
            }
            const f64 hx = kindHeight(*k, u + inv, v) - kindHeight(*k, u - inv, v);
            const f64 hy = kindHeight(*k, u, v + inv) - kindHeight(*k, u, v - inv);
            f64 n[3] = {-hx * k->bump, -hy * k->bump, 1.0};
            const f64 l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            const f64 rough = k->rough[0] + (k->rough[1] - k->rough[0]) * (1.0 - h);
            const f64 ao = std::clamp(0.55 + 0.6 * h, 0.0, 1.0);
            texels[y * size + x] = pack(toByte(n[0] / l * 0.5 + 0.5), toByte(n[1] / l * 0.5 + 0.5), toByte(rough), toByte(ao));
        }
    }
    return true;
}

MlBallScene ml_ball_scene(u32 width, u32 height, u32 materialCount) {
    MlBallScene s{};
    MlParams& p = s.params;
    p.width = width;
    p.height = height;
    p.camPos[0] = 0.f;
    p.camPos[1] = 0.f;
    p.camPos[2] = 6.f;
    p.camForward[0] = 0.f;
    p.camForward[1] = 0.f;
    p.camForward[2] = -1.f;
    p.camRight[0] = 1.f;
    p.camRight[1] = 0.f;
    p.camRight[2] = 0.f;
    p.camUp[0] = 0.f;
    p.camUp[1] = 1.f;
    p.camUp[2] = 0.f;
    p.tanHalfY = 0.34f;
    p.aspect = static_cast<f32>(width) / static_cast<f32>(height);
    const f64 sl = std::sqrt(0.5 * 0.5 + 0.7 * 0.7 + 0.6 * 0.6);
    p.sunDir[0] = static_cast<f32>(0.5 / sl);
    p.sunDir[1] = static_cast<f32>(0.7 / sl);
    p.sunDir[2] = static_cast<f32>(0.6 / sl);
    p.sunIntensity = 3.0f;
    p.ambient = 0.6f;
    p.exposure = 1.0f;
    p.background[0] = 0.05f;
    p.background[1] = 0.05f;
    p.background[2] = 0.06f;
    p.background[3] = 0.f;
    const u32 cols = 4u;
    for (u32 i = 0; i < materialCount && i < kMlMaxBalls; ++i) {
        MlBall b{};
        const u32 cx = i % cols, cy = i / cols;
        b.center[0] = -2.7f + 1.8f * static_cast<f32>(cx);
        b.center[1] = 0.9f - 1.8f * static_cast<f32>(cy);
        b.center[2] = 0.f;
        b.radius = 0.8f;
        b.material = i;
        s.balls.push_back(b);
    }
    p.ballCount = static_cast<u32>(s.balls.size());
    return s;
}

void ml_render_balls(const MlParams& params, const MlView& view, const MlBall* balls, std::vector<MlF4>& out) {
    out.assign(static_cast<usize>(params.width) * params.height, MlF4{});
    for (u32 y = 0; y < params.height; ++y) {
        for (u32 x = 0; x < params.width; ++x) {
            out[static_cast<usize>(y) * params.width + x] = ml_ball_pixel(params, view, balls, x, y);
        }
    }
}

void ml_to_srgb8(const std::vector<MlF4>& image, std::vector<u8>& rgba) {
    rgba.resize(image.size() * 4u);
    for (usize i = 0; i < image.size(); ++i) {
        rgba[i * 4u + 0u] = toByte(linearToSrgb(std::clamp<f64>(image[i].x, 0.0, 1.0)));
        rgba[i * 4u + 1u] = toByte(linearToSrgb(std::clamp<f64>(image[i].y, 0.0, 1.0)));
        rgba[i * 4u + 2u] = toByte(linearToSrgb(std::clamp<f64>(image[i].z, 0.0, 1.0)));
        rgba[i * 4u + 3u] = 255u; // opaque PNG (the background alpha 0 marks misses in the f32 image only)
    }
}

std::vector<u8> ml_png_encode(u32 width, u32 height, const std::vector<u8>& rgba) {
    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    be32(ihdr, width);
    be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8u, 6u, 0u, 0u, 0u}); // 8 bit, RGBA, deflate, filter 0, no interlace
    chunk(png, "IHDR", ihdr);
    std::vector<u8> raw;
    raw.reserve(static_cast<usize>(height) * (width * 4u + 1u));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0u);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y) * width * 4,
                   rgba.begin() + static_cast<std::ptrdiff_t>(y + 1u) * width * 4);
    }
    std::vector<u8> z = {0x78, 0x01};
    usize pos = 0;
    do {
        const usize n = std::min<usize>(65535u, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1u : 0u);
        z.push_back(static_cast<u8>(n));
        z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n));
        z.push_back(static_cast<u8>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    u32 a = 1u, b = 0u;
    for (const u8 c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    be32(z, (b << 16) | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

bool ml_png_decode(const std::vector<u8>& png, u32& width, u32& height, std::vector<u8>& rgba) {
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8u || std::memcmp(png.data(), sig, 8u) != 0) {
        return false;
    }
    usize pos = 8u;
    std::vector<u8> z;
    width = height = 0;
    while (pos + 12u <= png.size()) {
        const u32 len = rd32(png.data() + pos);
        if (pos + 12u + len > png.size()) {
            return false;
        }
        const u8* type = png.data() + pos + 4u;
        const u8* data = type + 4u;
        if ((crc32(type, len + 4u) ^ 0xFFFFFFFFu) != rd32(data + len)) {
            return false;
        }
        if (std::memcmp(type, "IHDR", 4u) == 0) {
            if (len != 13u || data[8] != 8u || data[9] != 6u || data[12] != 0u) {
                return false;
            }
            width = rd32(data);
            height = rd32(data + 4u);
        } else if (std::memcmp(type, "IDAT", 4u) == 0) {
            z.insert(z.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4u) == 0) {
            break;
        }
        pos += 12u + len;
    }
    if (width == 0u || height == 0u || z.size() < 6u) {
        return false;
    }
    std::vector<u8> raw;
    usize p = 2u;
    for (;;) {
        if (p + 5u > z.size()) {
            return false;
        }
        const u8 header = z[p];
        if ((header & 6u) != 0u) {
            return false; // compressed block: not one of ours
        }
        const usize n = static_cast<usize>(z[p + 1]) | (static_cast<usize>(z[p + 2]) << 8);
        p += 5u;
        if (p + n > z.size()) {
            return false;
        }
        raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(p), z.begin() + static_cast<std::ptrdiff_t>(p + n));
        p += n;
        if ((header & 1u) != 0u) {
            break;
        }
    }
    const usize stride = static_cast<usize>(width) * 4u + 1u;
    if (raw.size() != stride * height) {
        return false;
    }
    rgba.resize(static_cast<usize>(width) * height * 4u);
    for (u32 y = 0; y < height; ++y) {
        if (raw[y * stride] != 0u) {
            return false;
        }
        std::memcpy(rgba.data() + static_cast<usize>(y) * width * 4u, raw.data() + y * stride + 1u, width * 4u);
    }
    return true;
}

f64 ml_autocorrelation(const std::vector<f32>& image, u32 w, u32 h, u32 lag) {
    f64 mean = 0.0;
    for (const f32 v : image) {
        mean += v;
    }
    mean /= static_cast<f64>(image.size());
    f64 var = 0.0;
    for (const f32 v : image) {
        var += (v - mean) * (v - mean);
    }
    var /= static_cast<f64>(image.size());
    if (var <= 0.0 || lag >= w || lag >= h) {
        return 1.0;
    }
    f64 sx = 0.0, sy = 0.0;
    usize nx = 0, ny = 0;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f64 a = image[static_cast<usize>(y) * w + x] - mean;
            if (x + lag < w) {
                sx += a * (image[static_cast<usize>(y) * w + x + lag] - mean);
                ++nx;
            }
            if (y + lag < h) {
                sy += a * (image[static_cast<usize>(y + lag) * w + x] - mean);
                ++ny;
            }
        }
    }
    return 0.5 * (sx / (static_cast<f64>(nx) * var) + sy / (static_cast<f64>(ny) * var));
}

} // namespace fuse::renderer::material_layers
