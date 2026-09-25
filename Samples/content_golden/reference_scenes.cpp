// FUSE asset plan W0.8: deterministic reference scenes (see reference_scenes.hpp).
#include "reference_scenes.hpp"

namespace fuse::content_golden {

using math::Vec3;

std::vector<LightingSetup> lightingSetups() {
    return {
        {"sun", Vec3(0.45f, 0.80f, 0.40f), Vec3(0.30f, 0.45f, 0.70f)},
        {"overcast", Vec3(0.05f, 1.00f, 0.05f), Vec3(0.55f, 0.57f, 0.60f)},
        {"interior", Vec3(-0.85f, 0.25f, 0.30f), Vec3(0.015f, 0.012f, 0.010f)},
    };
}

renderer::harness::Scene buildMaterialBallGrid() {
    // 5 x 5 balls, 10 stacks x 16 slices: ~4k flat-shaded batches, a few seconds on Lavapipe.
    renderer::harness::Scene s = renderer::harness::buildSphereField(5u, 10u, 16u);
    s.name = "material_balls";
    // 16:9 framing of the whole grid, looking slightly down.
    s.camera.eye = Vec3(0.f, 3.6f, 6.4f);
    s.camera.target = Vec3(0.f, 0.2f, 0.f);
    s.camera.fovYDegrees = 40.f;
    return s;
}

std::vector<std::string> referenceSceneNames() { return {"material_balls"}; }

} // namespace fuse::content_golden
