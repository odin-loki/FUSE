// FUSE Relight RL-4.1 CPU tests: options, the bindless registration semantics (CPU heap), the GPU-scene adapter
// (CPU mirror of gpu_scene::GpuScene) and the passthrough-swap eligibility. Any platform (Wine in the MinGW tree).
//   fuse_relight_frame_tests <suite>   suites: options, bindless, scene, swap, all
#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/frame_options.hpp>
#include <fuse/relight/render/frame/frame_orchestrator.hpp>
#include <fuse/relight/render/frame/scene_adapter.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace rf = fuse::relight::render::frame;
namespace rt = fuse::relight::tap;
namespace rr = fuse::renderer;
namespace gs = fuse::renderer::gpu_scene;
namespace inst = fuse::relight::scene::instances;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

void testOptions() {
    rf::FrameMode m = rf::FrameMode::Off;
    check(rf::parseFrameMode("Passthrough", m) && m == rf::FrameMode::Passthrough, "mode passthrough");
    check(rf::parseFrameMode("SOLID", m) && m == rf::FrameMode::Solid, "mode solid");
    check(rf::parseFrameMode("0", m) && m == rf::FrameMode::Off, "mode 0 = off");
    check(!rf::parseFrameMode("raytrace", m) && m == rf::FrameMode::Off, "unknown mode rejected, value kept");
    check(std::strcmp(rf::frameModeName(rf::FrameMode::Solid), "solid") == 0, "mode name");
    std::uint32_t c = 0;
    check(rf::parseRgbHex("2050d0", c) && c == 0x2050d0u, "rgb hex");
    check(rf::parseRgbHex("#FFa001", c) && c == 0xffa001u, "rgb # prefix");
    check(rf::parseRgbHex("0x000102", c) && c == 0x000102u, "rgb 0x prefix");
    check(!rf::parseRgbHex("12345", c) && !rf::parseRgbHex("12345g", c), "bad rgb rejected");
    rf::FrameConfig cfg;
    check(!cfg.enabled(), "default config does nothing");
    cfg.textureSwap = true;
    check(cfg.enabled(), "texture swap alone enables the frame tap");
    rf::registerFrameOptions();
    const rf::FrameConfig fromOptions = rf::FrameConfig::fromOptions();
    check(fromOptions.mode == rf::FrameMode::Off && !fromOptions.textureSwap && fromOptions.injectAtUi &&
              fromOptions.solidColor == 0x2050d0u,
          "option defaults");
}

void testBindless() {
    rf::CpuBindlessHeap heap;
    rf::BindlessImageRegistry reg(heap, nullptr);
    rf::ExternalImageDesc a;
    a.texture = 1;
    a.vkImage = 0xA000;
    a.vkFormat = 44;
    rf::ExternalImageDesc b = a;
    b.texture = 2;
    b.vkImage = 0xB000;
    b.owned = true;
    const rr::BindlessSlotHandle ha = reg.registerImage(a, 1);
    const rr::BindlessSlotHandle hb = reg.registerImage(b, 1);
    check(ha.isValid() && hb.isValid() && ha.index != hb.index, "two slots");
    check(reg.registerImage(a, 1) == ha, "same texture + image keeps its slot");
    const std::uint32_t sa = reg.shaderHandle(1);
    check(sa != 0 && rr::bindlessShaderHandleType(sa) == rr::BindlessResourceType::SampledImage &&
              rr::bindlessShaderHandleIndex(sa) == ha.index,
          "32-bit shader handle (WP-0.4 layout)");
    check(reg.stats().external == 1 && reg.stats().owned == 1 && reg.stats().live == 2, "external / owned counts");
    check(reg.registerImage(rf::ExternalImageDesc{}, 1).isValid() == false, "no image: no slot");

    // DXVK recreated texture 1's image: the old slot retires at the given serial.
    a.vkImage = 0xA001;
    const rr::BindlessSlotHandle ha2 = reg.registerImage(a, 5);
    check(ha2.isValid() && !heap.validate(ha), "re-registration retires the old slot");
    check(reg.shaderHandle(1) != sa, "new handle");

    // onImageDestroy at serial 7: stale at once, index reusable only after serial 7 completed.
    check(reg.release(1, 7), "release");
    check(!heap.validate(ha2) && reg.shaderHandle(1) == 0, "released handle stale at once");
    check(reg.collect(4) == 0, "nothing reclaimed before serial 5");
    check(reg.collect(5) == 1, "slot of serial 5 reclaimed");
    check(reg.collect(6) == 0 && reg.collect(7) == 1, "slot of serial 7 reclaimed at 7");
    const rr::BindlessSlotHandle reused = reg.registerImage(a, 8);
    check(reused.isValid() && (reused.index == ha.index || reused.index == ha2.index) &&
              reused.generation != ha.generation,
          "reclaimed index reused with a new generation");
    check(!reg.release(99, 1), "unknown texture");
    reg.releaseAll();
    check(reg.stats().live == 0 && reg.stats().retired == 0, "releaseAll");
}

void testScene() {
    gs::GpuScene scene;
    gs::GpuSceneDesc desc;
    check(scene.init(desc), "GpuScene CPU mirror");
    std::uint32_t lookups = 0;
    rf::GpuSceneAdapter adapter(scene, [&](rt::ResourceId id) {
        ++lookups;
        return id == 7 ? rr::packBindlessShaderHandle(rr::BindlessResourceType::SampledImage, 3, 1) : 0u;
    });

    rf::AdapterDraw d1;
    d1.instanceId = 100;
    d1.blasId = 11;
    d1.created = true;
    d1.objectToWorld = inst::identityMatrix();
    d1.objectToWorld[12] = 1.f;
    d1.bounds.minPos = {-1, -1, -1};
    d1.bounds.maxPos = {1, 1, 1};
    d1.materialHash = 0x1234;
    d1.colorTexture = 7;
    rf::AdapterDraw d2 = d1;
    d2.instanceId = 200;
    d2.blasId = 12;
    d2.materialHash = 0;
    d2.colorTexture = rt::kNoResource;
    rf::AdapterDraw d3 = d1; // same BLAS and material as d1, other instance
    d3.instanceId = 300;

    std::vector<fuse::relight::scene::LightRecord> lights(2);
    lights[0].radiance = {2.f, 1.f, 0.f};
    lights[0].radius = 3.f;
    lights[0].hash = 1;
    lights[1].type = fuse::relight::hash::LightType::Distant;
    lights[1].radiance = {1.f, 1.f, 1.f};
    lights[1].hash = 2;

    adapter.beginFrame(1);
    adapter.submit(d1);
    adapter.submit(d2);
    adapter.submit(d3);
    adapter.submitLights(rf::adapterLights(lights));
    adapter.endFrame();
    rf::AdapterStats s = adapter.stats();
    check(s.instances == 3 && s.added == 3 && s.removed == 0, "frame 1: three instances");
    check(s.meshes == 2 && s.materials == 2 && s.texturedMaterials == 1, "shared mesh / material rows");
    check(s.lights == 2, "two lights");
    const gs::InstanceHandle h1 = adapter.instanceOf(100);
    check(h1.valid() && scene.transform(h1.slot).rows[0][3] == 1.f, "transform (translation in the last column)");
    check(scene.prevTransform(h1.slot).rows[0][3] == 1.f, "new instance: previous transform = current");
    const std::uint32_t mat = scene.instance(h1.slot).material;
    check(mat == adapter.materialOf(0x1234), "instance material row");
    check(scene.instance(h1.slot).mesh == adapter.meshOf(11), "instance mesh row");
    check(lookups >= 1, "bindless handle looked up for the colour texture");

    // Frame 2: instance 100 moves, 200 disappears, one light remains.
    d1.created = false;
    d1.objectToWorld[12] = 2.f;
    adapter.beginFrame(2);
    adapter.submit(d1);
    adapter.submit(d3);
    lights.pop_back();
    adapter.submitLights(rf::adapterLights(lights));
    adapter.endFrame();
    s = adapter.stats();
    check(s.instances == 2 && s.removed == 1 && s.moved == 1 && s.added == 0, "frame 2: moved / removed");
    check(scene.transform(h1.slot).rows[0][3] == 2.f && scene.prevTransform(h1.slot).rows[0][3] == 1.f,
          "motion: current and previous transforms");
    check(!adapter.instanceOf(200).valid(), "removed instance gone");
    check(s.lights == 1, "light removed");

    // Frame 3: a mesh replacement hides the original draw and places two parts.
    fuse::relight::replace::ReplacedDraw rd;
    rd.meshReplaced = true;
    rd.drawOriginal = false;
    rd.meshMod = "mod";
    rd.parts.resize(2);
    rd.parts[0].meshId = "a";
    rd.parts[1].meshId = "b";
    rd.parts[1].material = "mat_x";
    rd.parts[1].materialMod = "mod";
    d1.replaced = &rd;
    adapter.beginFrame(3);
    adapter.submit(d1);
    adapter.submit(d3);
    adapter.endFrame();
    s = adapter.stats();
    check(s.hiddenOriginals == 1 && s.replacementParts == 2, "replacement: original hidden, two parts");
    check(!adapter.instanceOf(100, 0).valid() && adapter.instanceOf(100, 1).valid() && adapter.instanceOf(100, 2).valid(),
          "part instances replace the original");
    check(s.instances == 3, "parts + the other instance");
    adapter.clear();
    check(scene.liveInstances() == 0, "clear removes every instance");
    scene.destroy();
}

void testSwap() {
    rt::TextureDesc t;
    t.id = 1;
    t.type = 3;
    t.vkImage = 0x1;
    t.pool = 1;
    check(rf::FrameOrchestrator::swappable(t), "managed texture is swappable");
    rt::TextureDesc rtTex = t;
    rtTex.usage = 0x1; // D3DUSAGE_RENDERTARGET
    check(!rf::FrameOrchestrator::swappable(rtTex), "render target is not");
    rt::TextureDesc sys = t;
    sys.pool = 2; // D3DPOOL_SYSTEMMEM
    check(!rf::FrameOrchestrator::swappable(sys), "system-memory texture is not");
    rt::TextureDesc surf = t;
    surf.type = 1; // surface
    check(!rf::FrameOrchestrator::swappable(surf), "surface is not");
    rt::TextureDesc noImage = t;
    noImage.vkImage = 0;
    check(!rf::FrameOrchestrator::swappable(noImage), "no image, no swap");
    check(rf::swapTwinId(5) != 5 && rf::swapTwinId(5) != rf::kFrameOutputId, "twin ids distinct");

    rf::FrameConfig cfg;
    cfg.mode = rf::FrameMode::Solid;
    rf::FrameOrchestrator orch(cfg, nullptr);
    check(!orch.attach(nullptr) && !orch.attached(), "no host: inert");
    const rf::InjectResult r = orch.inject();
    check(!r.injected && !r.error.empty(), "inject without a host does nothing");
    check(!orch.swapTexture(t), "swap without a host does nothing");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "options") {
        testOptions();
    }
    if (all || suite == "bindless") {
        testBindless();
    }
    if (all || suite == "scene") {
        testScene();
    }
    if (all || suite == "swap") {
        testSwap();
    }
    if (g_failures) {
        std::printf("FAIL: %s: %d check(s)\n", suite.c_str(), g_failures);
        return 1;
    }
    std::printf("PASS: %s\n", suite.c_str());
    return 0;
}
