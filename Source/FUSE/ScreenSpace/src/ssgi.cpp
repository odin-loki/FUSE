#include <fuse/ssfx/ssgi.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/ssfx/ssgi_kernel.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

// The gather, sampling and per-bounce re-lighting live once in fuse/ssfx/ssgi_kernel.hpp (FUSE_HOST_DEVICE,
// shared with Compute/kernels/ssgi.cu); this TU adapts the public scalar API and drives the bounce launches.

namespace fuse::ssfx {

math::Vec3 ssgiSampleDirection(const math::Vec3& n, u32 x, u32 y, u32 i, u32 j, u32 sampleSqrt) {
    return ssgi_kernel::sample_direction(n, x, y, i, j, sampleSqrt);
}

SsgiParams clampSsgiParams(const SsgiParams& raw) {
    return ssgi_kernel::clamp_params(raw);
}

math::Vec3 ssgiPixelGather(const SsfxGBufferView& view, const math::Vec3* radiance, const SsgiParams& rawParams,
                           u32 x, u32 y) {
    return ssgi_kernel::pixel_gather(view, radiance, ssgi_kernel::clamp_params(rawParams), x, y);
}

bool computeSsgiCpu(const SsfxGBufferView& view, const math::Vec3* directRadiance, const math::Vec3* albedo,
                    const SsgiParams& rawParams, math::Vec3* indirectOut, kernel::Backend backend) {
    if (!view.valid() || directRadiance == nullptr || indirectOut == nullptr) {
        return false;
    }
    const SsgiParams params = ssgi_kernel::clamp_params(rawParams);
    const u32 count = view.camera.width * view.camera.height;
    // Every bounce launch reads the inputs while writing the output: render into a temporary when they alias.
    const auto overlaps = [&](const math::Vec3* in) {
        const auto addr = [](const math::Vec3* ptr) { return reinterpret_cast<std::uintptr_t>(ptr); };
        return in != nullptr && addr(in) < addr(indirectOut + count) && addr(indirectOut) < addr(in + count);
    };
    std::vector<math::Vec3> aliasTarget;
    math::Vec3* out = indirectOut;
    if (overlaps(directRadiance) || overlaps(albedo)) {
        aliasTarget.resize(count);
        out = aliasTarget.data();
    }
    if (params.bounces == 0u) {
        std::fill(indirectOut, indirectOut + count, math::Vec3{});
        return true;
    }

    std::vector<math::Vec3> scratch(static_cast<size_t>(count) * ssgi_kernel::scratch_buffers(params.bounces));
    ssgi_kernel::Params base{};
    base.view = view;
    base.ssgi = params;
    base.direct = directRadiance;
    base.albedo = albedo;
    base.indirect_out = out;
    const bool ok = ssgi_kernel::run_bounces(
        base, scratch.empty() ? nullptr : scratch.data(), scratch.size() > count ? scratch.data() + count : nullptr,
        [&](const ssgi_kernel::Params& bounce) {
            return kernel::launch(backend, ssgi_kernel::make_launch(view), ssgi_kernel::Kernel{}, bounce).ok;
        });
    if (ok && out != indirectOut) {
        std::copy(aliasTarget.begin(), aliasTarget.end(), indirectOut);
    }
    return ok;
}

} // namespace fuse::ssfx
