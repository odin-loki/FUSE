#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/renderer/atmosphere/sky_lut.hpp>
#include <fuse/renderer/atmosphere/sky_pass.hpp>
#include <fuse/renderer/atmosphere/sky_scatter.hpp>
#include <fuse/renderer/render_graph.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr fuse::f32 kPi = 3.14159265358979323846f;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 value, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testAtmosphereParamsDefaults() {
    const fuse::renderer::AtmosphereParams params{};
    expectNear(params.earth_radius, 6371000.f, 1.f, "earth radius default");
    expectNear(params.atmo_radius, 6471000.f, 1.f, "atmo radius default");
    expectNear(params.rayleigh_coeff.x, 5.8e-6f, 1e-8f, "rayleigh R default");
    expectNear(params.mie_coeff, 21e-6f, 1e-8f, "mie coeff default");
    expectTrue(params.view_samples == 16u, "view samples default");
    expectTrue(params.light_samples == 8u, "light samples default");
}

void testPhaseFunctions() {
    const fuse::f32 rayleigh_forward = fuse::renderer::rayleigh_phase(1.f);
    const fuse::f32 rayleigh_side = fuse::renderer::rayleigh_phase(0.f);
    expectTrue(rayleigh_forward > rayleigh_side, "rayleigh forward scattering stronger");

    const fuse::f32 mie_forward = fuse::renderer::mie_phase(1.f, 0.758f);
    const fuse::f32 mie_back = fuse::renderer::mie_phase(-1.f, 0.758f);
    expectTrue(mie_forward > mie_back, "mie forward lobe stronger than backscatter");
}

void testComputeSkyColourStub() {
    fuse::renderer::AtmosphereParams params{};
    const fuse::math::Vec3 sun_dir{0.f, 0.2f, 1.f};
    const fuse::math::Vec3 zenith{0.f, 1.f, 0.f};
    const fuse::math::Vec3 horizon{0.f, 0.f, 1.f};

    const fuse::math::Vec3 zenith_colour =
        fuse::renderer::compute_sky_colour({}, zenith, sun_dir, params);
    const fuse::math::Vec3 horizon_colour =
        fuse::renderer::compute_sky_colour({}, horizon, sun_dir, params);

    expectTrue(zenith_colour.z > zenith_colour.x, "zenith skews blue in stub");
    expectTrue(horizon_colour.x / horizon_colour.z > zenith_colour.x / zenith_colour.z,
               "horizon warmer than zenith in stub");
}

void testSkyLutBuildAndSample() {
    fuse::renderer::SkyLutDesc desc{};
    desc.sun_elevation_bins = 8;
    desc.view_elevation_bins = 8;

    fuse::renderer::SkyLut lut;
    const fuse::math::Vec3 sun_dir{0.f, 0.5f, 1.f};
    expectTrue(lut.build(desc, sun_dir), "sky LUT builds");
    expectTrue(lut.isReady(), "sky LUT ready");
    expectTrue(lut.entries().size() == 64u, "LUT entry count");

    const fuse::math::Vec3 sampled = lut.sample(0.f, kPi * 0.25f);
    expectTrue(sampled.x >= 0.f && sampled.y >= 0.f && sampled.z >= 0.f, "LUT sample non-negative");
}

void testSkyPassScaffold() {
    fuse::renderer::SkyPassDesc desc{};
    desc.sun_direction = {0.f, 0.4f, 1.f};
    desc.lut.sun_elevation_bins = 4;
    desc.lut.view_elevation_bins = 4;

    auto pass = fuse::renderer::SkyPass::create(desc);
    expectTrue(pass != nullptr, "sky pass allocated");
    expectTrue(pass->isReady(), "sky pass ready");
    expectTrue(pass->lut().isReady(), "sky pass LUT ready");
    expectTrue(pass->recordFrame(), "sky pass records frame");
    expectTrue(pass->lastStats().framesRecorded == 1u, "sky pass frame counter");
}

void testSkyPassGraphNode() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::resetSkyPassGraphStorage();
    fuse::renderer::addSkyPassToGraph(graph);
    graph.compile();

    expectTrue(graph.compileInfo().compiled, "sky pass graph compiles");
    expectTrue(graph.compileInfo().passCount >= 1u, "sky pass node present");
}

} // namespace

int main() {
    fuse::core::initialize();

    testAtmosphereParamsDefaults();
    testPhaseFunctions();
    testComputeSkyColourStub();
    testSkyLutBuildAndSample();
    testSkyPassScaffold();
    testSkyPassGraphNode();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_atmosphere_sky: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_atmosphere_sky: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
