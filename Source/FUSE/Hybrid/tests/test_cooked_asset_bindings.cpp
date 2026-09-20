#include <fuse/core/init.hpp>
#include <fuse/hybrid/cooked_asset_bindings.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void writeTextFile(const char* path, const char* contents) {
    std::ofstream out(path, std::ios::trunc);
    out << contents;
}

void testCookedAssetBindingsProbeHeaders() {
    writeTextFile("/tmp/fuse_cooked_mat.fusetex", "FUSETEX_STUB\n");
    writeTextFile("/tmp/fuse_cooked_shader.fuseshader", "FUSESHADER_STUB\n");

    fuse::hybrid::CookedAssetBindings bindings;
    bindings.bindMaterial("/tmp/fuse_cooked_mat.fusetex", 3u);
    bindings.bindShader("/tmp/fuse_cooked_shader.fuseshader", 7u);
    bindings.refreshTints();

    expectTrue(bindings.materialCount() == 1u, "material binding recorded");
    expectTrue(bindings.shaderCount() == 1u, "shader binding recorded");
    expectTrue(bindings.validMaterialCount() == 1u, "material header probed");
    expectTrue(bindings.validShaderCount() == 1u, "shader header probed");
    expectTrue(bindings.materialTintR() > 0.f, "material tint applied");
    expectTrue(bindings.shaderTintG() > 0.f, "shader tint applied");
}

void testMeshSdfPreviewCatalogAndComposerDraws() {
    fuse::hybrid::HybridComposer composer;
    fuse::world3d::World3D world3D;
    fuse::hybrid::DimensionFlags flags{};
    flags.enable3D = true;
    composer.setProjectFlags(flags);
    composer.attachWorld3D(&world3D);
    composer.previewCatalog().addMesh({1.f, 2.f, 3.f, "cooked/mesh/preview_1.fusemesh", 1u, true});
    composer.previewCatalog().addSdf(
        {4.f, 5.f, 6.f, fuse::hybrid::SdfPreviewPrimitive::Sphere, 1.f, 0.f, 0.f, 2u, true});

    expectTrue(composer.previewCatalog().meshCount() == 1u, "mesh preview hint stored");
    expectTrue(composer.previewCatalog().sdfCount() == 1u, "sdf preview hint stored");

    fuse::frame::FrameCtx ctx{};
    ctx.frameIndex = 1u;
    composer.render(ctx);

    expectTrue(composer.meshPreviewDraws() == 1u, "mesh preview stub drawn");
    expectTrue(composer.sdfPreviewDraws() == 1u, "sdf preview stub drawn");
    expectTrue(composer.renderer().sample(160, 120) > 0, "preview stubs produce pixels");
}

} // namespace

int main() {
    fuse::core::initialize();
    testCookedAssetBindingsProbeHeaders();
    testMeshSdfPreviewCatalogAndComposerDraws();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_cooked_bindings: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_cooked_bindings: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
