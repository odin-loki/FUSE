// DDGI atlas upload: the GPU atlas textures use the same bordered (res + 2)^2 tile layout as the CPU
// reference volume, and `DDGI::uploadAtlases` copies the CPU texels into them through the async
// ResourceManager upload path. Verified by reading both atlases back (Lavapipe) and comparing
// every texel with the packed CPU volume, plus spot checks of tile origins and border texels.
#include <fuse/core/init.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::f32;
using fuse::u16;
using fuse::u32;
using fuse::usize;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

DDGIDesc smallVolume() {
    DDGIDesc desc{};
    desc.grid_origin = {-3.f, 0.5f, -2.f};
    desc.probe_spacing = {2.f, 2.f, 2.f};
    desc.grid_dims = {4, 2, 3};
    desc.rays_per_probe = 64;
    desc.probes_per_frame = 24;
    desc.irradiance_res = 6;
    desc.depth_res = 14;
    desc.max_ray_distance = 12.f;
    return desc;
}

void testPackedLayoutMatchesCpuVolume(const DdgiCpuVolume& volume) {
    const DDGIDesc& desc = volume.desc();
    std::vector<u16> irradiance;
    std::vector<u16> distance;
    expectTrue(ddgi_cpu::packIrradianceAtlasRgba16f(volume, irradiance), "irradiance atlas packs");
    expectTrue(ddgi_cpu::packDistanceAtlasRg16f(volume, distance), "distance atlas packs");
    const u32 irrWidth = ddgi_util::irradianceAtlasWidth(desc);
    const u32 depthWidth = ddgi_util::depthAtlasWidth(desc);
    expectTrue(irradiance.size() == static_cast<usize>(irrWidth) * ddgi_util::irradianceAtlasHeight(desc) * 4u,
               "irradiance atlas size = bordered extents x RGBA");
    expectTrue(volume.irradianceTileSize() == ProbeGridLayout::irradianceTileSize(desc) &&
                   volume.distanceTileSize() == ProbeGridLayout::depthTileSize(desc),
               "GPU tile size equals CPU bordered tile size");

    u32 mismatches = 0;
    for (u32 probe = 0; probe < volume.probeCount(); ++probe) {
        const ProbeGridCoord coord = ProbeGridLayout::probeCoordFromIndex(desc, probe);
        const Vec2 irrOrigin = ProbeGridLayout::probeIrradianceAtlasOrigin(desc, coord);
        const Vec2 depthOrigin = ProbeGridLayout::probeDepthAtlasOrigin(desc, coord);
        // Border corner (0,0) and an interior texel of every tile land where the layout helpers say.
        const u32 samples[2][2] = {{0u, 0u}, {desc.irradiance_res / 2u, 1u}};
        for (const auto& s : samples) {
            const usize texel = (static_cast<usize>(irrOrigin.y) + s[1]) * irrWidth + static_cast<usize>(irrOrigin.x) + s[0];
            const Vec3 e = volume.irradianceTexel(probe, s[0], s[1]);
            mismatches += irradiance[texel * 4u + 0u] != GBufferQuantize::floatToHalf(e.x) ? 1u : 0u;
            mismatches += irradiance[texel * 4u + 2u] != GBufferQuantize::floatToHalf(e.z) ? 1u : 0u;
        }
        const u32 last = volume.distanceTileSize() - 1u;
        const usize dTexel = (static_cast<usize>(depthOrigin.y) + last) * depthWidth + static_cast<usize>(depthOrigin.x) + last;
        const Vec2 m = volume.distanceTexel(probe, last, last);
        mismatches += distance[dTexel * 2u + 0u] != GBufferQuantize::floatToHalf(m.x) ? 1u : 0u;
        mismatches += distance[dTexel * 2u + 1u] != GBufferQuantize::floatToHalf(m.y) ? 1u : 0u;
    }
    expectTrue(mismatches == 0u, "packed atlas texels sit at ProbeGridLayout tile origins");
}

void testUploadRoundTrip() {
    VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — atlas upload round trip needs an ICD (Lavapipe in CI)\n");
        return;
    }
    BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());
    {
        ResourceManager resources;
        expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

        DdgiCpuScene scene{};
        DdgiCpuSurface wall{};
        wall.albedo = {0.7f, 0.4f, 0.3f};
        scene.addBox({-5.f, -1.f, -5.f}, {5.f, 0.f, 5.f}, wall);
        DdgiCpuSurface light{};
        light.emissive = {4.f, 3.f, 2.f};
        scene.addBox({-1.f, 3.f, -1.f}, {1.f, 3.2f, 1.f}, light);
        scene.sun_direction = Vec3{0.3f, 0.9f, 0.2f}.normalized();
        scene.sun_irradiance = {2.f, 2.f, 1.8f};
        scene.sky_radiance = {0.1f, 0.15f, 0.3f};

        DDGI ddgi;
        expectTrue(ddgi.init(smallVolume(), resources), "DDGI initialises small volume");
        ddgi.setCpuScene(&scene);
        for (u32 frame = 0; frame < 3u; ++frame) {
            expectTrue(ddgi.update(frame), "DDGI CPU update");
        }
        const DdgiCpuVolume* volume = ddgi.cpuVolume();
        expectTrue(volume != nullptr, "CPU volume present");
        if (volume == nullptr) {
            return;
        }
        testPackedLayoutMatchesCpuVolume(*volume);

        const Texture* irrTexture = resources.getTexture(ddgi.volume().irradiance_atlas);
        const Texture* depthTexture = resources.getTexture(ddgi.volume().depth_atlas);
        expectTrue(irrTexture != nullptr && irrTexture->desc.width == ddgi_util::irradianceAtlasWidth(ddgi.desc()) &&
                       irrTexture->desc.height == ddgi_util::irradianceAtlasHeight(ddgi.desc()),
                   "irradiance atlas texture has bordered extents");
        expectTrue(depthTexture != nullptr && depthTexture->desc.width == ddgi_util::depthAtlasWidth(ddgi.desc()),
                   "depth atlas texture has bordered extents");

        UploadTicket ticket{};
        expectTrue(ddgi.uploadAtlases(&ticket), "atlas upload submitted");
        expectTrue(ticket.isValid(), "upload ticket valid");
        expectTrue(resources.waitUpload(ticket), "atlas upload completes");

        std::vector<u16> expectedIrr;
        std::vector<u16> expectedDist;
        ddgi_cpu::packIrradianceAtlasRgba16f(*volume, expectedIrr);
        ddgi_cpu::packDistanceAtlasRg16f(*volume, expectedDist);
        std::vector<u16> readIrr(expectedIrr.size(), 0xFFFFu);
        std::vector<u16> readDist(expectedDist.size(), 0xFFFFu);
        expectTrue(resources.readTexture(ddgi.volume().irradiance_atlas, readIrr.data(), readIrr.size() * sizeof(u16)),
                   "irradiance atlas reads back");
        expectTrue(resources.readTexture(ddgi.volume().depth_atlas, readDist.data(), readDist.size() * sizeof(u16)),
                   "depth atlas reads back");
        const bool irrMatch = readIrr == expectedIrr;
        const bool distMatch = readDist == expectedDist;
        expectTrue(irrMatch, "irradiance atlas round-trips bit-exactly");
        expectTrue(distMatch, "distance atlas round-trips bit-exactly");

        // Readback returned the atlases to SHADER_READ_ONLY: a second update + upload still works.
        expectTrue(ddgi.update(3u), "DDGI update after readback");
        expectTrue(ddgi.uploadAtlases(&ticket) && resources.waitUpload(ticket), "second upload completes");
        ddgi_cpu::packIrradianceAtlasRgba16f(*volume, expectedIrr);
        expectTrue(resources.readTexture(ddgi.volume().irradiance_atlas, readIrr.data(), readIrr.size() * sizeof(u16)) &&
                       readIrr == expectedIrr,
                   "second irradiance upload round-trips");
        std::printf("ddgi atlas upload: irradiance %ux%u (%zu KB) match=%d, depth %ux%u match=%d\n",
                    ddgi_util::irradianceAtlasWidth(ddgi.desc()), ddgi_util::irradianceAtlasHeight(ddgi.desc()),
                    expectedIrr.size() * sizeof(u16) / 1024u, irrMatch ? 1 : 0,
                    ddgi_util::depthAtlasWidth(ddgi.desc()), ddgi_util::depthAtlasHeight(ddgi.desc()),
                    distMatch ? 1 : 0);
        ddgi.destroy();
        resources.destroy();
    }
    bindless.destroy(*bootstrap->device());
}

void testPackWithoutDevice() {
    DdgiCpuVolume volume;
    std::vector<u16> out;
    expectTrue(!ddgi_cpu::packIrradianceAtlasRgba16f(volume, out), "pack rejects an uninitialised volume");
    expectTrue(volume.init(smallVolume()), "CPU volume initialises");
    DdgiCpuScene scene{};
    scene.sky_radiance = {0.5f, 0.5f, 0.5f};
    volume.update(scene, 0u);
    testPackedLayoutMatchesCpuVolume(volume);
}

} // namespace

int main() {
    fuse::core::initialize();
    testPackWithoutDevice();
    testUploadRoundTrip();
    if (g_failures == 0) {
        std::printf("fuse_ddgi_atlas_upload: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "fuse_ddgi_atlas_upload: %d failure(s)\n", g_failures);
    return 1;
}
