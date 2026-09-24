// FUSE Relight RL-5.1: the path tracer's scene compiler (see pt_scene.hpp).
#include <fuse/relight/render/pathtrace/pt_scene.hpp>

#include <fuse/renderer/geometry/meshlet_builder.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::relight::render::pathtrace {

namespace geo = fuse::renderer::geometry;
namespace gs = fuse::renderer::gpu_scene;

namespace {

bool finite(float v) { return std::isfinite(v); }

float displayToLinear(float c) { return std::pow(std::max(c, 0.f), 2.2f); }

lk::float3 xform(const Mat34& m, float x, float y, float z) {
    return lk::float3(m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                      m[8] * x + m[9] * y + m[10] * z + m[11]);
}

float maxEmission(const PtMaterial& m) {
    return std::max(m.bsdf.emission.x, std::max(m.bsdf.emission.y, m.bsdf.emission.z));
}

bool emissive(const PtMaterial& m) { return (m.flags & kPtMatUnlit) == 0u && maxEmission(m) > 0.f; }

void normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

} // namespace

Mat34 identity34() { return Mat34{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f}; }

Mat34 fromD3dMatrix(const float* m) {
    return Mat34{m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14]};
}

Mat34 fromRowVectorMatrix(const double* m) {
    Mat34 r{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            r[static_cast<std::size_t>(row * 4 + col)] = static_cast<float>(m[col * 4 + row]);
        }
        r[static_cast<std::size_t>(row * 4 + 3)] = static_cast<float>(m[12 + row]);
    }
    return r;
}

bool PtCompiledScene::compile(const PtScene& scene, const PtCompileOptions& options, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) {
            *error = why;
        }
        m_valid = false;
        return false;
    };
    m_valid = false;
    m_options = options;
    m_stats = PtCompileStats{};
    if (scene.materials.empty()) {
        return fail("the scene has no material");
    }
    if (scene.portals.size() > kPtMaxPortals) {
        return fail("too many portals");
    }
    m_materials = scene.materials;
    const u32 materialCount = static_cast<u32>(m_materials.size());

    // Materials.
    m_materialWords.assign(std::size_t(materialCount) * kPtMaterialWords, Word{});
    for (u32 i = 0; i < materialCount; ++i) {
        const PtMaterial& m = m_materials[i];
        Word* w = m_materialWords.data() + std::size_t(i) * kPtMaterialWords;
        bsdfk::bsdfMaterialPack(m.bsdf, w);
        w[11] = Word(float(m.texture & 0xFFFFu), float(m.texture >> 16u), float(m.sampler & 0xFFFFu),
                     float(m.sampler >> 16u));
        w[12] = Word(float(m.flags), m.alphaReference, float(m.alphaCompare),
                     m.bsdf.model == bsdfk::kBsdfModelPortal && m.portal >= 0 ? float(m.portal) : -1.f);
    }

    // Meshes: cook, decode, and the triangle table in MTRI order.
    m_meshes.clear();
    m_meshes.resize(scene.meshes.size());
    m_triangleWords.clear();
    u32 triangleBase = 0;
    for (std::size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const PtMesh& src = scene.meshes[mi];
        Mesh& out = m_meshes[mi];
        const u32 vertexCount = static_cast<u32>(src.positions.size() / 3u);
        if (vertexCount == 0u || src.indices.size() < 3u || src.indices.size() % 3u != 0u ||
            (!src.normals.empty() && src.normals.size() != src.positions.size()) ||
            (!src.uvs.empty() && src.uvs.size() != std::size_t(vertexCount) * 2u) ||
            (!src.colors.empty() && src.colors.size() != std::size_t(vertexCount) * 4u)) {
            return fail("mesh " + std::to_string(mi) + ": inconsistent streams");
        }
        for (u32 index : src.indices) {
            if (index >= vertexCount) {
                return fail("mesh " + std::to_string(mi) + ": index out of range");
            }
        }
        for (float v : src.positions) {
            if (!finite(v)) {
                return fail("mesh " + std::to_string(mi) + ": non-finite position");
            }
        }
        geo::MeshletSource ms{};
        ms.positions = src.positions.data();
        ms.normals = src.normals.empty() ? nullptr : src.normals.data();
        ms.uvs = src.uvs.empty() ? nullptr : src.uvs.data();
        ms.vertex_count = vertexCount;
        ms.indices = src.indices.data();
        ms.index_count = static_cast<u32>(src.indices.size());
        if (src.submeshes.empty()) {
            if (src.material >= materialCount) {
                return fail("mesh " + std::to_string(mi) + ": material out of range");
            }
            ms.submeshes.push_back(geo::MeshletSourceSubmesh{0u, ms.index_count, src.material});
        } else {
            for (const PtSubmesh& s : src.submeshes) {
                if (s.material >= materialCount || s.indexCount % 3u != 0u ||
                    std::size_t(s.indexOffset) + s.indexCount > src.indices.size()) {
                    return fail("mesh " + std::to_string(mi) + ": bad submesh");
                }
                ms.submeshes.push_back(geo::MeshletSourceSubmesh{s.indexOffset, s.indexCount, s.material});
            }
        }
        geo::MeshletBuildOptions bo{};
        bo.backend = kernel::Backend::CpuReference;
        bo.keep_source_vertex_map = true;
        std::string why;
        if (!geo::build_meshlets(ms, bo, out.cooked, &why)) {
            return fail("mesh " + std::to_string(mi) + ": build_meshlets: " + why);
        }
        geo::DecodedVertices decoded;
        geo::decode_vertices(out.cooked, decoded, kernel::Backend::CpuReference);
        out.decoded = std::move(decoded.positions);
        out.triangleBase = triangleBase;
        out.triangles = out.cooked.triangle_count();
        out.indices.clear();
        out.indices.reserve(std::size_t(out.triangles) * 3u);
        out.emissive = 0;
        out.emissiveTriangles.clear();
        m_triangleWords.resize(std::size_t(triangleBase + out.triangles) * kPtTriangleWords);
        u32 t = 0;
        for (const geo::MeshletRecord& meshlet : out.cooked.meshlets) {
            const u32 material = out.cooked.submeshes[meshlet.submesh].material_index;
            const bool emits = m_options.emissiveLights && emissive(m_materials[material]);
            for (u32 k = 0; k < meshlet.triangle_count; ++k, ++t) {
                const u32 packed = out.cooked.meshlet_triangles[meshlet.triangle_offset + k];
                Word* w = m_triangleWords.data() + std::size_t(triangleBase + t) * kPtTriangleWords;
                float uv[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
                for (u32 c = 0; c < 3u; ++c) {
                    const u32 local = geo::triangle_index(packed, c);
                    const u32 mv = out.cooked.meshlet_vertices[meshlet.vertex_offset + local];
                    out.indices.push_back(mv);
                    const u32 sv = out.cooked.source_vertices.empty() ? mv : out.cooked.source_vertices[mv];
                    const float* p = out.decoded.data() + std::size_t(mv) * 3u;
                    float n[3] = {0.f, 0.f, 0.f};
                    if (!src.normals.empty()) {
                        n[0] = src.normals[std::size_t(sv) * 3u + 0u];
                        n[1] = src.normals[std::size_t(sv) * 3u + 1u];
                        n[2] = src.normals[std::size_t(sv) * 3u + 2u];
                    }
                    if (!src.uvs.empty()) {
                        uv[c * 2u + 0u] = src.uvs[std::size_t(sv) * 2u + 0u];
                        uv[c * 2u + 1u] = src.uvs[std::size_t(sv) * 2u + 1u];
                    }
                    Word color(1.f, 1.f, 1.f, 1.f);
                    if (!src.colors.empty()) {
                        const float* cc = src.colors.data() + std::size_t(sv) * 4u;
                        color = Word(displayToLinear(cc[0]), displayToLinear(cc[1]), displayToLinear(cc[2]), cc[3]);
                    }
                    w[c] = Word(p[0], p[1], p[2], 0.f);
                    w[3u + c] = Word(n[0], n[1], n[2], 0.f);
                    w[7u + c] = color;
                }
                // uv0 = (w0.w, w1.w), uv1 = (w2.w, w3.w), uv2 = (w4.w, w5.w)
                w[0].w = uv[0];
                w[1].w = uv[1];
                w[2].w = uv[2];
                w[3].w = uv[3];
                w[4].w = uv[4];
                w[5].w = uv[5];
                float ordinal = -1.f;
                if (emits) {
                    ordinal = float(out.emissive);
                    out.emissiveTriangles.push_back(t);
                    ++out.emissive;
                }
                w[6] = Word(float(material), ordinal, 0.f, 0.f);
            }
        }
        triangleBase += out.triangles;
        m_stats.triangles += out.triangles;
    }
    m_stats.meshes = static_cast<u32>(m_meshes.size());
    m_stats.materials = materialCount;

    // Instances (slot i by default).
    for (const PtInstance& inst : scene.instances) {
        if (inst.mesh >= m_meshes.size()) {
            return fail("instance mesh out of range");
        }
    }
    m_instances = scene.instances;
    m_slots.resize(m_instances.size());
    for (u32 i = 0; i < m_slots.size(); ++i) {
        m_slots[i] = i;
    }
    m_slotCount = static_cast<u32>(m_instances.size());
    m_stats.instances = m_slotCount;
    // Light-map bases: every instance of a mesh with emissive triangles gets a range (visible or not: the sizes stay
    // fixed across update()).
    m_lightMapBase.assign(m_instances.size(), ~0u);
    u32 mapSize = 0;
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const Mesh& mesh = m_meshes[m_instances[i].mesh];
        if (mesh.emissive != 0u) {
            m_lightMapBase[i] = mapSize;
            mapSize += mesh.emissive;
        }
    }
    m_lightMap.assign(mapSize, -1.f);

    // Portals.
    m_portalWords.assign(std::max<std::size_t>(scene.portals.size(), 1u) * kPtPortalWords, Word{});
    for (std::size_t p = 0; p < scene.portals.size(); ++p) {
        const Mat34& m = scene.portals[p];
        for (u32 r = 0; r < 3u; ++r) {
            m_portalWords[p * kPtPortalWords + r] = Word(m[r * 4u + 0u], m[r * 4u + 1u], m[r * 4u + 2u], m[r * 4u + 3u]);
        }
    }
    m_portalCount = static_cast<u32>(scene.portals.size());
    m_textures = scene.textures;
    std::memcpy(m_sky, scene.sky, sizeof(m_sky));
    m_camera = scene.camera;
    m_prevCamera = scene.hasPrevCamera ? scene.prevCamera : scene.camera;
    m_lights.reserve(static_cast<u32>(scene.lights.size()) + mapSize, 8u);

    packInstances();
    if (!buildLights(scene)) {
        return fail("light set build failed");
    }
    // Reference acceleration structures.
    for (u32 mi = 0; mi < m_meshes.size(); ++mi) {
        const Mesh& mesh = m_meshes[mi];
        m_reference.setMesh(mi, mesh.decoded.data(), static_cast<u32>(mesh.decoded.size() / 3u), mesh.indices.data(),
                            static_cast<u32>(mesh.indices.size()));
    }
    m_refInstances.clear();
    m_refTransforms.clear();
    buildReference();
    m_valid = true;
    return true;
}

void PtCompiledScene::packInstances() {
    m_instanceWords.assign(std::size_t(std::max(m_slotCount, 1u)) * kPtInstanceWords, Word{});
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const PtInstance& inst = m_instances[i];
        Word* w = m_instanceWords.data() + std::size_t(m_slots[i]) * kPtInstanceWords;
        const Mat34& m = inst.objectToWorld;
        w[0] = Word(m[0], m[1], m[2], m[3]);
        w[1] = Word(m[4], m[5], m[6], m[7]);
        w[2] = Word(m[8], m[9], m[10], m[11]);
        w[3] = Word(float(m_meshes[inst.mesh].triangleBase), float(inst.flags),
                    m_lightMapBase[i] == ~0u ? -1.f : float(m_lightMapBase[i]), 0.f);
    }
}

bool PtCompiledScene::buildLights(const PtScene& scene) {
    m_lights.beginFrame();
    m_stats.analyticLights = 0;
    m_stats.emissiveLights = 0;
    m_stats.rejectedLights = 0;
    for (std::size_t i = 0; i < scene.lights.size(); ++i) {
        if (m_lights.addLight(scene.lights[i], 0x1000000ull + i)) {
            ++m_stats.analyticLights;
        } else {
            ++m_stats.rejectedLights;
        }
    }
    // (lightCount() is the assembled table: known after build(); count the accepted adds instead)
    m_analyticCount = m_stats.analyticLights;
    u32 added = 0;
    for (u32 i = 0; i < m_instances.size(); ++i) {
        if (m_lightMapBase[i] == ~0u) {
            continue;
        }
        const PtInstance& inst = m_instances[i];
        const Mesh& mesh = m_meshes[inst.mesh];
        for (u32 k = 0; k < mesh.emissive; ++k) {
            float& slot = m_lightMap[m_lightMapBase[i] + k];
            slot = -1.f;
            // An instance that rays cannot see emits nothing (no estimator could collect it through BSDF sampling,
            // and NEE alone with MIS weights would be biased).
            if ((inst.flags & kPtInstanceVisible) == 0u) {
                continue;
            }
            if (added >= m_options.emissiveLimit) {
                ++m_stats.rejectedLights;
                continue;
            }
            const u32 t = mesh.emissiveTriangles[k];
            const Word w6 = m_triangleWords[std::size_t(mesh.triangleBase + t) * kPtTriangleWords + 6u];
            const PtMaterial& mat = m_materials[static_cast<u32>(w6.x)];
            const float* p0 = mesh.decoded.data() + std::size_t(mesh.indices[t * 3u + 0u]) * 3u;
            const float* p1 = mesh.decoded.data() + std::size_t(mesh.indices[t * 3u + 1u]) * 3u;
            const float* p2 = mesh.decoded.data() + std::size_t(mesh.indices[t * 3u + 2u]) * 3u;
            const lk::float3 radiance(mat.bsdf.emission.x, mat.bsdf.emission.y, mat.bsdf.emission.z);
            const lk::RlLight light = lights::makeTriangleLight(
                xform(inst.objectToWorld, p0[0], p0[1], p0[2]), xform(inst.objectToWorld, p1[0], p1[1], p1[2]),
                xform(inst.objectToWorld, p2[0], p2[1], p2[2]), radiance, true);
            if (m_lights.addLight(light, (u64(i) << 32) | t)) {
                slot = float(m_analyticCount + added);
                ++added;
                ++m_stats.emissiveLights;
            } else {
                ++m_stats.rejectedLights;
            }
        }
    }
    if (!m_lights.build()) {
        return false;
    }
    m_lightRecords.resize(m_lights.lightCount());
    for (u32 i = 0; i < m_lights.lightCount(); ++i) {
        m_lightRecords[i] = m_lights.light(i);
    }
    return true;
}

void PtCompiledScene::buildReference() {
    const bool rebuild = m_refInstances.size() != m_slotCount;
    m_refInstances.resize(m_slotCount);
    m_refTransforms.resize(m_slotCount);
    for (u32 s = 0; s < m_slotCount; ++s) {
        m_refInstances[s] = gs::GpuInstance{};
        m_refInstances[s].flags = 0u;
        m_refTransforms[s] = gs::GpuTransform{};
    }
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const PtInstance& inst = m_instances[i];
        gs::GpuInstance& gi = m_refInstances[m_slots[i]];
        gi.mesh = inst.mesh;
        gi.material = 0;
        gi.flags = gs::kInstanceValid | ((inst.flags & kPtInstanceVisible) != 0u ? gs::kInstanceVisible : 0u) |
                   ((inst.flags & kPtInstanceShadow) != 0u ? gs::kInstanceCastShadow : 0u);
        gs::GpuTransform& gt = m_refTransforms[m_slots[i]];
        for (u32 r = 0; r < 3u; ++r) {
            for (u32 c = 0; c < 4u; ++c) {
                gt.rows[r][c] = inst.objectToWorld[r * 4u + c];
            }
        }
    }
    if (rebuild) {
        m_reference.setInstances(m_refInstances.data(), m_refTransforms.data(), m_slotCount);
    } else {
        m_reference.updateInstances(m_refInstances.data(), m_refTransforms.data(), m_slotCount);
    }
}

bool PtCompiledScene::update(const PtScene& scene) {
    if (!m_valid || scene.instances.size() != m_instances.size() || scene.materials.size() != m_materials.size() ||
        scene.meshes.size() != m_meshes.size() || scene.portals.size() * kPtPortalWords > m_portalWords.size()) {
        return false;
    }
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        if (scene.instances[i].mesh != m_instances[i].mesh) {
            return false;
        }
    }
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
        if (emissive(scene.materials[i]) != emissive(m_materials[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
        m_materials[i] = scene.materials[i];
        const PtMaterial& m = m_materials[i];
        Word* w = m_materialWords.data() + i * kPtMaterialWords;
        bsdfk::bsdfMaterialPack(m.bsdf, w);
        w[11] = Word(float(m.texture & 0xFFFFu), float(m.texture >> 16u), float(m.sampler & 0xFFFFu),
                     float(m.sampler >> 16u));
        w[12] = Word(float(m.flags), m.alphaReference, float(m.alphaCompare),
                     m.bsdf.model == bsdfk::kBsdfModelPortal && m.portal >= 0 ? float(m.portal) : -1.f);
    }
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        m_instances[i].objectToWorld = scene.instances[i].objectToWorld;
        m_instances[i].flags = scene.instances[i].flags;
    }
    for (std::size_t p = 0; p < scene.portals.size(); ++p) {
        const Mat34& m = scene.portals[p];
        for (u32 r = 0; r < 3u; ++r) {
            m_portalWords[p * kPtPortalWords + r] = Word(m[r * 4u + 0u], m[r * 4u + 1u], m[r * 4u + 2u], m[r * 4u + 3u]);
        }
    }
    m_portalCount = static_cast<u32>(scene.portals.size());
    std::memcpy(m_sky, scene.sky, sizeof(m_sky));
    m_prevCamera = scene.hasPrevCamera ? scene.prevCamera : m_camera;
    m_camera = scene.camera;
    packInstances();
    if (!buildLights(scene)) {
        m_valid = false;
        return false;
    }
    buildReference();
    return true;
}

bool PtCompiledScene::setSlots(const std::vector<u32>& slots) {
    if (slots.size() != m_instances.size()) {
        return false;
    }
    u32 count = 0;
    for (u32 s : slots) {
        count = std::max(count, s + 1u);
    }
    std::vector<u8> used(count, 0u);
    for (u32 s : slots) {
        if (used[s] != 0u) {
            return false;
        }
        used[s] = 1u;
    }
    m_slots = slots;
    m_slotCount = count;
    packInstances();
    m_refInstances.clear();
    buildReference();
    return true;
}

void PtCompiledScene::packParams(const PtSettings& settings, u32 width, u32 height, u32 frameSeed, u32 sampleBase,
                                 Word* out) const {
    auto basis = [&](const PtCamera& c, float o[3], float r[3], float u[3], float f[3]) {
        std::memcpy(o, c.origin, sizeof(float) * 3u);
        std::memcpy(f, c.forward, sizeof(float) * 3u);
        normalize3(f);
        float up[3] = {c.up[0], c.up[1], c.up[2]};
        if (c.leftHanded) {
            cross3(up, f, r);
            normalize3(r);
            cross3(f, r, u);
        } else {
            cross3(f, up, r);
            normalize3(r);
            cross3(r, f, u);
        }
        normalize3(u);
        const float aspect = c.aspect > 0.f ? c.aspect : float(width) / float(std::max(height, 1u));
        const float ty = std::tan(0.5f * c.fovY);
        for (int k = 0; k < 3; ++k) {
            r[k] *= ty * aspect;
            u[k] *= ty;
        }
    };
    float o[3], r[3], u[3], f[3], po[3], pr[3], pu[3], pf[3];
    basis(m_camera, o, r, u, f);
    basis(m_prevCamera, po, pr, pu, pf);
    out[0] = Word(o[0], o[1], o[2], float(width));
    out[1] = Word(r[0], r[1], r[2], float(height));
    out[2] = Word(u[0], u[1], u[2], float(frameSeed & 0xFFFFFFu));
    out[3] = Word(f[0], f[1], f[2], float(settings.maxBounces));
    out[4] = Word(po[0], po[1], po[2], float(settings.rrStart));
    out[5] = Word(pr[0], pr[1], pr[2], float(settings.flags));
    out[6] = Word(pu[0], pu[1], pu[2], float(m_lights.lightCount()));
    out[7] = Word(pf[0], pf[1], pf[2], float(m_analyticCount));
    out[8] = Word(m_sky[0], m_sky[1], m_sky[2], float(settings.psrMaxBounces));
    out[9] = Word(settings.rayEps, settings.psrMirrorRoughness, float(sampleBase & 0xFFFFFFu),
                  float(settings.samplesPerPixel));
    out[10] = Word(float(settings.maxAlphaSkips), float(m_portalCount), 0.f, 0.f);
}

} // namespace fuse::relight::render::pathtrace
