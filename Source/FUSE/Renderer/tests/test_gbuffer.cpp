#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
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

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testLayoutFormats() {
    using fuse::renderer::GBufferAttachment;
    using fuse::renderer::GBufferLayout;
    using fuse::renderer::GpuFormat;

    expectTrue(GBufferLayout::format(GBufferAttachment::NormalAo) == GpuFormat::R16G16B16A16Sfloat,
               "RT0 uses RGBA16F");
    expectTrue(GBufferLayout::format(GBufferAttachment::AlbedoAlpha) == GpuFormat::R8G8B8A8Unorm,
               "RT1 uses RGBA8");
    expectTrue(GBufferLayout::format(GBufferAttachment::RoughMetalEmissiveShading) == GpuFormat::R8G8B8A8Unorm,
               "RT2 uses RGBA8");
    expectTrue(GBufferLayout::format(GBufferAttachment::Velocity) == GpuFormat::R16G16Sfloat,
               "RT3 uses RG16F");
    expectTrue(GBufferLayout::format(GBufferAttachment::Depth) == GpuFormat::R32Sfloat,
               "RT4 uses R32F");
    expectTrue(GBufferLayout::format(GBufferAttachment::Emissive) == GpuFormat::R16G16B16A16Sfloat,
               "RT5 uses RGBA16F");
    expectTrue(GBufferLayout::attachmentCount() == 6u, "six G-buffer attachments");
    expectTrue(GBufferLayout::channelCount(GBufferAttachment::Velocity) == 2u, "velocity has two channels");
    expectTrue(GBufferLayout::channelCount(GBufferAttachment::Depth) == 1u, "depth has one channel");
    expectTrue(GBufferLayout::validateAttachmentFormats(), "all attachment formats defined");
}

void testGBufferPackingRoundTrip() {
    fuse::renderer::GBufferPackedData source{};
    source.normal = {-0.2f, 0.7f, 0.68f};
    source.albedo = {0.25f, 0.5f, 0.75f};
    source.roughness = 0.42f;
    source.metallic = 0.9f;
    source.emissive = {2.f, 0.5f, 0.1f};
    source.ao = 0.8f;
    source.velocityX = 0.01f;
    source.velocityY = -0.02f;
    source.shadingModel = 4u;

    const fuse::renderer::GBufferMrt packed = fuse::renderer::GBufferPacking::pack(source, 0.75f);
    expectNear(packed.rt1.w, 0.75f, 1e-5f, "packed alpha channel");
    expectTrue(packed.rt2.z > 0.5f, "emissive mask set when emissive magnitude > 0.001");

    const fuse::renderer::GBufferPackedData decoded = fuse::renderer::GBufferPacking::unpack(packed);
    const float normalError =
        fuse::renderer::GBufferEncoding::angularErrorRadians(source.normal, decoded.normal);
    expectTrue(normalError < 0.001f, "packed normal round-trips through MRT channels");
    expectNear(decoded.albedo.x, source.albedo.x, 1e-5f, "packed albedo x");
    expectNear(decoded.roughness, source.roughness, 1e-5f, "packed roughness");
    expectNear(decoded.metallic, source.metallic, 1e-5f, "packed metallic");
    expectNear(decoded.emissive.y, source.emissive.y, 1e-5f, "packed emissive y");
    expectNear(decoded.ao, source.ao, 1e-5f, "packed ao");
    expectNear(decoded.velocityX, source.velocityX, 1e-5f, "packed velocity x");
    expectTrue(decoded.shadingModel == source.shadingModel, "packed shading model id");
}

void testNormalEncodingRoundTrip() {
    const fuse::math::Vec3 directions[] = {
        {0.f, 0.f, 1.f},
        {1.f, 0.f, 0.f},
        {0.f, 1.f, 0.f},
        {-0.6f, 0.2f, 0.75f},
        {0.3f, -0.7f, 0.64f},
    };

    for (const fuse::math::Vec3& direction : directions) {
        const fuse::math::Vec2 encoded = fuse::renderer::GBufferEncoding::encodeNormal(direction);
        const fuse::math::Vec3 decoded = fuse::renderer::GBufferEncoding::decodeNormal(encoded);
        const float error = fuse::renderer::GBufferEncoding::angularErrorRadians(direction, decoded);
        if (error >= 0.001f) {
            std::fprintf(stderr, "FAIL: normal round-trip error %f for direction (%f,%f,%f)\n", error,
                         direction.x, direction.y, direction.z);
            ++g_failures;
        }
    }
}

void testGBufferAllocationStub() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for G-buffer test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready for G-buffer");

    fuse::renderer::GBuffer gbuffer;
    fuse::renderer::GBufferDesc desc{};
    desc.width = 320;
    desc.height = 240;
    desc.reversedZ = true;

    expectTrue(gbuffer.init(resources, desc), "G-buffer allocates attachments");
    expectTrue(gbuffer.isReady(), "G-buffer ready");
    expectTrue(gbuffer.targets().attachments[0].isValid(), "normal attachment valid");
    expectTrue(gbuffer.targets().attachments[4].isValid(), "depth attachment valid");

    gbuffer.resize(640, 480);
    expectTrue(gbuffer.desc().width == 640u, "G-buffer resized width");
    expectTrue(gbuffer.desc().height == 480u, "G-buffer resized height");

    gbuffer.destroy();
    expectTrue(!gbuffer.isReady(), "G-buffer destroyed");
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testLayoutFormats();
    testNormalEncodingRoundTrip();
    testGBufferPackingRoundTrip();
    testGBufferAllocationStub();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_gbuffer: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_gbuffer: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
