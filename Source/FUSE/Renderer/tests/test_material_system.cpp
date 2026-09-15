#include <fuse/core/init.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testMaterialPack() {
    fuse::renderer::Material material{};
    material.baseColor = {0.8f, 0.2f, 0.1f};
    material.roughness = 0.35f;
    material.metallic = 0.75f;
    material.emissiveColor = {1.f, 0.5f, 0.25f};
    material.emissiveIntensity = 2.f;
    material.shadingModel = fuse::renderer::ShadingModel::Emissive;
    material.isProcedural = true;
    material.proceduralFnId = 3u;

    const fuse::renderer::Material::GPUMaterial packed = material.pack();
    expectTrue(packed.baseColor.x == 0.8f, "packed base color x");
    expectTrue(packed.baseColor.w == 0.75f, "packed metallic in base color w");
    expectTrue(packed.roughnessEmissive.x == 0.35f, "packed roughness");
    expectTrue(packed.emissiveIntensity == 2.f, "packed emissive intensity");
    expectTrue(packed.shadingModel == static_cast<fuse::u32>(fuse::renderer::ShadingModel::Emissive),
               "packed shading model");
    expectTrue((packed.flags & 1u) != 0u, "procedural flag set");
}

void testMaterialSystemRegisterFlush() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for material test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::MaterialSystem system;
    system.init(resources);

    fuse::renderer::Material dielectric{};
    dielectric.metallic = 0.f;
    dielectric.roughness = 0.5f;

    fuse::renderer::Material metal{};
    metal.metallic = 1.f;
    metal.roughness = 0.2f;
    metal.baseColor = {0.9f, 0.9f, 0.9f};

    const fuse::u32 dielectricId = system.registerMaterial(dielectric);
    const fuse::u32 metalId = system.registerMaterial(metal);
    expectTrue(dielectricId == 0u, "first material id");
    expectTrue(metalId == 1u, "second material id");
    expectTrue(system.materialCount() == 2u, "material count");

    system.updateMaterial(dielectricId, dielectric);
    system.flushGpuBuffer();
    expectTrue(system.dirtyCount() == 0u, "dirty count cleared after flush");
    expectTrue(system.gpuMaterials().size() == 2u, "gpu material rows present");
    expectTrue(system.gpuMaterials()[1].baseColor.w == 1.f, "metal metallic packed");

    system.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testMaterialPack();
    testMaterialSystemRegisterFlush();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_material_system: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_material_system: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
