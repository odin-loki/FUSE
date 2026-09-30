// UNI-U7-ASSET-1: dependency-free copy of the renderer's `.fusemat` binary reader + validation
// (Source/FUSE/Renderer/src/material_layers/fusemat.cpp, read_fusemat_binary / validate_fusemat).
// Keep the two in step: fuse_asset_runtime_material compares them field for field.

#include <fuse/asset/cooked_material.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fuse::asset {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

u64 fnv1a64(const u8* data, usize size) {
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

class Cursor {
public:
    Cursor(const u8* data, usize size) : m_data(data), m_size(size) {}
    bool ok() const { return m_ok; }
    bool done() const { return m_pos == m_size; }
    u32 u32v() {
        if (!need(4u)) {
            return 0;
        }
        u32 v = 0;
        for (u32 i = 0; i < 4u; ++i) {
            v |= static_cast<u32>(m_data[m_pos + i]) << (8u * i);
        }
        m_pos += 4u;
        return v;
    }
    f32 f32v() {
        const u32 bits = u32v();
        f32 v = 0.f;
        std::memcpy(&v, &bits, 4u);
        return v;
    }
    std::string str() {
        if (!need(2u)) {
            return {};
        }
        const usize n = static_cast<usize>(m_data[m_pos]) | (static_cast<usize>(m_data[m_pos + 1]) << 8);
        m_pos += 2u;
        if (!need(n)) {
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), n);
        m_pos += n;
        return s;
    }

private:
    bool need(usize n) {
        if (!m_ok || m_size - m_pos < n) {
            m_ok = false;
            return false;
        }
        return true;
    }
    const u8* m_data;
    usize m_size;
    usize m_pos = 0;
    bool m_ok = true;
};

CookedMaterialTextures read_set(Cursor& c) {
    CookedMaterialTextures s;
    s.albedo = c.str();
    s.normal = c.str();
    return s;
}

/// Collects the first failure only (the renderer's validator lists all; the runtime needs a reason).
struct Checker {
    std::string first;
    void fail(const std::string& path, const std::string& message) {
        if (first.empty()) {
            first = path + ": " + message;
        }
    }
    void range(const std::string& path, f32 v, f32 lo, f32 hi) {
        if (!(v >= lo && v <= hi)) {
            fail(path, "out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
        }
    }
    void positive(const std::string& path, f32 v, f32 hi) {
        if (!(v > 0.f && v <= hi)) {
            fail(path, "must be in (0, " + std::to_string(hi) + "]");
        }
    }
    void albedo(const std::string& path, const f32* a, f32 metallic, bool textured) {
        for (u32 c = 0; c < 3u; ++c) {
            const std::string p = path + "[" + std::to_string(c) + "]";
            if (textured) {
                range(p, a[c], 0.f, 1.f);
            } else if (metallic >= 0.5f) {
                range(p, a[c], 0.45f, 1.f);
            } else {
                range(p, a[c], 0.02f, 0.9f);
            }
        }
    }
    void id(const std::string& path, const std::string& value) {
        if (value.size() > 255u) {
            fail(path, "texture id longer than 255 characters");
        }
    }
};

} // namespace

bool validate_cooked_material(const CookedMaterial& m, std::string* error) {
    Checker r;
    if (m.version != kCookedMaterialVersion) {
        r.fail("fusemat", "unsupported version " + std::to_string(m.version));
    }
    if (m.name.empty() || m.name.size() > 255u) {
        r.fail("name", "must be 1-255 characters");
    }
    if (m.shading >= kMaterialShadingCount) {
        r.fail("shading_model", "unknown");
    }
    if (m.category >= kMaterialCategoryCount) {
        r.fail("category", "unknown");
    }
    if (m.wind >= kMaterialWindCount) {
        r.fail("wind", "unknown");
    }
    r.range("base.metallic", m.metallic, 0.f, 1.f);
    r.range("base.roughness", m.roughness, 0.f, 1.f);
    r.albedo("base.albedo", m.albedo, m.metallic, !m.textures.albedo.empty());
    r.range("base.normal_strength", m.normal_strength, 0.f, 4.f);
    r.id("base.textures.albedo", m.textures.albedo);
    r.id("base.textures.normal", m.textures.normal);
    r.positive("uv_scale", m.uv_scale, 1024.f);
    r.range("triplanar.sharpness", m.triplanar_sharpness, 1.f, 16.f);
    r.positive("stochastic.lattice", m.stochastic_lattice, 16.f);
    r.positive("macro.scale", m.macro_scale, 1024.f);
    r.range("macro.strength", m.macro_strength, 0.f, 1.f);
    r.id("detail.albedo", m.detail.albedo);
    r.id("detail.normal", m.detail.normal);
    r.range("detail.scale", m.detail_scale, 1.f, 32.f);
    r.range("detail.strength", m.detail_strength, 0.f, 2.f);
    if (!(m.detail_fade[0] >= 0.f && m.detail_fade[1] > m.detail_fade[0])) {
        r.fail("detail.fade", "needs 0 <= start < end");
    }
    if (m.layers.size() > kCookedMaterialMaxLayers) {
        r.fail("layers", "at most " + std::to_string(kCookedMaterialMaxLayers) + " layers");
    }
    for (usize i = 0; i < m.layers.size(); ++i) {
        const CookedMaterialLayer& l = m.layers[i];
        const std::string p = "layers[" + std::to_string(i) + "]";
        if (l.mode >= kMaterialLayerModeCount) {
            r.fail(p + ".mode", "unknown");
        }
        if (l.mask >= kMaterialMaskCount) {
            r.fail(p + ".mask", "unknown");
        }
        r.range(p + ".coverage", l.coverage, 0.f, 1.f);
        r.range(p + ".contrast", l.contrast, 1.f, 64.f);
        r.range(p + ".roughness", l.roughness, 0.f, 1.f);
        r.range(p + ".metallic", l.metallic, 0.f, 1.f);
        r.positive(p + ".uv_scale", l.uv_scale, 1024.f);
        r.range(p + ".normal_strength", l.normal_strength, 0.f, 4.f);
        r.range(p + ".mask_scale", l.mask_scale, -1024.f, 1024.f);
        r.range(p + ".mask_bias", l.mask_bias, -1.0e6f, 1.0e6f);
        if (l.mode == kMaterialLayerModeWet) {
            for (u32 c = 0; c < 3u; ++c) {
                const f32 a = l.albedo[c];
                if (!(a > 0.f && a <= 1.f)) {
                    r.fail(p + ".albedo[" + std::to_string(c) + "]", "wet darkening factor must be in (0, 1]");
                }
            }
        } else {
            r.albedo(p + ".albedo", l.albedo, l.metallic, !l.textures.albedo.empty());
        }
        r.id(p + ".textures.albedo", l.textures.albedo);
        r.id(p + ".textures.normal", l.textures.normal);
    }
    if (m.procedural_params.size() > kCookedMaterialMaxProceduralParams) {
        r.fail("procedural.params", "at most 8 parameters");
    }
    if (!r.first.empty()) {
        set_error(error, "cooked material invalid: " + r.first);
        return false;
    }
    return true;
}

bool read_cooked_material(const u8* data, usize size, CookedMaterial& out, std::string* error) {
    out = CookedMaterial{};
    auto reject = [&](const std::string& message) {
        set_error(error, "cooked material " + message);
        out = CookedMaterial{};
        return false;
    };
    if (data == nullptr || size < 20u) {
        return reject("truncated");
    }
    Cursor head(data, 12u);
    const u32 magic = head.u32v();
    const u32 version = head.u32v();
    const u32 payload = head.u32v();
    if (magic != kCookedMaterialMagic) {
        return reject("bad magic (not a .fusemat)");
    }
    if (version != kCookedMaterialVersion) {
        return reject("version " + std::to_string(version) + " unsupported");
    }
    if (static_cast<u64>(payload) + 20u != size) {
        return reject("size mismatch (truncated or trailing bytes)");
    }
    u64 trailer = 0;
    for (u32 i = 0; i < 8u; ++i) {
        trailer |= static_cast<u64>(data[size - 8u + i]) << (8u * i);
    }
    if (trailer != fnv1a64(data, size - 8u)) {
        return reject("checksum mismatch (FNV-1a trailer)");
    }
    Cursor c(data + 12u, payload);
    out.version = version;
    out.name = c.str();
    out.shading = c.u32v();
    out.category = c.u32v();
    out.wind = c.u32v();
    for (f32& a : out.albedo) {
        a = c.f32v();
    }
    out.roughness = c.f32v();
    out.metallic = c.f32v();
    out.normal_strength = c.f32v();
    out.textures = read_set(c);
    out.uv_scale = c.f32v();
    out.triplanar = c.u32v() != 0u;
    out.triplanar_sharpness = c.f32v();
    out.stochastic = c.u32v() != 0u;
    out.stochastic_lattice = c.f32v();
    out.macro_scale = c.f32v();
    out.macro_strength = c.f32v();
    out.detail = read_set(c);
    out.detail_scale = c.f32v();
    out.detail_strength = c.f32v();
    out.detail_fade[0] = c.f32v();
    out.detail_fade[1] = c.f32v();
    const u32 layers = c.u32v();
    if (!c.ok() || layers > kCookedMaterialMaxLayers) {
        return reject("bad layer count");
    }
    for (u32 i = 0; i < layers; ++i) {
        CookedMaterialLayer l{};
        l.name = c.str();
        l.mode = c.u32v();
        l.mask = c.u32v();
        l.mask_bias = c.f32v();
        l.mask_scale = c.f32v();
        l.coverage = c.f32v();
        l.contrast = c.f32v();
        for (f32& a : l.albedo) {
            a = c.f32v();
        }
        l.roughness = c.f32v();
        l.metallic = c.f32v();
        l.uv_scale = c.f32v();
        l.normal_strength = c.f32v();
        l.textures = read_set(c);
        out.layers.push_back(std::move(l));
    }
    out.procedural_function = c.u32v();
    const u32 params = c.u32v();
    if (!c.ok() || params > kCookedMaterialMaxProceduralParams) {
        return reject("bad procedural parameter count");
    }
    for (u32 i = 0; i < params; ++i) {
        out.procedural_params.push_back(c.f32v());
    }
    if (!c.ok() || !c.done()) {
        return reject("payload size does not match its fields");
    }
    if (!validate_cooked_material(out, error)) {
        out = CookedMaterial{};
        return false;
    }
    return true;
}

bool read_cooked_material_file(const std::string& path, CookedMaterial& out, std::string* error) {
    out = CookedMaterial{};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cooked material unreadable");
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return read_cooked_material(bytes.data(), bytes.size(), out, error);
}

} // namespace fuse::asset
