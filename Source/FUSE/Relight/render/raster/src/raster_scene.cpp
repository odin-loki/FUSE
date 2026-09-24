// FUSE Relight RL-4.2: the raster remaster's CPU frame (see raster_scene.hpp).
#include <fuse/relight/render/raster/raster_scene.hpp>

#include <fuse/relight/replace/mod_content.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>
#include <fuse/renderer/lighting/clustered.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_set>

namespace fuse::relight::render::raster {

namespace gs = fuse::renderer::gpu_scene;
namespace geo = fuse::relight::capture::geometry;
using scene::InstanceCategories;

static_assert(offsetof(RasterVertex, normal) == 16 && offsetof(RasterVertex, uv) == 32, "RasterVertex layout");
static_assert(offsetof(RasterMaterial, texture) == 64 && offsetof(RasterMaterial, metallic) == 84,
              "RasterMaterial layout");
static_assert(offsetof(RasterDrawGpu, material) == 192, "RasterDrawGpu layout");
static_assert(offsetof(RasterFrameGpu, shadowMatrix) == 160 && offsetof(RasterFrameGpu, counts) == 240 &&
                  offsetof(RasterFrameGpu, grid) == 288 && offsetof(RasterFrameGpu, lut) == 312,
              "RasterFrameGpu layout");

namespace {

constexpr std::uint32_t kTopologyTriangleList = 3, kTopologyTriangleStrip = 4, kTopologyTriangleFan = 5;
constexpr std::uint32_t kInvalid = 0xffffffffu;

const char* const kFeatureNames[kFeatureCount] = {"gbuffer",     "clustered",      "forward",        "decals",
                                                  "fog",         "shadows",        "meshlet_path",   "rt_shadows",
                                                  "rt_reflections", "ddgi"};

template <typename T>
T load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
    float v;
    if (exp == 0) {
        v = std::ldexp(static_cast<float>(man), -24);
    } else if (exp == 31) {
        v = man ? NAN : INFINITY;
    } else {
        v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
    }
    return sign ? -v : v;
}

/// One element of a vertex attribute as float4 (D3D decoding; missing components 0, w 1).
std::array<float, 4> readAttribute(const geo::VertexAttribute& a, std::uint32_t vertex) {
    std::array<float, 4> v{0.f, 0.f, 0.f, 1.f};
    if (!a.defined()) {
        return v;
    }
    const std::uint8_t* p = a.base() + static_cast<std::size_t>(vertex) * a.stride;
    using T = hash::D3DDeclType;
    auto snorm = [](float x, float m) { return std::max(x / m, -1.f); };
    switch (a.type) {
    case T::Float4:
        v[3] = load<float>(p + 12);
        [[fallthrough]];
    case T::Float3:
        v[2] = load<float>(p + 8);
        [[fallthrough]];
    case T::Float2:
        v[1] = load<float>(p + 4);
        [[fallthrough]];
    case T::Float1:
        v[0] = load<float>(p);
        break;
    case T::D3DColor:
        v = {p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f};
        break;
    case T::UByte4:
        v = {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
        break;
    case T::UByte4N:
        v = {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f};
        break;
    case T::Short2:
        v = {float(load<std::int16_t>(p)), float(load<std::int16_t>(p + 2)), 0.f, 1.f};
        break;
    case T::Short4:
        v = {float(load<std::int16_t>(p)), float(load<std::int16_t>(p + 2)), float(load<std::int16_t>(p + 4)),
             float(load<std::int16_t>(p + 6))};
        break;
    case T::Short2N:
        v = {snorm(load<std::int16_t>(p), 32767.f), snorm(load<std::int16_t>(p + 2), 32767.f), 0.f, 1.f};
        break;
    case T::Short4N:
        v = {snorm(load<std::int16_t>(p), 32767.f), snorm(load<std::int16_t>(p + 2), 32767.f),
             snorm(load<std::int16_t>(p + 4), 32767.f), snorm(load<std::int16_t>(p + 6), 32767.f)};
        break;
    case T::UShort2N:
        v = {load<std::uint16_t>(p) / 65535.f, load<std::uint16_t>(p + 2) / 65535.f, 0.f, 1.f};
        break;
    case T::UShort4N:
        v = {load<std::uint16_t>(p) / 65535.f, load<std::uint16_t>(p + 2) / 65535.f,
             load<std::uint16_t>(p + 4) / 65535.f, load<std::uint16_t>(p + 6) / 65535.f};
        break;
    case T::UDec3: {
        const std::uint32_t u = load<std::uint32_t>(p);
        v = {float(u & 1023u), float((u >> 10) & 1023u), float((u >> 20) & 1023u), 1.f};
        break;
    }
    case T::Dec3N: {
        const std::uint32_t u = load<std::uint32_t>(p);
        auto s10 = [&](std::uint32_t x) {
            return snorm(static_cast<float>(static_cast<std::int32_t>(x << 22) >> 22), 511.f);
        };
        v = {s10(u & 1023u), s10((u >> 10) & 1023u), s10((u >> 20) & 1023u), 1.f};
        break;
    }
    case T::Float16_2:
        v = {halfToFloat(load<std::uint16_t>(p)), halfToFloat(load<std::uint16_t>(p + 2)), 0.f, 1.f};
        break;
    case T::Float16_4:
        v = {halfToFloat(load<std::uint16_t>(p)), halfToFloat(load<std::uint16_t>(p + 2)),
             halfToFloat(load<std::uint16_t>(p + 4)), halfToFloat(load<std::uint16_t>(p + 6))};
        break;
    case T::Unused:
        break;
    }
    return v;
}

std::uint32_t packRgba8(const std::array<float, 4>& c) {
    std::uint32_t out = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const float f = std::clamp(c[i], 0.f, 1.f) * 255.f + 0.5f;
        out |= (static_cast<std::uint32_t>(f) & 0xffu) << (8u * i);
    }
    return out;
}

bool isIdentity(const scene::Mat4f& m) {
    for (std::size_t i = 0; i < 16; ++i) {
        if (m[i] != ((i % 5 == 0) ? 1.f : 0.f)) {
            return false;
        }
    }
    return true;
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void normalize3(float* v) {
    const float l = std::sqrt(dot3(v, v));
    if (l > 0.f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void cross3(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// Camera from a D3D view (row-vector, orthonormal rotation) and projection.
RasterCamera cameraOf(const scene::DrawTransforms& t) {
    RasterCamera c;
    const float* w = t.worldToView.data();
    const float* p = t.viewToProjection.data();
    auto r = [&](int row, int col) { return w[row * 4 + col]; };
    for (int i = 0; i < 3; ++i) {
        c.forward[i] = r(i, 2);
        c.up[i] = r(i, 1);
        c.eye[i] = -(r(3, 0) * r(i, 0) + r(3, 1) * r(i, 1) + r(3, 2) * r(i, 2));
    }
    normalize3(c.forward);
    normalize3(c.up);
    // LH perspective: P[2][3] = 1, P[3][3] = 0.
    c.perspective = std::fabs(p[11] - 1.f) < 1e-4f && std::fabs(p[15]) < 1e-6f && p[5] != 0.f && p[0] != 0.f;
    if (c.perspective) {
        const float a = p[10], b = p[14];
        c.nearZ = a != 0.f ? -b / a : 0.1f;
        c.farZ = (1.f - a) != 0.f ? b / (1.f - a) : 1000.f;
        if (!(c.nearZ > 0.f) || !(c.farZ > c.nearZ) || !std::isfinite(c.farZ)) {
            c.nearZ = 0.1f;
            c.farZ = 1000.f;
        }
        c.fovY = 2.f * std::atan(1.f / p[5]);
        c.aspect = p[5] / p[0];
    }
    c.valid = true;
    return c;
}

/// A row-major D3D look-along view matrix (LH: +z forward).
void lookAlong(const float* eye, const float* dir, float* out) {
    float z[3] = {dir[0], dir[1], dir[2]};
    normalize3(z);
    float upHint[3] = {0.f, 1.f, 0.f};
    if (std::fabs(z[1]) > 0.99f) {
        upHint[0] = 1.f;
        upHint[1] = 0.f;
    }
    float x[3], y[3];
    cross3(upHint, z, x); // LH: right = up x forward
    normalize3(x);
    cross3(z, x, y);
    const float m[16] = {x[0], y[0], z[0], 0.f, x[1], y[1], z[1], 0.f, x[2], y[2], z[2], 0.f,
                         -dot3(x, eye), -dot3(y, eye), -dot3(z, eye), 1.f};
    std::memcpy(out, m, sizeof(m));
}

void transformPoint(const float* m, const float* p, float* out) {
    for (int c = 0; c < 3; ++c) {
        out[c] = p[0] * m[0 * 4 + c] + p[1] * m[1 * 4 + c] + p[2] * m[2 * 4 + c] + m[3 * 4 + c];
    }
}

bool isDecal(const scene::CategoryFlags& c) {
    return c.test(InstanceCategories::DecalStatic) || c.test(InstanceCategories::DecalDynamic) ||
           c.test(InstanceCategories::DecalSingleOffset) || c.test(InstanceCategories::DecalNoOffset);
}

} // namespace

// ---- tiers ---------------------------------------------------------------------------------------------------------

const char* featureName(std::uint32_t bit) {
    for (std::uint32_t i = 0; i < kFeatureCount; ++i) {
        if (bit == (1u << i)) {
            return kFeatureNames[i];
        }
    }
    return "?";
}

std::string featureList(std::uint32_t mask) {
    std::string s;
    for (std::uint32_t i = 0; i < kFeatureCount; ++i) {
        if (mask & (1u << i)) {
            s += s.empty() ? "" : ",";
            s += kFeatureNames[i];
        }
    }
    return s;
}

std::uint32_t tierFeatures(std::uint32_t tier) {
    std::uint32_t m = kFeatureGBuffer | kFeatureClustered | kFeatureForward | kFeatureDecals | kFeatureFog;
    if (tier >= 1) {
        m |= kFeatureShadows | kFeatureMeshletPath;
    }
    if (tier >= 2) {
        m |= kFeatureRtShadows | kFeatureRtReflections | kFeatureDdgi;
    }
    return m;
}

std::uint32_t implementedFeatures() {
    return kFeatureGBuffer | kFeatureClustered | kFeatureForward | kFeatureDecals | kFeatureFog | kFeatureShadows;
}

TierPlan planTier(std::uint32_t deviceTier, int optionTier, std::uint32_t available) {
    TierPlan p;
    p.deviceTier = deviceTier;
    p.optionTier = optionTier;
    p.tier = std::min<std::uint32_t>(deviceTier, 2u);
    if (optionTier >= 0) {
        p.tier = std::min(p.tier, static_cast<std::uint32_t>(optionTier));
    }
    p.requested = tierFeatures(p.tier);
    // The minimum (G-buffer + clustered) is always kept: without it nothing renders.
    p.enabled = (p.requested & available) | kFeatureGBuffer | kFeatureClustered;
    p.degraded = featureList(p.requested & ~p.enabled);
    return p;
}

const char* bucketName(Bucket b) {
    switch (b) {
    case Bucket::Opaque:
        return "opaque";
    case Bucket::Decal:
        return "decal";
    case Bucket::Blend:
        return "blend";
    case Bucket::Skipped:
        break;
    }
    return "skipped";
}

// ---- materials -----------------------------------------------------------------------------------------------------

float roughnessFromPower(float power) {
    const float alpha = std::sqrt(2.f / (std::max(power, 0.f) + 2.f));
    return std::clamp(std::sqrt(alpha), 0.05f, 1.f);
}

RasterMaterial legacyMaterial(const scene::LegacyMaterialRecord& m, bool hasColor0, bool sky) {
    RasterMaterial r;
    const tap::Material& d = m.d3dMaterial;
    const bool neverSet = d.diffuse.r == 0.f && d.diffuse.g == 0.f && d.diffuse.b == 0.f && d.diffuse.a == 0.f &&
                          d.ambient.r == 0.f && d.ambient.g == 0.f && d.ambient.b == 0.f && d.emissive.r == 0.f &&
                          d.emissive.g == 0.f && d.emissive.b == 0.f && d.specular.r == 0.f && d.specular.g == 0.f &&
                          d.specular.b == 0.f;
    if (neverSet) {
        // D3D's default material (all zero) with a material colour source: an unlit draw's diffuse is opaque white.
        r.diffuse[0] = r.diffuse[1] = r.diffuse[2] = r.diffuse[3] = 1.f;
    } else {
        r.diffuse[0] = d.diffuse.r;
        r.diffuse[1] = d.diffuse.g;
        r.diffuse[2] = d.diffuse.b;
        r.diffuse[3] = d.diffuse.a;
        r.emissive[0] = d.emissive.r;
        r.emissive[1] = d.emissive.g;
        r.emissive[2] = d.emissive.b;
        r.specular[0] = d.specular.r;
        r.specular[1] = d.specular.g;
        r.specular[2] = d.specular.b;
    }
    const std::array<float, 4> tf = scene::decodeD3DColor(m.tFactor);
    std::copy(tf.begin(), tf.end(), r.tfactor);
    r.ops = packOps(static_cast<std::uint32_t>(m.textureColorOperation),
                    static_cast<std::uint32_t>(m.textureColorArg1Source),
                    static_cast<std::uint32_t>(m.textureColorArg2Source),
                    static_cast<std::uint32_t>(m.textureAlphaOperation),
                    static_cast<std::uint32_t>(m.textureAlphaArg1Source),
                    static_cast<std::uint32_t>(m.textureAlphaArg2Source));
    if (m.diffuseColorSource == scene::TextureArgSource::VertexColor0 && hasColor0) {
        r.flags |= kMatVertexColor;
    }
    if (m.alphaTestEnabled) {
        r.flags |= kMatAlphaTest;
        r.alphaTest = m.alphaTestCompareOp | static_cast<std::uint32_t>(m.alphaTestReferenceValue) << 8;
    }
    if (sky) {
        r.flags |= kMatUnlit;
    }
    r.roughness = roughnessFromPower(d.power);
    r.metallic = 0.f;
    return r;
}

// ---- matrices ------------------------------------------------------------------------------------------------------

void multiplyRowMajor(const float* a, const float* b, float* out) {
    float t[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) {
                s += a[r * 4 + k] * b[k * 4 + c];
            }
            t[r * 4 + c] = s;
        }
    }
    std::memcpy(out, t, sizeof(t));
}

bool normalMatrix(const float* m, float* out) {
    // Upper 3x3 A (row-vector convention: n_world = n A^-T ... as a row vector n (A^-1)^T).
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    std::memset(out, 0, 16 * sizeof(float));
    out[15] = 1.f;
    if (!(std::fabs(det) > 1e-20f)) {
        out[0] = out[5] = out[10] = 1.f;
        return false;
    }
    const float inv = 1.f / det;
    // Cofactor matrix / det = (A^-1)^T.
    out[0] = (e * i - f * h) * inv;
    out[1] = -(d * i - f * g) * inv;
    out[2] = (d * h - e * g) * inv;
    out[4] = -(b * i - c * h) * inv;
    out[5] = (a * i - c * g) * inv;
    out[6] = -(a * h - b * g) * inv;
    out[8] = (b * f - c * e) * inv;
    out[9] = -(a * f - c * d) * inv;
    out[10] = (a * e - b * d) * inv;
    return true;
}

// ---- the frame -----------------------------------------------------------------------------------------------------

bool buildRasterFrame(const BuildInputs& in, const BuildOptions& opt, RasterFrame& out) {
    out = RasterFrame{};
    out.frame = in.frame;
    out.width = in.width;
    out.height = in.height;
    out.features = opt.features;
    if (!in.draws || !in.classifications || !in.sceneDraw) {
        return false;
    }
    const std::vector<tap::CaptureDrawRecord>& draws = *in.draws;
    const std::uint32_t count = std::min<std::uint32_t>(in.count, static_cast<std::uint32_t>(draws.size()));

    // Camera: the first Main-camera scene draw, else the first scene draw.
    const tap::CaptureDrawRecord* cameraDraw = nullptr;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!(*in.sceneDraw)[i]) {
            continue;
        }
        if (!cameraDraw) {
            cameraDraw = &draws[i];
        }
        if (draws[i].translation.cameraType == scene::CameraType::Main) {
            cameraDraw = &draws[i];
            break;
        }
    }
    if (cameraDraw) {
        out.camera = cameraOf(cameraDraw->translation.transforms);
    }

    // Fog: the first scene draw with a fog mode.
    scene::FogRecord fog;
    for (std::uint32_t i = 0; i < count && (opt.features & kFeatureFog) && opt.fog; ++i) {
        if ((*in.sceneDraw)[i] && draws[i].translation.fog.mode != scene::d3dff::FOG_NONE) {
            fog = draws[i].translation.fog;
            break;
        }
    }
    out.stats.fogMode = fog.mode;

    std::unordered_set<tap::ResourceId> textures;
    float casterMin[3] = {INFINITY, INFINITY, INFINITY}, casterMax[3] = {-INFINITY, -INFINITY, -INFINITY};
    std::vector<std::uint32_t> tri;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!(*in.sceneDraw)[i]) {
            continue;
        }
        const tap::CaptureDrawRecord& r = draws[i];
        const scene::DrawClassification& cls = (*in.classifications)[i];
        ++out.stats.draws;
        RasterDraw rd;
        rd.source = i;
        auto skip = [&](const char* why) {
            rd.bucket = Bucket::Skipped;
            rd.skipReason = why;
            ++out.stats.skipped;
            out.draws.push_back(rd);
        };
        const geo::CapturedDraw* g = r.geometry.get();
        if (!g || !g->captured() || !g->vertices.position.defined()) {
            skip("no_geometry");
            continue;
        }
        if (g->vertices.hasPositionT) {
            skip("pretransformed");
            continue;
        }
        if (g->programmableVs) {
            skip("programmable_vs");
            continue;
        }
        if (cls.categories.test(InstanceCategories::Hidden) || cls.categories.test(InstanceCategories::Ignore)) {
            skip("hidden");
            continue;
        }
        if (g->topology != kTopologyTriangleList && g->topology != kTopologyTriangleStrip &&
            g->topology != kTopologyTriangleFan) {
            skip("not_triangles");
            continue;
        }
        // Vertex indices of the draw's window, then the triangle list.
        std::vector<std::uint32_t> idx;
        if (g->indices && g->indices->indexCount() > 0) {
            const geo::RebasedIndices& ri = *g->indices;
            idx.resize(ri.indexCount());
            const std::uint8_t* b = ri.bytes().data();
            for (std::uint32_t k = 0; k < ri.indexCount(); ++k) {
                idx[k] = ri.indexSize() == 2 ? load<std::uint16_t>(b + 2u * k) : load<std::uint32_t>(b + 4u * k);
            }
        } else {
            idx.resize(g->vertexCount);
            for (std::uint32_t k = 0; k < g->vertexCount; ++k) {
                idx[k] = k;
            }
        }
        tri.clear();
        if (g->topology == kTopologyTriangleList) {
            tri.assign(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(idx.size() / 3 * 3));
        } else if (g->topology == kTopologyTriangleStrip) {
            for (std::size_t k = 2; k < idx.size(); ++k) {
                // D3D strips alternate the winding: odd triangles are emitted (k-1, k-2, k) to keep it.
                if (k % 2 == 0) {
                    tri.insert(tri.end(), {idx[k - 2], idx[k - 1], idx[k]});
                } else {
                    tri.insert(tri.end(), {idx[k - 1], idx[k - 2], idx[k]});
                }
            }
        } else {
            for (std::size_t k = 2; k < idx.size(); ++k) {
                tri.insert(tri.end(), {idx[0], idx[k - 1], idx[k]});
            }
        }
        if (tri.empty()) {
            skip("empty");
            continue;
        }

        const scene::TranslatedDraw& t = r.translation;
        const bool sky = cls.categories.test(InstanceCategories::Sky);
        const bool hasColor0 = g->vertices.color0.defined();
        const bool hasNormals = g->vertices.normal.defined();
        RasterDrawGpu gd;
        gd.material = legacyMaterial(t.material, hasColor0, sky);
        if (hasNormals) {
            gd.material.flags |= kMatHasNormals;
        }
        // Bucket.
        if (sky) {
            rd.bucket = Bucket::Opaque;
            ++out.stats.unlit;
        } else if (isDecal(cls.categories) && (opt.features & kFeatureDecals)) {
            rd.bucket = Bucket::Decal;
        } else if (t.material.blendMode.enableBlending && (opt.features & kFeatureForward)) {
            rd.bucket = Bucket::Blend;
        } else {
            rd.bucket = Bucket::Opaque;
        }
        // Colour texture.
        const std::int32_t slot = cls.colorTextureSlots[0];
        for (const tap::CaptureDrawRecord::BoundTexture& bt : r.textures) {
            if (slot >= 0 && bt.slot == static_cast<std::uint32_t>(slot)) {
                rd.texture = bt.texture;
            }
        }
        if (rd.texture != tap::kNoResource && in.textureHandle) {
            gd.material.texture = in.textureHandle(rd.texture);
            if (gd.material.texture) {
                gd.material.flags |= kMatTextured;
                textures.insert(rd.texture);
                ++out.stats.textured;
            }
        }
        if (in.samplerHandle) {
            gd.material.sampler = in.samplerHandle(r.colorSampler);
        }
        // RL-3.2 replacement material for the draw's legacy material hash.
        if (in.replacementMaterial && t.material.hash() != hash::kEmptyHash && !sky) {
            if (const replace::MaterialDef* def = in.replacementMaterial(t.material.hash())) {
                if (!def->ignoreMaterial) {
                    RasterMaterial& m = gd.material;
                    for (int c = 0; c < 4; ++c) {
                        m.diffuse[c] = static_cast<float>(def->baseColor[static_cast<std::size_t>(c)]);
                    }
                    m.flags &= ~kMatVertexColor;
                    m.roughness = std::clamp(static_cast<float>(def->roughness), 0.02f, 1.f);
                    m.metallic = std::clamp(static_cast<float>(def->metallic), 0.f, 1.f);
                    // Emission: nits relative to an 80-nit display white, tinted by the base colour.
                    const float e = static_cast<float>(def->emissiveNits) / 80.f;
                    for (int c = 0; c < 3; ++c) {
                        m.emissive[c] = std::min(1.f, m.diffuse[c] * e);
                    }
                    m.flags |= kMatReplacement;
                    ++out.stats.replaced;
                }
            }
        }
        if (fog.mode != scene::d3dff::FOG_NONE && !sky) {
            gd.material.flags |= kMatFog;
        }
        rd.castShadow = rd.bucket == Bucket::Opaque && !sky;
        if (rd.castShadow) {
            gd.material.flags |= kMatCastShadow;
        }
        // Transforms.
        std::memcpy(gd.objectToWorld, t.transforms.objectToWorld.data(), sizeof(gd.objectToWorld));
        multiplyRowMajor(t.transforms.worldToView.data(), t.transforms.viewToProjection.data(), gd.worldToClip);
        normalMatrix(gd.objectToWorld, gd.normalToWorld);
        rd.blend = t.material.blendMode;
        rd.zEnable = t.zEnable;
        rd.zWrite = t.zWriteEnable;
        rd.cullMode = r.cullMode;
        rd.minZ = t.minZ;
        rd.maxZ = t.maxZ;
        // Vertices (de-indexed).
        const bool texTransform = !isIdentity(t.transforms.textureTransform);
        if (t.transforms.texgenMode != scene::TexGenMode::None) {
            ++out.stats.texgenIgnored;
        }
        const float* tt = t.transforms.textureTransform.data();
        rd.firstVertex = static_cast<std::uint32_t>(out.vertices.size());
        for (const std::uint32_t v : tri) {
            if (v >= g->vertexCount) {
                continue;
            }
            RasterVertex rv;
            const std::array<float, 4> p = readAttribute(g->vertices.position, v);
            rv.pos[0] = p[0];
            rv.pos[1] = p[1];
            rv.pos[2] = p[2];
            if (hasNormals) {
                const std::array<float, 4> n = readAttribute(g->vertices.normal, v);
                rv.normal[0] = n[0];
                rv.normal[1] = n[1];
                rv.normal[2] = n[2];
            }
            if (g->vertices.texcoord.defined()) {
                const std::array<float, 4> uv = readAttribute(g->vertices.texcoord, v);
                rv.uv[0] = uv[0];
                rv.uv[1] = uv[1];
                if (texTransform) {
                    rv.uv[0] = uv[0] * tt[0] + uv[1] * tt[4] + tt[8];
                    rv.uv[1] = uv[0] * tt[1] + uv[1] * tt[5] + tt[9];
                }
            }
            if (hasColor0) {
                rv.color = packRgba8(readAttribute(g->vertices.color0, v));
            }
            if (rd.castShadow) {
                float w[3];
                transformPoint(gd.objectToWorld, rv.pos, w);
                for (int c = 0; c < 3; ++c) {
                    casterMin[c] = std::min(casterMin[c], w[c]);
                    casterMax[c] = std::max(casterMax[c], w[c]);
                }
            }
            out.vertices.push_back(rv);
        }
        rd.vertexCount = static_cast<std::uint32_t>(out.vertices.size()) - rd.firstVertex;
        if (rd.vertexCount < 3) {
            out.vertices.resize(rd.firstVertex);
            skip("empty");
            continue;
        }
        out.stats.triangles += rd.vertexCount / 3;
        rd.gpu = static_cast<std::uint32_t>(out.gpuDraws.size());
        out.gpuDraws.push_back(gd);
        switch (rd.bucket) {
        case Bucket::Opaque:
            ++out.stats.opaque;
            break;
        case Bucket::Decal:
            ++out.stats.decals;
            break;
        case Bucket::Blend:
            ++out.stats.blended;
            break;
        case Bucket::Skipped:
            break;
        }
        out.draws.push_back(rd);
    }
    out.stats.vertices = static_cast<std::uint32_t>(out.vertices.size());
    out.textures.assign(textures.begin(), textures.end());
    std::sort(out.textures.begin(), out.textures.end());
    if (out.gpuDraws.empty()) {
        return false;
    }

    // ---- lights: directional list, clustered point / spot lights (renderer WP-2.1 CPU oracle) ----
    RasterFrameGpu& k = out.constants;
    namespace rr = fuse::renderer;
    std::vector<rr::PointLightInput> points;
    std::vector<rr::SpotLightInput> spots;
    std::vector<std::uint32_t> pointSlots, spotSlots;
    const RasterLight* strongest = nullptr;
    for (const RasterLight& l : in.lights) {
        if (l.slot == kInvalid) {
            continue;
        }
        ++out.stats.lights;
        const gs::GpuLight& g = l.light;
        const auto type = static_cast<gs::GpuLightType>(g.type);
        if (type == gs::GpuLightType::Directional) {
            out.directional.push_back(l.slot);
            if (!strongest || g.intensity > strongest->light.intensity) {
                strongest = &l;
            }
            continue;
        }
        // Mirrored x: the cluster math's view basis is right-handed (right = forward x up), D3D's left-handed.
        const fuse::math::Vec3 pos{-g.position[0], g.position[1], g.position[2]};
        if (type == gs::GpuLightType::Spot) {
            rr::SpotLightInput s;
            s.position = pos;
            s.direction = {-g.direction[0], g.direction[1], g.direction[2]};
            s.radius = g.range;
            s.intensity = g.intensity;
            s.outerConeDeg = std::acos(std::clamp(g.cosOuter, -1.f, 1.f)) * 57.29577951f;
            s.innerConeDeg = std::acos(std::clamp(g.cosInner, -1.f, 1.f)) * 57.29577951f;
            spots.push_back(s);
            spotSlots.push_back(l.slot);
        } else {
            rr::PointLightInput p;
            p.position = pos;
            p.radius = g.range;
            p.intensity = g.intensity;
            points.push_back(p);
            pointSlots.push_back(l.slot);
        }
    }
    out.stats.directional = static_cast<std::uint32_t>(out.directional.size());
    out.stats.clustered = static_cast<std::uint32_t>(points.size() + spots.size());
    const bool clustered = out.camera.valid && out.camera.perspective && out.width > 0 && out.height > 0;
    rr::ClusterDesc desc;
    desc.tilesX = clustered ? opt.tilesX : 1u;
    desc.tilesY = clustered ? opt.tilesY : 1u;
    desc.slicesZ = clustered ? opt.slicesZ : 1u;
    desc.maxLightsPerCluster = 64;
    desc = rr::ClusterDesc::clampCounts(desc);
    const std::uint32_t clusters = desc.clusterCount();
    out.grid.assign(2u * clusters, 0u);
    if (clustered && !(points.empty() && spots.empty())) {
        rr::ClusterCameraDesc cam;
        cam.position = {-out.camera.eye[0], out.camera.eye[1], out.camera.eye[2]};
        cam.forward = {-out.camera.forward[0], out.camera.forward[1], out.camera.forward[2]};
        cam.up = {-out.camera.up[0], out.camera.up[1], out.camera.up[2]};
        cam.nearPlane = out.camera.nearZ;
        cam.farPlane = out.camera.farZ;
        cam.fovYRadians = out.camera.fovY;
        cam.screenWidth = out.width;
        cam.screenHeight = out.height;
        cam.reversedZ = false;
        std::vector<rr::ClusterAABB> aabbs;
        rr::cluster_math::buildClusterAabbs(desc, cam, aabbs, kernel::Backend::CpuReference);
        rr::ClusterCullLists lists;
        const rr::ClusterCullResult res = rr::cluster_math::cullLightsToClusterLists(
            desc, cam, aabbs, points, spots, lists, kernel::Backend::CpuReference);
        rr::ClusterGridSoA grid;
        rr::cluster_math::compactClusterLists(lists, clusters, grid, kernel::Backend::CpuReference);
        out.stats.lightsCulled = res.lightsCulled;
        for (std::uint32_t c = 0; c < clusters && c < grid.grid.size(); ++c) {
            out.grid[2u * c] = grid.grid[c].offset;
            out.grid[2u * c + 1u] = grid.grid[c].count;
            out.stats.clustersUsed += grid.grid[c].count ? 1u : 0u;
        }
        out.lightList.reserve(grid.lightList.size());
        for (const std::uint32_t li : grid.lightList) {
            out.lightList.push_back(li < pointSlots.size() ? pointSlots[li]
                                                           : spotSlots[li - static_cast<std::uint32_t>(pointSlots.size())]);
        }
    } else if (!(points.empty() && spots.empty())) {
        // No perspective main camera: one cluster holding every point / spot light.
        out.grid[0] = 0;
        out.grid[1] = static_cast<std::uint32_t>(points.size() + spots.size());
        out.lightList = pointSlots;
        out.lightList.insert(out.lightList.end(), spotSlots.begin(), spotSlots.end());
        out.stats.clustersUsed = 1;
    }

    // Fallback light (Remix rtx.fallbackLightMode): 1 = when the frame has no light, 2 = always.
    const bool fallback = opt.fallbackLightMode == 2 || (opt.fallbackLightMode == 1 && out.stats.lights == 0);
    out.stats.fallbackLight = fallback;
    if (fallback) {
        float d[3] = {opt.fallbackDirection[0], opt.fallbackDirection[1], opt.fallbackDirection[2]};
        normalize3(d);
        k.fallbackDir[0] = d[0];
        k.fallbackDir[1] = d[1];
        k.fallbackDir[2] = d[2];
        k.fallbackDir[3] = 1.f;
        k.fallbackColor[0] = opt.fallbackRadiance[0];
        k.fallbackColor[1] = opt.fallbackRadiance[1];
        k.fallbackColor[2] = opt.fallbackRadiance[2];
    }

    // Shadows (T1+): the strongest distant light (or the fallback light) over the casters' bounds.
    if ((opt.features & kFeatureShadows) && casterMin[0] <= casterMax[0] && (strongest || fallback)) {
        float dir[3];
        if (strongest) {
            dir[0] = strongest->light.direction[0];
            dir[1] = strongest->light.direction[1];
            dir[2] = strongest->light.direction[2];
            out.shadowLight = strongest->slot;
        } else {
            dir[0] = k.fallbackDir[0];
            dir[1] = k.fallbackDir[1];
            dir[2] = k.fallbackDir[2];
            out.shadowLight = kInvalid;
        }
        normalize3(dir);
        const float centre[3] = {0.5f * (casterMin[0] + casterMax[0]), 0.5f * (casterMin[1] + casterMax[1]),
                                 0.5f * (casterMin[2] + casterMax[2])};
        float view[16];
        lookAlong(centre, dir, view);
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {(c & 1) ? casterMax[0] : casterMin[0], (c & 2) ? casterMax[1] : casterMin[1],
                                (c & 4) ? casterMax[2] : casterMin[2]};
            float v[3];
            transformPoint(view, p, v);
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], v[a]);
                hi[a] = std::max(hi[a], v[a]);
            }
        }
        const float pad = 0.01f * std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-3f});
        for (int a = 0; a < 3; ++a) {
            lo[a] -= pad;
            hi[a] += pad;
        }
        // D3DXMatrixOrthoOffCenterLH.
        const float sx = 2.f / (hi[0] - lo[0]), sy = 2.f / (hi[1] - lo[1]), sz = 1.f / (hi[2] - lo[2]);
        const float ortho[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, -(hi[0] + lo[0]) / (hi[0] - lo[0]),
                                 -(hi[1] + lo[1]) / (hi[1] - lo[1]), -lo[2] * sz, 1};
        multiplyRowMajor(view, ortho, out.shadowMatrix);
        k.shadowParams[2] = std::max(hi[0] - lo[0], hi[1] - lo[1]); // divided by the map size on the GPU side
        out.shadow = true;
        out.stats.shadow = true;
    }

    // ---- constants ----
    k.eye[0] = out.camera.eye[0];
    k.eye[1] = out.camera.eye[1];
    k.eye[2] = out.camera.eye[2];
    k.forward[0] = out.camera.forward[0];
    k.forward[1] = out.camera.forward[1];
    k.forward[2] = out.camera.forward[2];
    k.forward[3] = out.camera.nearZ;
    k.cluster[0] = static_cast<float>(desc.tilesX);
    k.cluster[1] = static_cast<float>(desc.tilesY);
    k.cluster[2] = static_cast<float>(desc.slicesZ);
    k.cluster[3] = out.camera.farZ;
    k.screen[0] = static_cast<float>(out.width);
    k.screen[1] = static_cast<float>(out.height);
    k.screen[2] = out.width ? 1.f / static_cast<float>(out.width) : 0.f;
    k.screen[3] = out.height ? 1.f / static_cast<float>(out.height) : 0.f;
    if (fog.mode != scene::d3dff::FOG_NONE) {
        k.fogColor[0] = fog.color[0];
        k.fogColor[1] = fog.color[1];
        k.fogColor[2] = fog.color[2];
        k.fogColor[3] = static_cast<float>(fog.mode);
        k.fogParams[0] = fog.scale;
        k.fogParams[1] = fog.end;
        k.fogParams[2] = fog.density;
        k.fogParams[3] = 1.f;
    }
    k.ambient[0] = opt.ambient[0];
    k.ambient[1] = opt.ambient[1];
    k.ambient[2] = opt.ambient[2];
    k.ambient[3] = opt.exposure;
    const std::array<float, 4> clear = scene::decodeD3DColor(in.haveClear ? in.clearColor : 0xff000000u);
    std::copy(clear.begin(), clear.end(), k.clearColor);
    std::memcpy(k.shadowMatrix, out.shadowMatrix, sizeof(k.shadowMatrix));
    k.counts[0] = static_cast<std::uint32_t>(out.directional.size());
    k.counts[1] = clusters;
    k.counts[2] = out.shadowLight;
    k.counts[3] = out.features & ~(out.shadow ? 0u : static_cast<std::uint32_t>(kFeatureShadows));
    k.shadowParams[3] = out.shadow ? 1.f : 0.f;
    return true;
}

} // namespace fuse::relight::render::raster
