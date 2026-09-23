#include <fuse/core/temp_path.hpp>
#include <fuse/core/init.hpp>
#include <fuse/hybrid/cooked_asset_bindings.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

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

void writeBinaryFile(const char* path, const std::string& header, const std::vector<char>& payload) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << header;
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

void testCookedAssetBindingsProbeHeaders() {
    writeTextFile(fuse::test::tempPath("fuse_cooked_mat.fusetex").c_str(), "FUSETEX_STUB\n");
    writeTextFile(fuse::test::tempPath("fuse_cooked_mat_bc7.fusetex").c_str(), "FUSETEX_BC7\n");
    writeTextFile(fuse::test::tempPath("fuse_cooked_shader.fuseshader").c_str(), "FUSESHADER_STUB\n");

    const std::vector<char> spirvPayload = {
        static_cast<char>(0x03), static_cast<char>(0x02), static_cast<char>(0x23), static_cast<char>(0x07),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    writeBinaryFile(fuse::test::tempPath("fuse_cooked_shader_glslang.fuseshader").c_str(),
                    "FUSESHADER_GLSLANG\nstage=fragment\nversion=450\nwords=8\nDATA\n", spirvPayload);

    fuse::hybrid::CookedAssetBindings bindings;
    bindings.bindMaterial(fuse::test::tempPath("fuse_cooked_mat.fusetex"), 3u);
    bindings.bindMaterial(fuse::test::tempPath("fuse_cooked_mat_bc7.fusetex"), 4u);
    bindings.bindShader(fuse::test::tempPath("fuse_cooked_shader.fuseshader"), 7u);
    bindings.bindShader(fuse::test::tempPath("fuse_cooked_shader_glslang.fuseshader"), 8u);
    bindings.refreshTints();

    expectTrue(bindings.materialCount() == 2u, "material bindings recorded");
    expectTrue(bindings.shaderCount() == 2u, "shader bindings recorded");
    expectTrue(bindings.validMaterialCount() == 2u, "material headers probed");
    expectTrue(bindings.validShaderCount() == 2u, "shader headers probed");
    expectTrue(bindings.bc7MaterialCount() == 1u, "bc7 material header classified");
    expectTrue(bindings.glslangShaderCount() == 1u, "glslang shader header classified");
    expectTrue(bindings.materialTintR() > 0.f, "material tint applied");
    expectTrue(bindings.shaderTintG() > 0.f, "shader tint applied");
    expectTrue(bindings.shaderTintB() > bindings.shaderTintG(), "glslang shader boosts blue tint");

    const std::optional<fuse::hybrid::CookedMaterialBinding> material = bindings.findMaterial(4u);
    expectTrue(material.has_value() && material->headerKind == fuse::hybrid::CookedHeaderKind::TextureBc7,
               "material lookup by id returns bc7 binding");
    expectTrue(bindings.materialTintBoost(4u) > bindings.materialTintBoost(3u),
               "bc7 material tint boost exceeds stub");
}

void testMeshSdfPreviewCatalogAndComposerDraws() {
    fuse::hybrid::HybridComposer composer;
    fuse::world3d::World3D world3D;
    fuse::hybrid::DimensionFlags flags{};
    flags.enable3D = true;
    composer.setProjectFlags(flags);
    composer.attachWorld3D(&world3D);

    writeTextFile(fuse::test::tempPath("fuse_preview_mat.fusetex").c_str(), "FUSETEX_BC7\n");
    composer.cookedAssets().bindMaterial(fuse::test::tempPath("fuse_preview_mat.fusetex"), 1u);
    composer.cookedAssets().refreshTints();

    fuse::hybrid::MeshPreviewHint meshHint{};
    meshHint.x = 1.f;
    meshHint.y = 2.f;
    meshHint.z = 3.f;
    meshHint.materialId = 1u;
    meshHint.cookedMeshPath = fuse::test::tempPath("fuse_preview_mat.fusetex");
    meshHint.cookedMeshResolved = true;
    meshHint.visible = true;
    composer.previewCatalog().addMesh(meshHint);
    composer.previewCatalog().addSdf(
        {4.f, 5.f, 6.f, fuse::hybrid::SdfPreviewPrimitive::Torus, 1.2f, 0.5f, 0.f, 2u, true});

    expectTrue(composer.previewCatalog().meshCount() == 1u, "mesh preview hint stored");
    expectTrue(composer.previewCatalog().sdfCount() == 1u, "sdf preview hint stored");
    expectTrue(composer.previewCatalog().cookedMeshResolvedCount() == 1u, "cooked mesh resolved counted");

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
