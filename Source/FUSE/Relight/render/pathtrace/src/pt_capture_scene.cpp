// FUSE Relight RL-5.1: PtScene from an RL-1.8 capture (see pt_capture_scene.hpp).
#include <fuse/relight/render/pathtrace/pt_capture_scene.hpp>

#include <fuse/relight/mods/import/light_table.hpp>
#include <fuse/relight/mods/import/mesh_import.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>
#include <fuse/relight/render/lights/light_set.hpp>

#include <cmath>

namespace fuse::relight::render::pathtrace {

namespace usd = fuse::relight::mods::usd;
namespace imp = fuse::relight::mods::import;

namespace {

/// The attribute's default, else its earliest time sample.
const usd::Value* earliest(const usd::Attribute* a) {
    if (a == nullptr) {
        return nullptr;
    }
    if (!a->timeSamples.empty()) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < a->timeSamples.size(); ++i) {
            if (a->timeSamples[i].first < a->timeSamples[best].first) {
                best = i;
            }
        }
        return &a->timeSamples[best].second.get();
    }
    if (a->hasDefault) {
        return &a->defaultValue.get();
    }
    return nullptr;
}

bool invisible(const usd::ComposedStage& stage, const std::string& path) {
    std::string p = path;
    while (!p.empty() && p != "/") {
        if (const usd::Prim* prim = stage.find(p)) {
            const usd::Value* v = earliest(prim->attribute("visibility"));
            if (v != nullptr) {
                const std::optional<std::string> s = v->asString();
                if (s && *s == "invisible") {
                    return true;
                }
            }
        }
        const std::size_t cut = p.find_last_of('/');
        if (cut == std::string::npos || cut == 0u) {
            break;
        }
        p.resize(cut);
    }
    return false;
}

float attrFloat(const usd::Prim& prim, const char* name, float fallback) {
    const usd::Value* v = earliest(prim.attribute(name));
    if (v == nullptr) {
        return fallback;
    }
    const std::optional<double> d = v->asNumber();
    return d ? static_cast<float>(*d) : fallback;
}

} // namespace

bool loadCaptureScene(const std::string& usdaPath, const PtCaptureOptions& options, PtScene& out,
                      PtCaptureReport* report, std::string* error) {
    PtCaptureReport local;
    PtCaptureReport& rep = report != nullptr ? *report : local;
    rep = PtCaptureReport{};
    out = PtScene{};
    const usd::ComposedStage stage = usd::readStage(usdaPath);
    if (!stage.ok) {
        if (error != nullptr) {
            *error = "cannot read " + usdaPath;
        }
        return false;
    }
    out.materials.push_back(options.material);
    out.sky[0] = options.sky[0];
    out.sky[1] = options.sky[1];
    out.sky[2] = options.sky[2];
    for (const auto& [path, prim] : stage.prims) {
        if (prim.typeName == "Shader") {
            for (const auto& [name, attr] : prim.attributes) {
                if (name.find("_texture") != std::string::npos && attr.hasDefault &&
                    attr.defaultValue->kind == usd::Value::Kind::Asset && !attr.defaultValue->text.empty()) {
                    ++rep.textures;
                }
            }
            continue;
        }
        if (prim.typeName == "Mesh") {
            if (path.rfind("/RootNode/meshes/", 0) == 0u || invisible(stage, path)) {
                continue;
            }
            std::vector<imp::MeshIssue> issues;
            const std::optional<imp::ImportedMesh> mesh = imp::importMesh(stage, prim, "", &issues);
            for (const imp::MeshIssue& i : issues) {
                rep.warnings.push_back(path + ": " + i.message);
            }
            if (!mesh || mesh->indices.size() < 3u) {
                continue;
            }
            PtMesh m;
            m.positions.reserve(mesh->points.size() * 3u);
            for (const auto& p : mesh->points) {
                m.positions.insert(m.positions.end(), {p[0], p[1], p[2]});
            }
            if (mesh->normals.size() == mesh->points.size()) {
                for (const auto& n : mesh->normals) {
                    m.normals.insert(m.normals.end(), {n[0], n[1], n[2]});
                }
            }
            if (mesh->uv0.size() == mesh->points.size()) {
                for (const auto& t : mesh->uv0) {
                    m.uvs.insert(m.uvs.end(), {t[0], 1.f - t[1]}); // USD st -> D3D uv
                }
            }
            if (mesh->colors.size() == mesh->points.size()) {
                for (const auto& c : mesh->colors) {
                    m.colors.insert(m.colors.end(), {c[0], c[1], c[2], c[3]});
                }
            }
            m.indices = mesh->indices;
            m.material = 0;
            PtInstance inst;
            inst.mesh = static_cast<u32>(out.meshes.size());
            const imp::Mat4d world = imp::relativeTransform(stage, path, "");
            inst.objectToWorld = fromRowVectorMatrix(world.data());
            rep.triangles += static_cast<u32>(m.indices.size() / 3u);
            out.meshes.push_back(std::move(m));
            out.instances.push_back(inst);
            ++rep.meshes;
            continue;
        }
        if (options.lights && imp::isUsdLightType(prim.typeName)) {
            if (invisible(stage, path)) {
                continue;
            }
            std::vector<std::string> issues;
            const imp::LightParams params = imp::readLightParams(prim, &issues);
            const imp::Mat4d world = imp::relativeTransform(stage, path, "");
            float m16[16];
            for (int k = 0; k < 16; ++k) {
                m16[k] = static_cast<float>(world[static_cast<std::size_t>(k)]);
            }
            lk::RlLight light{};
            const char* warning = nullptr;
            if (lights::lightFromUsd(params, m16, light, &warning) && light.kind != lk::kRlKindNone) {
                out.lights.push_back(light);
                ++rep.lights;
            } else {
                ++rep.skippedLights;
            }
            if (warning != nullptr) {
                rep.warnings.push_back(path + ": " + warning);
            }
            continue;
        }
        if (prim.typeName == "Camera" && !rep.camera) {
            const imp::Mat4d m = imp::relativeTransform(stage, path, "");
            PtCamera c;
            c.origin[0] = static_cast<float>(m[12]);
            c.origin[1] = static_cast<float>(m[13]);
            c.origin[2] = static_cast<float>(m[14]);
            c.forward[0] = -static_cast<float>(m[8]);
            c.forward[1] = -static_cast<float>(m[9]);
            c.forward[2] = -static_cast<float>(m[10]);
            c.up[0] = static_cast<float>(m[4]);
            c.up[1] = static_cast<float>(m[5]);
            c.up[2] = static_cast<float>(m[6]);
            const float focal = attrFloat(prim, "focalLength", 50.f);
            const float horizontal = attrFloat(prim, "horizontalAperture", 20.955f);
            const float vertical = horizontal / options.aspect;
            c.fovY = 2.f * std::atan(vertical / (2.f * focal));
            c.aspect = options.aspect;
            c.leftHanded = false;
            out.camera = c;
            rep.camera = true;
        }
    }
    if (out.meshes.empty()) {
        if (error != nullptr) {
            *error = usdaPath + ": no visible mesh";
        }
        return false;
    }
    return true;
}

} // namespace fuse::relight::render::pathtrace
