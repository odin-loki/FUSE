// FUSE Relight RL-3.1: RL-1.8's Remix-compatible USDA capture read back through the USD reader (ctest
// rl_mod_capture_roundtrip).
//
// Synthetic CaptureData (static and animated variants: meshes with normals / texcoords / colours / categories,
// a skinned mesh with its skeleton, materials, instances incl. a sky instance, sphere + distant lights, the
// camera) -> capture::exporter::writeRemixUsda -> in-memory file system -> readStage. Everything the writer put
// in must come back out of the composed stage: the Remix classification (mesh_ / mat_ / light_ hashes), mesh
// buffers, material bindings and textures (anchored to the materials/ layer), instance transforms and
// visibility samples, lights, skeleton joints, the per-mesh customLayerData hash components, categories, the
// stage metadata. The composition must be clean (no errors, no warnings).
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <fuse/relight/capture/export/capture_model.hpp>
#include <fuse/relight/capture/export/usda_writer.hpp>
#include <fuse/relight/hash/hash_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

namespace usd = fuse::relight::mods::usd;
namespace ex = fuse::relight::capture::exporter;
namespace hash = fuse::relight::hash;
namespace scene = fuse::relight::scene;

ex::Mat4d translation(double x, double y, double z) {
    ex::Mat4d m = ex::identity4d();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

ex::CaptureData buildCapture(std::size_t frames) {
    ex::CaptureData c;
    c.meta.windowTitle = "Round Trip \"Game\"";
    c.meta.exeName = "game.exe";
    c.meta.geometryHashRule = "positions,indices,geometrydescriptor";
    c.meta.stageName = "capture";
    c.meta.metersPerUnit = 0.0254;
    c.meta.numFramesCaptured = frames;
    c.meta.endTimeCode = frames > 1 ? double(frames - 1) : 0.0;

    for (hash::Hash64 h : {hash::Hash64(0x1111222233334444ull), hash::Hash64(0x00000000000000ABull)}) {
        ex::CaptureMaterial m;
        m.hash = h;
        m.albedoTexture = h;
        m.enableOpacity = h == 0x00000000000000ABull;
        m.wrapU = ex::mdl::kWrapClamp;
        c.materials[h] = m;
    }

    ex::CaptureMesh a;
    a.hash = 0xA1B2C3D4E5F60718ull;
    for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
        a.components[static_cast<hash::HashComponent>(i)] = 0x0102030405060700ull + i;
    }
    a.points = {{0.f, 0.f, 0.f}, {1.5f, 0.f, 0.f}, {0.f, 2.25f, 0.f}, {0.1f, 0.2f, 0.3f}};
    a.normals = {{0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}};
    a.texcoords = {{0.f, 1.f}, {1.f, 1.f}, {0.f, 0.f}, {0.33f, 0.66f}};
    a.colors = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 0.5f}, {0.f, 0.f, 1.f, 0.25f}, {1.f, 1.f, 1.f, 1.f}};
    a.indices = {0, 1, 2, 2, 1, 3};
    a.materialHash = 0x1111222233334444ull;
    a.isDoubleSided = true;
    a.categories.set(scene::InstanceCategories::Sky);
    a.categories.set(scene::InstanceCategories::HairCards);
    c.meshes[a.hash] = a;

    ex::CaptureMesh b;
    b.hash = 0x0000000000000042ull;
    b.points = {{-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {0.f, 1.f, 0.f}};
    b.indices = {0, 1, 2};
    c.meshes[b.hash] = b;

    ex::CaptureMesh s; // skinned
    s.hash = 0x5EED5EED5EED5EEDull;
    s.points = {{0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}};
    s.indices = {0, 1, 2};
    s.numBones = 2;
    s.bonesPerVertex = 1;
    s.jointIndices = {0, 1, 1};
    s.jointWeights = {1.f, 1.f, 1.f};
    c.meshes[s.hash] = s;
    ex::CaptureSkeleton skel;
    skel.jointNames = {"joint0", "joint0/joint1"};
    skel.bindPose = {ex::identity4d(), translation(0, 1, 0)};
    skel.restPose = {ex::identity4d(), translation(0, 1, 0)};
    c.skeletons[s.hash] = skel;

    auto instance = [&](std::uint64_t id, hash::Hash64 mesh, hash::Hash64 mat, std::uint32_t num, bool sky) {
        ex::CaptureInstance i;
        i.id = id;
        i.mesh = mesh;
        i.material = mat;
        i.meshInstNum = num;
        i.isSky = sky;
        i.firstTime = 0.0;
        i.finalTime = frames > 1 ? double(frames - 1) : 0.0;
        for (std::size_t f = 0; f < std::max<std::size_t>(frames, 1); ++f) {
            i.xforms.push_back({double(f), translation(double(id), 0.5 * double(f), -2.0)});
        }
        c.instances[id] = i;
    };
    instance(1, a.hash, a.materialHash, 0, false);
    instance(2, a.hash, a.materialHash, 1, false);
    instance(3, b.hash, 0, 0, true);
    instance(4, s.hash, 0, 0, false);
    c.instances[4].boneXforms.push_back({0.0, {ex::identity4d(), ex::identity4d()}});
    if (frames > 1) {
        c.instances[4].boneXforms.push_back({1.0, {ex::identity4d(), ex::identity4d()}});
    }

    ex::CaptureSphereLight sl;
    sl.hash = 0xFEEDFACECAFEBEEFull;
    sl.color = {1.f, 0.5f, 0.25f};
    sl.radius = 3.f;
    sl.intensity = 1200.f;
    sl.shapingEnabled = true;
    sl.coneAngleDegrees = 30.f;
    sl.coneSoftness = 0.1f;
    sl.focusExponent = 2.f;
    sl.finalTime = frames > 1 ? double(frames - 1) : 0.0;
    sl.xforms.push_back({0.0, translation(4, 5, 6)});
    c.sphereLights[sl.hash] = sl;

    ex::CaptureDistantLight dl;
    dl.hash = 0x0D15EA5E0D15EA5Eull;
    dl.color = {1.f, 1.f, 0.9f};
    dl.intensity = 2.f;
    dl.angleDegrees = 0.5f;
    dl.direction = {0.f, -1.f, 0.f};
    dl.finalTime = sl.finalTime;
    c.distantLights[dl.hash] = dl;

    c.camera.valid = true;
    c.camera.fov = 1.0f;
    c.camera.aspectRatio = 16.f / 9.f;
    c.camera.nearPlane = 0.1f;
    c.camera.farPlane = 1000.f;
    c.camera.xforms.push_back({0.0, translation(0, 1, 10)});
    return c;
}

std::vector<float> floats(const usd::Value& v) {
    std::vector<float> out;
    std::function<void(const usd::Value&)> walk = [&](const usd::Value& x) {
        if (x.kind == usd::Value::Kind::Number) {
            out.push_back(float(x.number));
        }
        for (const usd::Value& i : x.items) {
            walk(i);
        }
    };
    walk(v);
    return out;
}

template <typename V>
std::vector<float> flat(const std::vector<V>& v) {
    std::vector<float> out;
    for (const V& e : v) {
        for (float f : e) {
            out.push_back(f);
        }
    }
    return out;
}

const usd::Value* attr(const usd::ComposedStage& s, const std::string& prim, const std::string& name) {
    const usd::Prim* p = s.find(prim);
    if (!p) {
        std::fprintf(stderr, "  missing prim %s\n", prim.c_str());
        return nullptr;
    }
    const usd::Attribute* a = p->attribute(name);
    if (!a || !a->hasDefault) {
        std::fprintf(stderr, "  missing attribute %s.%s\n", prim.c_str(), name.c_str());
        return nullptr;
    }
    return &a->defaultValue.get();
}

void roundTrip(std::size_t frames) {
    const ex::CaptureData c = buildCapture(frames);
    std::map<std::string, std::string> files;
    for (auto& [rel, text] : ex::writeRemixUsda(c)) {
        files["/capture_root/" + rel] = text;
    }
    usd::ReadOptions opt;
    opt.files = usd::memoryFileSource(files);
    const usd::ComposedStage s = usd::readStage("/capture_root/capture.usda", opt);
    CHECK(s.ok);
    CHECK(s.count(usd::Diagnostic::Severity::Error) == 0);
    CHECK(s.count(usd::Diagnostic::Severity::Warning) == 0);
    for (const usd::Diagnostic& d : s.diagnostics) {
        std::fprintf(stderr, "  diagnostic %s %s: %s\n", d.code.c_str(), d.primPath.c_str(), d.message.c_str());
    }
    CHECK(s.layers.size() == files.size()); // every written layer is reachable from the stage
    CHECK(s.defaultPrim == "RootNode");

    // Stage metadata and customLayerData.
    const usd::Value* rule = s.customLayerData.get("lightspeed_geometry_hash_rules");
    CHECK(rule && rule->text == c.meta.geometryHashRule);
    const usd::Value* title = s.customLayerData.get("lightspeed_game_name");
    CHECK(title && title->text == c.meta.windowTitle);
    const usd::Value* layerType = s.customLayerData.get("lightspeed_layer_type");
    CHECK(layerType && layerType->text == "capture");
    const usd::Value* camSettings = s.customLayerData.get("cameraSettings");
    CHECK(camSettings && camSettings->get("boundCamera") && camSettings->get("boundCamera")->text == "/RootNode/cameras/Camera");
    bool mpu = false;
    for (const auto& [k, v] : s.layerMetadata) {
        mpu = mpu || (k == "metersPerUnit" && v.asNumber() && std::fabs(*v.asNumber() - c.meta.metersPerUnit) < 1e-12);
    }
    CHECK(mpu);

    // Remix view: one mesh / material / light replacement per written asset, hashes from the prim names.
    const usd::RemixMod mod = usd::collectRemixMod(s);
    CHECK(mod.meshes.size() == c.meshes.size());
    CHECK(mod.materials.size() == c.materials.size());
    CHECK(mod.lights.size() == c.sphereLights.size() + c.distantLights.size());
    CHECK(mod.diagnostics.empty());
    for (const usd::MeshReplacement& m : mod.meshes) {
        CHECK(c.meshes.count(m.hash) == 1);
    }
    for (const usd::MaterialReplacement& m : mod.materials) {
        CHECK(c.materials.count(m.hash) == 1);
        CHECK(m.hashFromName);
        CHECK(m.mdlSourceAsset == "./AperturePBR_Opacity.mdl");
        CHECK(m.mdlSubIdentifier == "AperturePBR_Opacity");
    }
    for (const usd::LightReplacement& l : mod.lights) {
        CHECK((c.sphereLights.count(l.hash) == 1 && l.type == "SphereLight") ||
              (c.distantLights.count(l.hash) == 1 && l.type == "DistantLight"));
    }

    // Mesh prototypes: buffers through /RootNode/meshes/mesh_<H> -> meshes/mesh_<H>.usda</mesh_<H>>.
    for (const auto& [h, mesh] : c.meshes) {
        const std::string root = "/RootNode/meshes/" + hash::primName(hash::prim_prefix::kMesh, h);
        const std::string geo = root + "/mesh";
        const usd::Prim* rp = s.find(root);
        CHECK(rp && rp->typeName == (mesh.numBones > 0 ? "SkelRoot" : "Xform"));
        const usd::Classification cls = usd::classifyPrimPath(root);
        CHECK(cls.cls == usd::PrimClass::Mesh && cls.hash == h);
        if (const usd::Value* v = attr(s, geo, "points")) {
            CHECK(floats(*v) == flat(mesh.points));
        }
        if (const usd::Value* v = attr(s, geo, "faceVertexIndices")) {
            std::vector<float> idx(mesh.indices.begin(), mesh.indices.end());
            CHECK(floats(*v) == idx);
        }
        if (!mesh.normals.empty()) {
            const usd::Value* v = attr(s, geo, "normals");
            CHECK(v && floats(*v) == flat(mesh.normals));
        }
        if (!mesh.texcoords.empty()) {
            const usd::Value* v = attr(s, geo, "primvars:st");
            CHECK(v && floats(*v) == flat(mesh.texcoords));
        }
        if (!mesh.colors.empty()) {
            const usd::Value* v = attr(s, geo, "primvars:displayOpacity");
            std::vector<float> alpha;
            for (const auto& col : mesh.colors) {
                alpha.push_back(col[3]);
            }
            CHECK(v && floats(*v) == alpha);
        }
        if (const usd::Value* v = attr(s, geo, "doubleSided")) {
            CHECK(v->asBool() == mesh.isDoubleSided);
        }
        if (const usd::Prim* g = s.find(geo)) {
            const usd::CategoryOverrides cat = usd::readCategories(*g);
            CHECK(cat.flags == mesh.categories);
            // Every category but hair cards (absent when false) is authored.
            CHECK(cat.exists.test(scene::InstanceCategories::Sky));
            CHECK(cat.exists.test(scene::InstanceCategories::HairCards) == mesh.categories.test(scene::InstanceCategories::HairCards));
        }
        if (mesh.materialHash != 0) {
            const usd::Relationship* r = rp ? rp->relationship("material:binding") : nullptr;
            CHECK((r && r->targets == std::vector<std::string>{"/RootNode/Looks/" + hash::primName(hash::prim_prefix::kMaterial, mesh.materialHash)}));
        }
        // The mesh layer's customLayerData carries the nine hash components.
        const std::string layerPath = "/capture_root/meshes/" + hash::primName(hash::prim_prefix::kMesh, h) + ".usda";
        std::string err;
        const auto layer = usd::loadLayer(layerPath, files.at(layerPath), &err);
        CHECK(layer.has_value() && err.empty());
        if (layer) {
            for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
                const auto comp = static_cast<hash::HashComponent>(i);
                const usd::Value* v = layer->customLayerData.get(hash::hashComponentName(comp));
                CHECK(v && v->kind == usd::Value::Kind::Number && v->text == std::to_string(mesh.components[comp]));
            }
        }
    }

    // Materials: shader inputs, texture anchored to the material layer's directory.
    for (const auto& [h, m] : c.materials) {
        const std::string shader = "/RootNode/Looks/" + hash::primName(hash::prim_prefix::kMaterial, h) + "/Shader";
        if (const usd::Value* v = attr(s, shader, "inputs:diffuse_texture")) {
            CHECK(v->kind == usd::Value::Kind::Asset);
            CHECK(v->resolved == "/capture_root/textures/" + hash::hashToString(h) + ".dds");
        }
        if (const usd::Value* v = attr(s, shader, "enable_opacity")) {
            CHECK(v->asBool() == m.enableOpacity);
        }
        if (const usd::Value* v = attr(s, shader, "wrap_mode_u")) {
            CHECK(v->asNumber() == double(m.wrapU));
        }
        const usd::Prim* mat = s.find("/RootNode/Looks/" + hash::primName(hash::prim_prefix::kMaterial, h));
        const usd::Attribute* out = mat ? mat->attribute("outputs:mdl:surface") : nullptr;
        CHECK((out && out->connections == std::vector<std::string>{shader + ".outputs:out"}));
    }

    // Instances: internal references to the prototypes, transforms, visibility, material bindings.
    for (const auto& [id, inst] : c.instances) {
        const std::string path = "/RootNode/instances/" + inst.primName();
        const usd::Prim* p = s.find(path);
        CHECK(p != nullptr);
        if (!p) {
            continue;
        }
        const ex::CaptureMesh& mesh = c.meshes.at(inst.mesh);
        if (const usd::Value* v = attr(s, path + "/mesh", "points")) {
            CHECK(floats(*v) == flat(mesh.points)); // composed through the internal and the external reference
        }
        const usd::Attribute* xf = p->attribute("xformOp:transform");
        CHECK(xf != nullptr);
        if (xf) {
            if (frames > 1) {
                CHECK(xf->timeSamples.size() == inst.xforms.size());
                for (std::size_t i = 0; i < xf->timeSamples.size() && i < inst.xforms.size(); ++i) {
                    CHECK(xf->timeSamples[i].first == inst.xforms[i].time);
                    const std::vector<float> m = floats(xf->timeSamples[i].second);
                    CHECK(m.size() == 16 && m[12] == float(inst.xforms[i].xform[12]) && m[13] == float(inst.xforms[i].xform[13]));
                }
            } else {
                CHECK(xf->hasDefault && floats(xf->defaultValue).size() == 16);
            }
        }
        const usd::Attribute* vis = p->attribute("visibility");
        CHECK(vis && vis->hasDefault && vis->defaultValue->text == (inst.isSky ? "invisible" : "inherited"));
        if (frames > 1) {
            CHECK(vis && !vis->timeSamples.empty());
        }
        if (inst.material != 0) {
            const usd::Relationship* r = p->relationship("material:binding");
            CHECK((r && r->targets == std::vector<std::string>{"/RootNode/Looks/" + hash::primName(hash::prim_prefix::kMaterial, inst.material)}));
        }
        if (!inst.boneXforms.empty()) {
            const usd::Prim* pose = s.find(path + "/pose");
            CHECK(pose && pose->typeName == "SkelAnimation");
            const usd::Prim* sk = s.find(path + "/skel");
            const usd::Relationship* src = sk ? sk->relationship("skel:animationSource") : nullptr;
            CHECK((src && src->targets == std::vector<std::string>{path + "/pose"}));
            const usd::Value* joints = sk ? attr(s, path + "/skel", "joints") : nullptr;
            CHECK(joints && joints->items.size() == c.skeletons.at(inst.mesh).jointNames.size());
        } else {
            if (const usd::Value* v = attr(s, path + "/mesh", "primvars:_remix_metadata:isVertexColorBakedLighting")) {
                CHECK(v->asBool() == inst.metadata.isVertexColorBakedLighting);
            }
        }
    }

    // Lights.
    for (const auto& [h, l] : c.sphereLights) {
        const std::string path = "/RootNode/lights/" + hash::primName(hash::prim_prefix::kLight, h);
        const usd::Prim* p = s.find(path);
        CHECK(p && p->typeName == "SphereLight" && p->hasApiSchema("ShapingAPI"));
        if (const usd::Value* v = attr(s, path, "inputs:radius")) {
            CHECK(v->asNumber() == double(l.radius));
        }
        if (const usd::Value* v = attr(s, path, "inputs:shaping:cone:angle")) {
            CHECK(v->asNumber() == double(l.coneAngleDegrees));
        }
        if (const usd::Value* v = attr(s, path, "inputs:intensity")) {
            CHECK(v->asNumber() == double(l.intensity));
        }
        const usd::Attribute* in = p ? p->attribute("inputs:intensity") : nullptr;
        CHECK(in && (frames > 1) == !in->timeSamples.empty());
        CHECK(in && in->definingLayer == "/capture_root/lights/" + hash::primName(hash::prim_prefix::kLight, h) + ".usda");
    }
    for (const auto& [h, l] : c.distantLights) {
        const std::string path = "/RootNode/lights/" + hash::primName(hash::prim_prefix::kLight, h);
        if (const usd::Value* v = attr(s, path, "inputs:angle")) {
            CHECK(v->asNumber() == double(l.angleDegrees));
        }
    }

    // Skeleton prototype.
    for (const auto& [h, skel] : c.skeletons) {
        const std::string path = "/RootNode/meshes/" + hash::primName(hash::prim_prefix::kMesh, h) + "/skel";
        const usd::Prim* p = s.find(path);
        CHECK(p && p->typeName == "Skeleton");
        if (const usd::Value* v = attr(s, path, "joints")) {
            CHECK(v->items.size() == skel.jointNames.size() && v->items.size() == 2 && v->items[1].text == skel.jointNames[1]);
        }
    }

    // Camera.
    const usd::Prim* cam = s.find("/RootNode/cameras/Camera");
    CHECK(cam && cam->typeName == "Camera" && cam->attribute("focalLength") != nullptr);

    // Deterministic dump.
    CHECK(usd::canonicalDump(s) == usd::canonicalDump(usd::readStage("/capture_root/capture.usda", opt)));
}

void categoryNames() {
    for (std::uint32_t i = 0; i < scene::kInstanceCategoryCount; ++i) {
        const auto cat = static_cast<scene::InstanceCategories>(i);
        CHECK(std::string(usd::remixCategoryAttributeName(cat)) == ex::remixCategoryAttribute(cat));
    }
}

} // namespace

int main() {
    categoryNames();
    roundTrip(1);
    roundTrip(3);
    std::printf("rl_mod_capture_roundtrip: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
