// FUSE Relight RL-5.5: the CPU oracle of the path tracer's denoiser (PtDenoiseCpu, see pt_denoise.hpp).
#include <fuse/relight/render/denoise/pt_denoise.hpp>

#include <fuse/relight/render/material/bsdf_host.hpp>

#include "pt_reference_kernels.hpp"
#include "rl_dn_gradient_cpp.hpp"

#include <fuse/compute_kernel/launch.hpp>

#include <cstring>

namespace fuse::relight::render::denoise {

bool PtDenoiseCpu::runGradient(const pathtrace::PtCompiledScene& scene, const pathtrace::Word* params, bool havePrev,
                               u32 strataW, u32 strataH, pathtrace::Word* records, pathtrace::Word* gradient,
                               kernel::Backend backend) {
    if (!scene.valid() || strataW == 0u || strataH == 0u) {
        return false;
    }
    const material::AlbedoLut& lut = material::sharedAlbedoLut();
    if (!lut.valid()) {
        return false;
    }
    std::vector<ptk::PtCpuTexture> textures(scene.textures().size());
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const pathtrace::PtTextureImage& t = scene.textures()[i];
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
    ctx.instanceCount = static_cast<u32>(scene.instanceWords().size() / pathtrace::kPtInstanceWords);
    ctx.triangles = scene.triangleWords().data();
    ctx.triangleCount = static_cast<u32>(scene.triangleWords().size() / pathtrace::kPtTriangleWords);
    ctx.materials = scene.materialWords().data();
    ctx.materialCount = static_cast<u32>(scene.materialWords().size() / pathtrace::kPtMaterialWords);
    ctx.portals = scene.portalWords().data();
    ctx.portalCount = static_cast<u32>(scene.portalWords().size() / pathtrace::kPtPortalWords);
    ctx.lightMap = scene.lightMap().empty() ? nullptr : scene.lightMap().data();
    ctx.lightMapCount = static_cast<u32>(scene.lightMap().size());
    ctx.textures = textures.empty() ? nullptr : textures.data();
    ctx.textureCount = static_cast<u32>(textures.size());
    ptk::RldnParams p{};
    p.ctx = &ctx;
    p.params = params;
    p.records = kernel::Span<ptk::float4>{records, strataW * strataH};
    p.gradient = kernel::Span<ptk::float4>{gradient, strataW * strataH};
    p.strataW = strataW;
    p.strataH = strataH;
    p.havePrev = havePrev;
    const kernel::LaunchResult r = kernel::launch(
        backend, kernel::KernelLaunch{ptk::kGradientName, kernel::extent2(strataW, strataH), {8u, 8u, 1u}},
        ptk::RldnKernel{}, p);
    return r.ok;
}

void PtDenoiseCpu::init(u32 width, u32 height, const rdn::RdnSettings& settings) {
    m_width = width;
    m_height = height;
    m_settings = settings;
    m_ref.init(width, height, settings);
    m_image.resize(width, height);
    const usize n = static_cast<usize>(width) * height;
    const usize ns = static_cast<usize>((width + 2u) / 3u) * ((height + 2u) / 3u);
    m_diffuse.assign(n, {});
    m_specular.assign(n, {});
    m_normal.assign(n, {});
    m_albedoD.assign(n, {});
    m_albedoS.assign(n, {});
    m_depth.assign(n, 0.f);
    m_motion.assign(n, {});
    m_instance.assign(n, 0u);
    m_records.assign(ns, pathtrace::Word(0.f, 0.f, 0.f, 0.f));
    m_gradient.assign(ns, pathtrace::Word(0.f, -1.f, 0.f, -1.f));
    m_havePrev = false;
}

void PtDenoiseCpu::reset() {
    m_ref.reset();
    m_havePrev = false;
}

bool PtDenoiseCpu::runFrame(const pathtrace::PtCompiledScene& scene, const pathtrace::PtSettings& settings,
                            u32 frameSeed, u32 sampleBase, kernel::Backend backend) {
    if (m_width == 0u) {
        return false;
    }
    m_image.clear();
    const u32 spp = settings.samplesPerPixel > 0u ? settings.samplesPerPixel : 1u;
    if (!pathtrace::renderReference(scene, settings, m_width, m_height, frameSeed, sampleBase, spp, m_image, backend)) {
        return false;
    }
    const usize n = static_cast<usize>(m_width) * m_height;
    const ptk::PtReferencePixel* px = m_image.data();
    for (usize i = 0; i < n; ++i) {
        const ptk::PtReferencePixel& p = px[i];
        const f64 inv = 1.0 / static_cast<f64>(p.samples > 0u ? p.samples : 1u);
        const bool specHit = (p.flags & 2u) != 0u; // kPtSampleSpecularHit
        m_diffuse[i] = rdn::rdnk::float4(static_cast<f32>(p.diffuse[0] * inv), static_cast<f32>(p.diffuse[1] * inv),
                                         static_cast<f32>(p.diffuse[2] * inv), specHit ? 0.f : p.hitDist);
        m_specular[i] = rdn::rdnk::float4(static_cast<f32>(p.specular[0] * inv), static_cast<f32>(p.specular[1] * inv),
                                          static_cast<f32>(p.specular[2] * inv), specHit ? p.hitDist : 0.f);
        m_normal[i] = rdn::rdnk::float4(p.normal[0], p.normal[1], p.normal[2], p.roughness);
        m_albedoD[i] = rdn::rdnk::float4(p.albedoD[0], p.albedoD[1], p.albedoD[2], 0.f);
        m_albedoS[i] = rdn::rdnk::float4(p.albedoS[0], p.albedoS[1], p.albedoS[2], 0.f);
        m_depth[i] = p.depth;
        m_motion[i] = rdn::rdnk::float2(p.motion[0], p.motion[1]);
        m_instance[i] = p.instance;
    }
    // This frame's parameter words, then the previous frame's (the gradient producer's layout).
    pathtrace::Word cur[pathtrace::kPtParamWords];
    scene.packParams(settings, m_width, m_height, frameSeed, sampleBase, cur);
    const bool havePrev = m_havePrev && m_prevWidth == m_width;
    if (!havePrev) {
        std::memcpy(m_words + pathtrace::kPtParamWords, cur, sizeof(cur));
    }
    std::memcpy(m_words, cur, sizeof(cur));
    if (m_settings.gradients && !runGradient(scene, m_words, havePrev, (m_width + 2u) / 3u, (m_height + 2u) / 3u,
                                             m_records.data(), m_gradient.data(), backend)) {
        return false;
    }
    std::memcpy(m_words + pathtrace::kPtParamWords, cur, sizeof(cur));
    m_prevWidth = m_width;
    m_havePrev = true;
    rdn::RdnReferenceInputs in{};
    in.diffuse = m_diffuse.data();
    in.specular = m_specular.data();
    in.normal = m_normal.data();
    in.depth = m_depth.data();
    in.motion = m_motion.data();
    in.instance = m_instance.data();
    static_assert(sizeof(pathtrace::Word) == sizeof(rdn::rdnk::float4), "Word / rdnk::float4");
    in.gradient = m_settings.gradients ? reinterpret_cast<const rdn::rdnk::float4*>(m_gradient.data()) : nullptr;
    return m_ref.runFrame(in, cameraFromParams(cur, false), cameraFromParams(cur, true), backend);
}

} // namespace fuse::relight::render::denoise
