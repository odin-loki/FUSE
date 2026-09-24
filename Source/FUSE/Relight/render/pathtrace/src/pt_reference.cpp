// FUSE Relight RL-5.1: the CPU reference path tracer (see pt_reference.hpp).
#include <fuse/relight/render/pathtrace/pt_reference.hpp>

#include <fuse/relight/render/material/bsdf_host.hpp>

#include "pt_reference_kernels.hpp"

#include <fuse/compute_kernel/launch.hpp>

#include <chrono>

namespace fuse::relight::render::pathtrace {

PtReferenceImage::PtReferenceImage() = default;
PtReferenceImage::~PtReferenceImage() = default;
PtReferenceImage::PtReferenceImage(const PtReferenceImage&) = default;
PtReferenceImage& PtReferenceImage::operator=(const PtReferenceImage&) = default;

void PtReferenceImage::resize(u32 width, u32 height) {
    m_width = width;
    m_height = height;
    m_pixels.assign(std::size_t(width) * height, ptk::PtReferencePixel{});
}

void PtReferenceImage::clear() { m_pixels.assign(m_pixels.size(), ptk::PtReferencePixel{}); }

const ptk::PtReferencePixel& PtReferenceImage::pixel(u32 x, u32 y) const {
    return m_pixels[std::size_t(y) * m_width + x];
}

ptk::PtReferencePixel* PtReferenceImage::data() { return m_pixels.data(); }

u32 PtReferenceImage::samples(u32 x, u32 y) const { return pixel(x, y).samples; }

double PtReferenceImage::mean(u32 x, u32 y, u32 c) const {
    const ptk::PtReferencePixel& p = pixel(x, y);
    return p.samples != 0u ? p.sum[c] / double(p.samples) : 0.0;
}

double PtReferenceImage::variance(u32 x, u32 y, u32 c) const {
    const ptk::PtReferencePixel& p = pixel(x, y);
    if (p.samples < 2u) {
        return 0.0;
    }
    const double n = double(p.samples);
    const double m = p.sum[c] / n;
    const double v = (p.sumSq[c] - n * m * m) / (n - 1.0);
    return v > 0.0 ? v : 0.0;
}

double PtReferenceImage::channel(u32 x, u32 y, u32 which, u32 c) const {
    const ptk::PtReferencePixel& p = pixel(x, y);
    if (p.samples == 0u) {
        return 0.0;
    }
    const double* src = which == 0u ? p.emissive : (which == 1u ? p.diffuse : p.specular);
    return src[c] / double(p.samples);
}

bool renderReference(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
                     u32 sampleBase, u32 samples, PtReferenceImage& image, kernel::Backend backend,
                     PtReferenceStats* stats) {
    if (!scene.valid() || width == 0u || height == 0u || image.width() != width || image.height() != height) {
        return false;
    }
    const material::AlbedoLut& lut = material::sharedAlbedoLut();
    if (!lut.valid()) {
        return false;
    }
    std::vector<ptk::PtCpuTexture> textures(scene.textures().size());
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const PtTextureImage& t = scene.textures()[i];
        textures[i].handle = t.handle;
        textures[i].width = t.width;
        textures[i].height = t.height;
        textures[i].texels = t.texels.empty() ? nullptr : t.texels.data();
    }
    ptk::PtCpuContext ctx{};
    ctx.lut = lut.data();
    ctx.rt = &scene.reference();
    ctx.lights = &scene.lightSet();
    ctx.lightRecords = scene.lightRecords().empty() ? nullptr : scene.lightRecords().data();
    ctx.instances = scene.instanceWords().data();
    ctx.instanceCount = static_cast<u32>(scene.instanceWords().size() / kPtInstanceWords);
    ctx.triangles = scene.triangleWords().data();
    ctx.triangleCount = static_cast<u32>(scene.triangleWords().size() / kPtTriangleWords);
    ctx.materials = scene.materialWords().data();
    ctx.materialCount = static_cast<u32>(scene.materialWords().size() / kPtMaterialWords);
    ctx.portals = scene.portalWords().data();
    ctx.portalCount = static_cast<u32>(scene.portalWords().size() / kPtPortalWords);
    ctx.lightMap = scene.lightMap().empty() ? nullptr : scene.lightMap().data();
    ctx.lightMapCount = static_cast<u32>(scene.lightMap().size());
    ctx.textures = textures.empty() ? nullptr : textures.data();
    ctx.textureCount = static_cast<u32>(textures.size());
    Word params[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, params);
    ptk::PtReferenceParams p{};
    p.ctx = &ctx;
    p.params = params;
    p.pixels = kernel::Span<ptk::PtReferencePixel>{image.data(), width * height};
    p.width = width;
    p.height = height;
    p.samples = samples;
    p.writeGbuffer = true;
    const auto start = std::chrono::steady_clock::now();
    const kernel::LaunchResult r = kernel::launch(
        backend, kernel::KernelLaunch{ptk::kReferenceName, kernel::extent2(width, height), ptk::kReferenceWorkgroup},
        ptk::PtReferenceKernel{}, p);
    if (stats != nullptr) {
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats->paths = u64(width) * height * samples;
    }
    return r.ok;
}

} // namespace fuse::relight::render::pathtrace
