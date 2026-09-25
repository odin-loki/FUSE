// B6.7 / B6.10 / B6.13 gates — material panel edits reach the shaded result (roughness /
// metallic response, sRGB colour picker, procedural switch), survive a save / load cycle, and the
// profiler flame-graph model consumes real `fuse::profiler` scope events.
#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/material_library_io.hpp>
#include <fuse/editor/material_property_inspect.hpp>
#include <fuse/editor/profiler_flame_graph.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry_serialiser.hpp>
#include <fuse/profiler/profiler.hpp>
#include <fuse/renderer/material/brdf.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/material/procedural_materials.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/resource_manager.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec3;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool near(f32 a, f32 b, f32 eps) {
    return std::fabs(a - b) <= eps;
}

Vec3 unit(f32 x, f32 y, f32 z) {
    const f32 len = std::sqrt(x * x + y * y + z * z);
    return {x / len, y / len, z / len};
}

/// Material system on a resource manager without a device: CPU rows (the SSBO contents the
/// shaders index) are packed by `flushGpuBuffer`; no GPU buffer is created.
struct MaterialFixture {
    fuse::renderer::ResourceManager resources;
    fuse::renderer::MaterialSystem materials;
    fuse::editor::EditorState state;
    fuse::editor::CommandStack cmds;
    fuse::editor::MaterialEditorPanel panel;

    explicit MaterialFixture(u32 count) {
        materials.init(resources);
        for (u32 i = 0; i < count; ++i) {
            fuse::renderer::Material material{};
            material.baseColor = {0.9f, 0.02f, 0.02f};
            materials.registerMaterial(material);
        }
        panel.syncFromMaterialSystem(state, materials);
    }
    ~MaterialFixture() { materials.destroy(); }

    /// Panel -> MaterialSystem -> packed SSBO row -> shader-side material sample (CPU reference).
    fuse::renderer::MaterialSample shadeInput(const Vec3& worldPos = {0.f, 0.f, 0.f}) {
        panel.pushToMaterialSystem(materials, cmds);
        materials.flushGpuBuffer();
        return fuse::renderer::MaterialEval::sample(materials.gpuMaterials()[panel.selectedMaterialId()], worldPos);
    }
};

Vec3 shade(const fuse::renderer::MaterialSample& s, const Vec3& n, const Vec3& v, const Vec3& l) {
    return fuse::renderer::Brdf::shade(s.albedo, s.roughness, s.metallic, n, v, l);
}

// ---------------------------------------------------------------------------------------------
// Row 5303: roughness / metallic sliders produce the correct (monotonic) visual change.
void testRoughnessMetallicResponse() {
    MaterialFixture fx(1u);
    const Vec3 n{0.f, 0.f, 1.f};
    const Vec3 mirrorV = unit(0.5f, 0.f, 1.f);
    const Vec3 mirrorL = unit(-0.5f, 0.f, 1.f); // perfect reflection of V: the highlight peak
    const Vec3 offL = unit(-1.6f, 0.f, 1.f);    // 30-ish degrees off the peak

    // Roughness sweep (metallic 0): the peak falls monotonically, the lobe widens (off / peak rises).
    fx.panel.setMetallic(0.f, fx.cmds);
    f32 previousPeak = 1e30f;
    f32 previousRatio = -1.f;
    bool peakFalls = true;
    bool lobeWidens = true;
    for (int i = 1; i <= 10; ++i) {
        const f32 roughness = 0.1f * static_cast<f32>(i);
        expectTrue(fx.panel.setRoughness(roughness, fx.cmds), "roughness slider edit accepted");
        const fuse::renderer::MaterialSample s = fx.shadeInput();
        expectTrue(near(s.roughness, roughness, 1e-6f), "shader row carries the slider roughness");
        const f32 peak = shade(s, n, mirrorV, mirrorL).x;
        peakFalls = peakFalls && peak < previousPeak;
        previousPeak = peak;
        // Lobe width on the specular term alone (remove the roughness-independent Lambert part).
        const auto lambert = [&](const Vec3& l) {
            const Vec3 h = unit(mirrorV.x + l.x, mirrorV.y + l.y, mirrorV.z + l.z);
            const f32 voh = mirrorV.x * h.x + mirrorV.y * h.y + mirrorV.z * h.z;
            const Vec3 f = fuse::renderer::Brdf::fSchlick(voh, fuse::renderer::Brdf::specularF0(s.albedo, s.metallic));
            return (1.f - f.x) * (1.f - s.metallic) / fuse::renderer::Brdf::kPi * s.albedo.x * l.z;
        };
        const f32 lambertPeak = lambert(mirrorL);
        const f32 lambertOff = lambert(offL);
        const f32 ratio = (shade(s, n, mirrorV, offL).x - lambertOff) / (peak - lambertPeak);
        lobeWidens = lobeWidens && ratio > previousRatio;
        previousRatio = ratio;
    }
    expectTrue(peakFalls, "highlight peak decreases strictly with roughness");
    expectTrue(lobeWidens, "highlight spreads (off-peak / peak increases) with roughness");

    // Metallic sweep on a red albedo: specular tint -> albedo, diffuse -> 0.
    fx.panel.setRoughness(0.3f, fx.cmds);
    f32 previousRedPeak = -1.f;
    f32 previousGreen = 1e30f;
    bool redRises = true;
    bool greenFalls = true;
    fuse::renderer::MaterialSample last{};
    for (int i = 0; i <= 10; ++i) {
        const f32 metallic = 0.1f * static_cast<f32>(i);
        expectTrue(fx.panel.setMetallic(metallic, fx.cmds), "metallic slider edit accepted");
        last = fx.shadeInput();
        expectTrue(near(last.metallic, metallic, 1e-6f), "shader row carries the slider metallic");
        const Vec3 peak = shade(last, n, mirrorV, mirrorL);
        redRises = redRises && peak.x > previousRedPeak;
        greenFalls = greenFalls && peak.y < previousGreen;
        previousRedPeak = peak.x;
        previousGreen = peak.y;
    }
    expectTrue(redRises, "red highlight rises with metallic (F0 -> albedo)");
    expectTrue(greenFalls, "green (low albedo channel) falls with metallic (no diffuse, F0 -> 0.02)");
    // Fully metallic at the head-on peak: Fresnel = F0 = albedo and diffuse = 0, so the reflected
    // colour ratio is exactly the albedo ratio.
    const Vec3 headOn = shade(last, n, n, n);
    expectTrue(near(headOn.x / headOn.y, 0.9f / 0.02f, 1e-2f), "metal reflects in its albedo colour");
}

// ---------------------------------------------------------------------------------------------
// Row 5304: colour picker updates base colour with the correct sRGB -> linear conversion.
void testColourPickerSrgb() {
    MaterialFixture fx(1u);
    expectTrue(fx.panel.setBaseColorSrgb(0.5f, 0.2f, 1.f, fx.cmds), "picker edit accepted");
    const fuse::renderer::MaterialSample s = fx.shadeInput();
    // Exact IEC 61966-2-1 values.
    expectTrue(near(s.albedo.x, 0.2140411f, 1e-6f), "sRGB 0.5 -> linear 0.2140 in the shader row");
    expectTrue(near(s.albedo.y, 0.0331048f, 1e-6f), "sRGB 0.2 -> linear 0.0331");
    expectTrue(s.albedo.z == 1.f, "sRGB 1 -> linear 1 exactly");
    // The renderer's output encode (post stack linear -> sRGB) returns the picked colour: no
    // double / missing conversion anywhere on the path.
    const Vec3 displayed = fuse::renderer::linear_to_srgb({s.albedo.x, s.albedo.y, s.albedo.z});
    expectTrue(near(displayed.x, 0.5f, 1e-5f) && near(displayed.y, 0.2f, 1e-5f) && near(displayed.z, 1.f, 1e-6f),
               "display encode of the stored colour reproduces the picked sRGB");
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    fx.panel.baseColorSrgb(r, g, b);
    expectTrue(near(r, 0.5f, 1e-5f) && near(g, 0.2f, 1e-5f) && near(b, 1.f, 1e-6f), "picker swatch round-trips");

    // Linear segment near black and monotonicity over the full range.
    expectTrue(near(fuse::editor::srgbToLinear(0.04f), 0.04f / 12.92f, 1e-7f), "linear toe below 0.04045");
    expectTrue(fuse::editor::srgbToLinear(0.f) == 0.f && fuse::editor::srgbToLinear(1.f) == 1.f, "endpoints exact");
    bool monotonic = true;
    f32 previous = -1.f;
    for (int i = 0; i <= 255; ++i) {
        const f32 value = fuse::editor::srgbToLinear(static_cast<f32>(i) / 255.f);
        monotonic = monotonic && value > previous &&
                    near(fuse::editor::linearToSrgb(value), static_cast<f32>(i) / 255.f, 1e-5f);
        previous = value;
    }
    expectTrue(monotonic, "8-bit picker values convert monotonically and invert");
}

// ---------------------------------------------------------------------------------------------
// Row 5305: procedural switch replaces constant / texture shading.
void testProceduralSwitch() {
    MaterialFixture fx(1u);
    fx.panel.setBaseColor(0.9f, 0.02f, 0.02f, fx.cmds);
    const Vec3 p0{0.1f, 0.2f, 0.3f};
    const Vec3 p1{3.7f, -1.2f, 5.9f};
    fuse::renderer::MaterialSample flat0 = fx.shadeInput(p0);
    const fuse::renderer::Material::GPUMaterial flatRow = fx.materials.gpuMaterials()[0];
    fuse::renderer::MaterialSample flat1 = fuse::renderer::MaterialEval::sample(flatRow, p1);
    expectTrue((flatRow.flags & fuse::renderer::MaterialFlagBits::kProcedural) == 0u, "flat row has no procedural bit");
    expectTrue(flat0.albedo.x == flat1.albedo.x && near(flat0.albedo.x, 0.9f, 1e-6f), "flat shading is constant");

    const u32 wood = static_cast<u32>(fuse::renderer::ProceduralMaterialId::Wood);
    expectTrue(fx.panel.setProceduralMaterial(wood, 7u, fx.cmds), "procedural switch accepted");
    const fuse::renderer::MaterialSample proc0 = fx.shadeInput(p0);
    const fuse::renderer::Material::GPUMaterial procRow = fx.materials.gpuMaterials()[0];
    const fuse::renderer::MaterialSample proc1 = fuse::renderer::MaterialEval::sample(procRow, p1);
    expectTrue((procRow.flags & fuse::renderer::MaterialFlagBits::kProcedural) != 0u &&
                   fuse::renderer::MaterialFlagBits::proceduralId(procRow.flags) == wood && procRow.proceduralSeed == 7u,
               "packed row flags the procedural function and seed");
    const fuse::renderer::MaterialSample expected =
        fuse::renderer::ProceduralMaterials::evaluate(fuse::renderer::ProceduralMaterialId::Wood, p0, 7u);
    expectTrue(proc0.albedo.x == expected.albedo.x && proc0.albedo.y == expected.albedo.y &&
                   proc0.roughness == expected.roughness,
               "shading comes from the procedural function, not the constants");
    expectTrue(proc0.albedo.x != proc1.albedo.x || proc0.albedo.y != proc1.albedo.y || proc0.roughness != proc1.roughness,
               "procedural shading varies over the surface");
    expectTrue(!(near(proc0.albedo.x, 0.9f, 1e-4f) && near(proc0.albedo.y, 0.02f, 1e-4f)),
               "constant base colour no longer drives the result");

    expectTrue(fx.panel.setProceduralMaterial(0u, 0u, fx.cmds), "switch back to constant shading");
    const fuse::renderer::MaterialSample back = fx.shadeInput(p1);
    expectTrue((fx.materials.gpuMaterials()[0].flags & fuse::renderer::MaterialFlagBits::kProcedural) == 0u &&
                   near(back.albedo.x, 0.9f, 1e-6f),
               "switching back restores constant shading");
}

// ---------------------------------------------------------------------------------------------
// Row 5306: material changes persist after a save / load cycle.
void testMaterialSaveLoad() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_b6_material_gate";
    std::filesystem::create_directories(dir);
    const std::string libPath = (dir / "scene.fmatlib").string();
    const std::string scenePath = (dir / "scene.fecs").string();

    MaterialFixture fx(3u);
    // Author material 1 through the panel, material 2 as procedural.
    expectTrue(fx.panel.selectMaterial(1u), "select material 1");
    fx.panel.syncFromMaterialSystem(fx.state, fx.materials); // load material 1's values into the panel
    fx.panel.setRoughness(0.123456789f, fx.cmds);
    fx.panel.setMetallic(0.87654321f, fx.cmds);
    fx.panel.setBaseColorSrgb(0.3f, 0.6f, 0.9f, fx.cmds);
    fx.panel.setShadingModel(static_cast<fuse::u8>(fuse::renderer::ShadingModel::ClearCoat), fx.cmds);
    expectTrue(fx.panel.pushToMaterialSystem(fx.materials, fx.cmds), "push material 1");
    fx.materials.get(1u).parameters.clearCoat.clearCoat = 0.75f;
    fx.materials.get(1u).emissiveColor = {0.1f, 0.2f, 0.3f};
    fx.materials.get(1u).emissiveIntensity = 3.5f;
    fx.materials.updateMaterial(1u, fx.materials.get(1u));
    expectTrue(fx.panel.selectMaterial(2u), "select material 2");
    fx.panel.syncFromMaterialSystem(fx.state, fx.materials);
    fx.panel.setProceduralMaterial(static_cast<u32>(fuse::renderer::ProceduralMaterialId::Concrete), 99u, fx.cmds);
    expectTrue(fx.panel.pushToMaterialSystem(fx.materials, fx.cmds), "push material 2");
    fx.materials.flushGpuBuffer();

    // Scene references the materials by id.
    fuse::editor::EditorScene scene;
    scene.init();
    const fuse::ecs::EntityID meshEntity = scene.registry().create();
    scene.registry().add<fuse::ecs::Transform>(meshEntity);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 1u;
    scene.registry().add(meshEntity, mesh);

    const fuse::editor::MaterialLibraryIoResult saved = fuse::editor::saveMaterialLibrary(fx.materials, libPath);
    expectTrue(saved.ok && saved.materialCount == 3u, "material library saved");
    expectTrue(fuse::ecs::RegistrySerialiser::save(scene.registry(), scenePath).ok, "scene saved");

    // Fresh session: load both.
    fuse::renderer::ResourceManager resources;
    fuse::renderer::MaterialSystem loaded;
    loaded.init(resources);
    const fuse::editor::MaterialLibraryIoResult result = fuse::editor::loadMaterialLibrary(libPath, loaded);
    expectTrue(result.ok && loaded.materialCount() == 3u, "material library loaded");
    fuse::editor::EditorScene reloaded;
    reloaded.init();
    expectTrue(fuse::ecs::RegistrySerialiser::load(scenePath, reloaded.registry()).ok, "scene loaded");
    const fuse::ecs::Mesh* loadedMesh = reloaded.registry().get<fuse::ecs::Mesh>(meshEntity);
    expectTrue(loadedMesh != nullptr && loadedMesh->material_id == 1u, "mesh keeps its material id");

    bool fieldsExact = true;
    for (u32 id = 0; id < 3u; ++id) {
        const fuse::renderer::Material& a = fx.materials.get(id);
        const fuse::renderer::Material& b = loaded.get(id);
        fieldsExact = fieldsExact && a.baseColor.x == b.baseColor.x && a.baseColor.y == b.baseColor.y &&
                      a.baseColor.z == b.baseColor.z && a.roughness == b.roughness && a.metallic == b.metallic &&
                      a.shadingModel == b.shadingModel && a.emissiveIntensity == b.emissiveIntensity &&
                      a.emissiveColor.z == b.emissiveColor.z && a.normalStrength == b.normalStrength &&
                      a.parameters.clearCoat.clearCoat == b.parameters.clearCoat.clearCoat &&
                      a.isProcedural == b.isProcedural && a.proceduralFnId == b.proceduralFnId &&
                      a.proceduralSeed == b.proceduralSeed;
    }
    expectTrue(fieldsExact, "every authored field survives save / load bit for bit");
    loaded.flushGpuBuffer();
    expectTrue(std::memcmp(fx.materials.gpuMaterials().data(), loaded.gpuMaterials().data(),
                           3u * sizeof(fuse::renderer::Material::GPUMaterial)) == 0,
               "packed shader rows identical after reload");

    // The editor panel re-syncs from the loaded system with the edited values.
    fuse::editor::MaterialEditorPanel panel;
    fuse::editor::EditorState state;
    panel.syncFromMaterialSystem(state, loaded);
    panel.selectMaterial(1u);
    panel.syncFromMaterialSystem(state, loaded);
    expectTrue(panel.editState().roughness == 0.123456789f && panel.editState().metallic == 0.87654321f,
               "panel shows the persisted edits");

    // A corrupt file is rejected and leaves the loaded materials untouched.
    {
        std::ofstream bad(libPath, std::ios::trunc);
        bad << "FUSEMATLIB 1\ncount 1\nmaterial 0\nroughness banana\nend\n";
    }
    const f32 before = loaded.get(0u).roughness;
    expectTrue(!fuse::editor::loadMaterialLibrary(libPath, loaded).ok && loaded.get(0u).roughness == before,
               "malformed library rejected without side effects");
    loaded.destroy();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------------------------
// Row 6449: profiler events are consumable by the flame-graph panel model.
void spin(u64 microseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
    while (std::chrono::steady_clock::now() < end) {
    }
}

void profiledFrame() {
    fuse::profiler::ProfileScope frame("Frame");
    {
        fuse::profiler::ProfileScope update("Update");
        spin(300);
        {
            fuse::profiler::ProfileScope physics("Physics");
            spin(500);
        }
    }
    {
        fuse::profiler::ProfileScope render("Render");
        {
            fuse::profiler::ProfileScope gbuffer("GBuffer");
            spin(200);
        }
        {
            fuse::profiler::ProfileScope lighting("Lighting");
            spin(200);
        }
        fuse::profiler::profile_gpu_begin("GpuDeferred", nullptr);
        spin(100);
        fuse::profiler::profile_gpu_end(nullptr);
    }
}

void testProfilerFlameGraph() {
    fuse::profiler::setEnabled(true);
    fuse::profiler::reset();
    for (int frame = 0; frame < 3; ++frame) {
        fuse::profiler::beginFrame();
        profiledFrame();
        fuse::profiler::endFrame();
    }
    std::thread worker([] {
        fuse::profiler::ProfileScope job("WorkerJob");
        spin(200);
    });
    worker.join();

    fuse::editor::ProfilerFlameGraph graph;
    graph.buildFromProfiler();
    expectTrue(graph.unmatchedEndCount() == 0u && graph.openScopeCount() == 0u, "all scopes paired");
    expectTrue(graph.threadCount() == 2u, "main thread and worker are separate tracks");
    // Per frame: Frame, Update, Physics, Render, GBuffer, Lighting, GpuDeferred = 7 slices.
    expectTrue(graph.slices().size() == 3u * 7u + 1u, "one slice per scope instance");
    expectTrue(graph.maxDepth() == 2u, "nesting depth Frame > Update > Physics");

    bool nestedInside = true;
    bool selfConsistent = true;
    for (const fuse::editor::ProfilerSlice& slice : graph.slices()) {
        u64 childSum = 0;
        for (u32 child : slice.children) {
            const fuse::editor::ProfilerSlice& c = graph.slices()[child];
            childSum += c.durationNs;
            nestedInside = nestedInside && c.parent != UINT32_MAX && c.depth == slice.depth + 1u &&
                           c.startNs >= slice.startNs &&
                           c.startNs + c.durationNs <= slice.startNs + slice.durationNs && c.threadId == slice.threadId;
        }
        selfConsistent = selfConsistent && slice.selfNs + childSum == slice.durationNs;
    }
    expectTrue(nestedInside, "children lie inside their parent's time span on the same thread");
    expectTrue(selfConsistent, "self time == duration - children");

    const fuse::editor::FlameNode* frame = graph.findPath({"Frame"});
    const fuse::editor::FlameNode* physics = graph.findPath({"Frame", "Update", "Physics"});
    const fuse::editor::FlameNode* gpu = graph.findPath({"Frame", "Render", "GpuDeferred"});
    const fuse::editor::FlameNode* workerNode = graph.findPath({"WorkerJob"});
    expectTrue(frame != nullptr && frame->callCount == 3u && frame->children.size() == 2u,
               "flame graph merges the three frames under one Frame node with Update + Render");
    expectTrue(physics != nullptr && physics->callCount == 3u && physics->totalNs >= 3u * 500'000u,
               "Physics node aggregates its three calls (>= 3 x 500 us)");
    expectTrue(gpu != nullptr && gpu->totalNs >= 3u * 100'000u, "GPU complete markers become leaf nodes");
    expectTrue(workerNode != nullptr && workerNode->callCount == 1u && workerNode->totalNs >= 200'000u, "worker track root");
    const fuse::editor::FlameNode* update = graph.findPath({"Frame", "Update"});
    expectTrue(update != nullptr && update->totalNs >= physics->totalNs && update->selfNs >= 3u * 300'000u &&
                   update->selfNs + physics->totalNs == update->totalNs,
               "parent total = self + children in the merged graph");
    expectTrue(frame != nullptr && frame->totalNs <= graph.captureEndNs() - graph.captureStartNs(),
               "durations fit inside the capture window");

    // Ring-buffer truncation: an End without its Begin and a still-open scope are reported, not drawn.
    std::vector<fuse::profiler::ProfileEvent> events;
    fuse::profiler::ProfileEvent e{};
    e.threadId = 1u;
    e.name = "Lost";
    e.phase = fuse::profiler::EventPhase::End;
    e.scopeId = 900u;
    e.timestampNs = 10u;
    events.push_back(e);
    e.name = "Root";
    e.phase = fuse::profiler::EventPhase::Begin;
    e.scopeId = 1u;
    e.timestampNs = 20u;
    events.push_back(e);
    e.name = "Child";
    e.scopeId = 2u;
    e.timestampNs = 25u;
    events.push_back(e);
    e.phase = fuse::profiler::EventPhase::End;
    e.timestampNs = 35u;
    events.push_back(e);
    e.name = "Root";
    e.scopeId = 1u;
    e.timestampNs = 60u;
    events.push_back(e);
    e.name = "Open";
    e.phase = fuse::profiler::EventPhase::Begin;
    e.scopeId = 3u;
    e.timestampNs = 70u;
    events.push_back(e);
    fuse::editor::ProfilerFlameGraph partial;
    partial.build(events);
    const fuse::editor::FlameNode* root = partial.findPath({"Root"});
    const fuse::editor::FlameNode* child = partial.findPath({"Root", "Child"});
    expectTrue(partial.unmatchedEndCount() == 1u && partial.openScopeCount() == 1u, "truncation is counted");
    expectTrue(root != nullptr && root->totalNs == 40u && root->selfNs == 30u && child != nullptr && child->totalNs == 10u,
               "explicit capture durations are exact");
    expectTrue(partial.findPath({"Open"}) == nullptr && partial.findPath({"Lost"}) == nullptr,
               "unpaired events are not drawn");
}

} // namespace

int main() {
    fuse::core::initialize();

    testRoughnessMetallicResponse();
    testColourPickerSrgb();
    testProceduralSwitch();
    testMaterialSaveLoad();
    testProfilerFlameGraph();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_material_profiler_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_material_profiler_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
