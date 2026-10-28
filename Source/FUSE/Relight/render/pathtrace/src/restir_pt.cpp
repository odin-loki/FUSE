// FUSE Relight: bounded CPU ReSTIR PT initial reservoir (see restir_pt.hpp).
#include <fuse/relight/render/pathtrace/restir_pt.hpp>

#include <cmath>
#include <vector>

#include "pt_reference_cpp.hpp"

namespace fuse::relight::render::pathtrace {

namespace {

float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool finite3(float x, float y, float z) { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }

void clearSample(RestirPtReservoir& r) {
    r.direction[0] = r.direction[1] = r.direction[2] = 0.f;
    r.hitDistance = 0.f;
    r.pHat[0] = r.pHat[1] = r.pHat[2] = 0.f;
    r.M = 0.f;
    r.pdf = 0.f;
    r.secondHit = false;
}

/// Balance-heuristic mass of a reservoir: M * p_hat / q, with p_hat the luminance of the stored target.
float misMass(const RestirPtReservoir& r) {
    if (!r.secondHit || !(r.M > 0.f) || !(r.pdf > 0.f) || !std::isfinite(r.pdf) || !std::isfinite(r.M)) {
        return 0.f;
    }
    const float p = ptk::ptLum(ptk::float3(r.pHat[0], r.pHat[1], r.pHat[2]));
    if (!(p > 0.f) || !std::isfinite(p)) {
        return 0.f;
    }
    return r.M * p / r.pdf;
}

bool previousEligible(const RestirPtReservoir& previous, const RestirPtReservoir& current) {
    if (!previous.secondHit || !(previous.M > 0.f)) {
        return false;
    }
    if (current.hasPrimaryId && previous.hasPrimaryId) {
        return current.primaryInstance == previous.primaryInstance &&
               current.primaryPrimitive == previous.primaryPrimitive;
    }
    return dist3(current.camera, previous.camera) < kRestirPtMotionEpsilon &&
           dist3(current.primaryPos, previous.primaryPos) < kRestirPtMotionEpsilon;
}

float capM(float m, float mCap) {
    if (!(mCap > 0.f) || !std::isfinite(mCap)) {
        return 0.f;
    }
    if (!std::isfinite(m) || !(m > 0.f)) {
        return 0.f;
    }
    return m > mCap ? mCap : m;
}

void adoptSample(RestirPtReservoir& dst, const RestirPtReservoir& src) {
    dst.direction[0] = src.direction[0];
    dst.direction[1] = src.direction[1];
    dst.direction[2] = src.direction[2];
    dst.hitDistance = src.hitDistance;
    dst.pHat[0] = src.pHat[0];
    dst.pHat[1] = src.pHat[1];
    dst.pHat[2] = src.pHat[2];
    dst.pdf = src.pdf;
    dst.secondHit = src.secondHit;
}

struct SceneBind {
    ptk::PtCpuContext ctx{};
    ptk::PtParams params{};
    std::vector<ptk::PtCpuTexture> textures;
};

bool bindScene(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height, u32 frameSeed,
               u32 sampleBase, SceneBind& bound) {
    bound.textures.resize(scene.textures().size());
    for (std::size_t i = 0; i < bound.textures.size(); ++i) {
        const PtTextureImage& t = scene.textures()[i];
        bound.textures[i].handle = t.handle;
        bound.textures[i].width = t.width;
        bound.textures[i].height = t.height;
        bound.textures[i].texels = t.texels.empty() ? nullptr : t.texels.data();
    }
    ptk::PtCpuContext& ctx = bound.ctx;
    ctx = ptk::PtCpuContext{};
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
    ctx.textures = bound.textures.empty() ? nullptr : bound.textures.data();
    ctx.textureCount = static_cast<u32>(bound.textures.size());
    Word words[kPtParamWords];
    scene.packParams(settings, width, height, frameSeed, sampleBase, words);
    bound.params = ptk::ptParamsUnpack(words);
    return ctx.rt != nullptr;
}

RestirPtReservoir tracePixel(const SceneBind& bound, u32 x, u32 y) {
    const ptk::PtCpuContext& ctx = bound.ctx;
    const ptk::PtParams& P = bound.params;
    RestirPtReservoir r;
    r.camera[0] = P.camOrigin.x;
    r.camera[1] = P.camOrigin.y;
    r.camera[2] = P.camOrigin.z;

    const ptk::uint seed = ptk::ptSeed(x, y, P.frameSeed, P.sampleBase);
    float jx = 0.5f;
    float jy = 0.5f;
    if ((P.flags & ptk::kPtFlagJitter) != 0u) {
        jx = ptk::ptRandom(seed, 0u);
        jy = ptk::ptRandom(seed, 1u);
    }
    const ptk::float3 origin = P.camOrigin;
    const ptk::float3 view = ptk::ptPrimaryDirection(P, float(x) + jx, float(y) + jy);
    const ptk::PtRawHit primary = ptk::ptTraceScene(ctx, P, origin, view, 0.f, ptk::kPtFar, ptk::kPtMaskVisible);
    if (!primary.hit) {
        return r;
    }
    if (primary.instance != ptk::kPtInvalid) {
        r.hasPrimaryId = true;
        r.primaryInstance = primary.instance;
        r.primaryPrimitive = primary.primitive;
    }
    const ptk::PtSurface surface = ptk::ptLoadSurface(ctx, primary);
    r.primaryPos[0] = surface.position.x;
    r.primaryPos[1] = surface.position.y;
    r.primaryPos[2] = surface.position.z;
    // The second vertex is a diffuse continuation. Unlit and portal hits do not scatter one.
    if ((surface.flags & ptk::kPtMatUnlit) != 0u || surface.portal != ptk::kPtInvalid) {
        return r;
    }

    ptk::float3 n = surface.normal;
    ptk::float3 ng = surface.geoNormal;
    if (surface.m.model == ptk::kBsdfModelOpaque && ptk::dot(ng, view) > 0.f) {
        n = -n;
        ng = -ng;
    }
    ptk::float3 tx;
    ptk::float3 ty;
    ptk::ptBasis(n, tx, ty);
    const ptk::float3 wiLocal = ptk::bsdfCosineSample(ptk::ptRandom(seed, 2u), ptk::ptRandom(seed, 3u));
    ptk::float3 wi = ptk::ptSafeNormalize(tx * wiLocal.x + ty * wiLocal.y + n * wiLocal.z, n);
    if (!finite3(wi.x, wi.y, wi.z)) {
        return r;
    }
    const ptk::float3 from = ptk::ptOffset(surface.position, ng, wi, P.rayEps);
    const ptk::PtRawHit second = ptk::ptTraceScene(ctx, P, from, wi, 0.f, ptk::kPtFar, ptk::kPtMaskVisible);
    r.direction[0] = wi.x;
    r.direction[1] = wi.y;
    r.direction[2] = wi.z;
    r.pdf = wiLocal.z / ptk::kBsdfPi;
    if (!second.hit || !(second.t > 0.f) || !std::isfinite(second.t)) {
        return r;
    }
    const ptk::PtSurface hit = ptk::ptLoadSurface(ctx, second);
    // Lambertian cosine weight: f * cos / pdf = albedo. p_hat = throughput * Le.
    const ptk::float3 pHat = surface.m.albedo * hit.emission;
    if (!finite3(pHat.x, pHat.y, pHat.z) || !std::isfinite(r.pdf)) {
        clearSample(r);
        return r;
    }
    r.pHat[0] = pHat.x;
    r.pHat[1] = pHat.y;
    r.pHat[2] = pHat.z;
    r.hitDistance = second.t;
    r.M = 1.f;
    r.secondHit = true;
    return r;
}

} // namespace

bool RestirPtCpu::initial(const PtCompiledScene& scene, const PtSettings& settings, u32 width, u32 height,
                          u32 frameSeed, u32 sampleBase) {
    if (!scene.valid() || width == 0u || height == 0u) {
        m_reservoirs.clear();
        m_width = 0;
        m_height = 0;
        return false;
    }
    SceneBind bound;
    if (!bindScene(scene, settings, width, height, frameSeed, sampleBase, bound)) {
        m_reservoirs.clear();
        m_width = 0;
        m_height = 0;
        return false;
    }
    m_width = width;
    m_height = height;
    m_reservoirs.assign(std::size_t(width) * height, RestirPtReservoir{});
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            m_reservoirs[std::size_t(y) * width + x] = tracePixel(bound, x, y);
        }
    }
    return true;
}

RestirPtReservoir temporal_reuse(const RestirPtReservoir& previous, const RestirPtReservoir& current, u32 pixel,
                                 float mCap) {
    RestirPtReservoir out = current;
    if (!out.secondHit || !(out.M > 0.f)) {
        clearSample(out);
        return out;
    }
    if (!previousEligible(previous, out)) {
        out.M = capM(out.M, mCap);
        if (!(out.M > 0.f)) {
            clearSample(out);
        }
        return out;
    }
    // w_i = M_i * p_hat(x_i) / q_i(x_i). The ratio w / (w_c + w_p) is the balance heuristic between the
    // current cosine sample and the previous reservoir (same pixel, Jacobian 1, no hybrid shift).
    const float wCurrent = misMass(out);
    const float wPrevious = misMass(previous);
    const float sum = wCurrent + wPrevious;
    const float u = ptk::ptRandom(ptk::ptHash(pixel + 1u), 17u);
    if (sum > 0.f && !(u * sum < wCurrent)) {
        adoptSample(out, previous);
    }
    out.M = capM(current.M + previous.M, mCap);
    if (!(out.M > 0.f)) {
        clearSample(out);
    }
    return out;
}

} // namespace fuse::relight::render::pathtrace
