// RL-5.5 radiance denoiser: settings, constants, layout, pass list and the CPU reference chain
// (include/fuse/renderer/denoise/radiance_denoise.hpp).
#include <fuse/renderer/denoise/radiance_denoise.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::renderer::denoise {

namespace {
constexpr u64 kTexel = 16u;

u64 align256(u64 v) { return (v + 255u) & ~u64{255u}; }

void setCamera(const RdnCamera& c, f32* o, f32* r, f32* u, f32* f) {
    for (u32 k = 0; k < 3u; ++k) {
        o[k] = c.origin[k];
        r[k] = c.right[k];
        u[k] = c.up[k];
        f[k] = c.forward[k];
    }
}
} // namespace

RdnSettings rdn_preset() { return RdnSettings{}; }

RdnFrame rdn_resolve(const RdnSettings& s, u32 width, u32 height, bool history, const RdnCamera& camera,
                     const RdnCamera& prevCamera) {
    RdnFrame c{};
    f32 o[3], r[3], u[3], f[3];
    setCamera(camera, o, r, u, f);
    c.camOx = o[0], c.camOy = o[1], c.camOz = o[2];
    c.camRx = r[0], c.camRy = r[1], c.camRz = r[2];
    c.camUx = u[0], c.camUy = u[1], c.camUz = u[2];
    c.camFx = f[0], c.camFy = f[1], c.camFz = f[2];
    setCamera(prevCamera, o, r, u, f);
    c.prvOx = o[0], c.prvOy = o[1], c.prvOz = o[2];
    c.prvRx = r[0], c.prvRy = r[1], c.prvRz = r[2];
    c.prvUx = u[0], c.prvUy = u[1], c.prvUz = u[2];
    c.prvFx = f[0], c.prvFy = f[1], c.prvFz = f[2];
    c.width = width;
    c.height = height;
    c.strataW = (width + kRdnStratum - 1u) / kRdnStratum;
    c.strataH = (height + kRdnStratum - 1u) / kRdnStratum;
    u32 flags = 0u;
    flags |= history ? FUSE_RDN_FLAG_HISTORY : 0u;
    flags |= s.gradients ? FUSE_RDN_FLAG_GRADIENTS : 0u;
    flags |= s.virtualMotion ? FUSE_RDN_FLAG_VIRTUAL : 0u;
    flags |= s.historyFix ? FUSE_RDN_FLAG_HISTFIX : 0u;
    flags |= s.spatialVariance ? FUSE_RDN_FLAG_SPATIALVAR : 0u;
    flags |= s.instanceTest ? FUSE_RDN_FLAG_INSTANCE : 0u;
    flags |= s.preblur ? FUSE_RDN_FLAG_PREBLUR : 0u;
    c.flags = flags;
    c.atrousIterations = std::clamp(s.atrousIterations, 1u, kRdnMaxAtrous);
    c.historyTap = std::min(s.historyTap, c.atrousIterations - 1u);
    c.gradientIterations = s.gradients ? std::min(s.gradientIterations, kRdnMaxGradientIterations) : 0u;
    c.sigmaNormalD = s.sigmaNormalD;
    c.sigmaNormalS = s.sigmaNormalS;
    c.motionScale = s.motionScale;
    c.alphaD = s.alphaD;
    c.alphaS = s.alphaS;
    c.maxHistD = std::max(s.maxHistD, 1.f);
    c.maxHistS = std::max(s.maxHistS, 1.f);
    c.reprojNormal = s.reprojNormal;
    c.reprojDepth = s.reprojDepth;
    c.minReprojWeight = s.minReprojWeight;
    c.virtRough0 = s.virtRough0;
    c.virtRough1 = s.virtRough1;
    c.clampSigmaD = s.clampSigmaD;
    c.clampSigmaS = s.clampSigmaS;
    c.fireflyRatio = s.fireflyRatio;
    c.fireflySigma = s.fireflySigma;
    c.preblurRadiusD = s.preblurRadiusD;
    c.preblurRadiusS = s.preblurRadiusS;
    c.hitDistScale = s.hitDistScale;
    c.fixFrames = s.fixFrames;
    c.fixMaxWeight = std::max(s.fixMaxWeight, 1.f);
    c.varianceHistory = s.varianceHistory;
    c.varianceBoost = s.varianceBoost;
    c.sigmaPlane = s.sigmaPlane;
    c.sigmaLumD = s.sigmaLumD;
    c.sigmaLumS = s.sigmaLumS;
    c.specRadiusMin = s.specRadiusMin;
    c.specRadiusMax = s.specRadiusMax;
    c.gradientScale = s.gradientScale;
    c.gradientEpsilon = s.gradientEpsilon;
    c.depthEpsilon = s.depthEpsilon;
    c.sigmaRoughness = s.sigmaRoughness;
    return c;
}

RdnBufferLayout RdnBufferLayout::compute(u32 width, u32 height, bool keepIntermediates) {
    RdnBufferLayout l{};
    l.width = width;
    l.height = height;
    l.strataW = (width + kRdnStratum - 1u) / kRdnStratum;
    l.strataH = (height + kRdnStratum - 1u) / kRdnStratum;
    l.keepIntermediates = keepIntermediates;
    const u64 n = static_cast<u64>(width) * height;
    const u64 ns = static_cast<u64>(l.strataW) * l.strataH;
    u64 cursor = 0;
    auto take = [&cursor](u64 bytes) {
        const u64 at = cursor;
        cursor = align256(cursor + std::max<u64>(bytes, 16u));
        return at;
    };
    for (u32 k = 0; k < 2u; ++k) {
        l.guide[k] = take(n * kTexel);
        l.aux[k] = take(n * kTexel);
        l.histD[k] = take(n * kTexel);
        l.histS[k] = take(n * kTexel);
        l.mom[k] = take(n * kTexel);
    }
    l.stateBytes = cursor;
    cursor = 0;
    l.preD = take(n * kTexel);
    l.preS = take(n * kTexel);
    l.blurD = take(n * kTexel);
    l.blurS = take(n * kTexel);
    l.accD = take(n * kTexel);
    l.accS = take(n * kTexel);
    for (u32 k = 0; k < kRdnMipLevels; ++k) {
        l.mipW[k] = (width + (2u << k) - 1u) >> (k + 1u);
        l.mipH[k] = (height + (2u << k) - 1u) >> (k + 1u);
        l.mip[k] = take(static_cast<u64>(l.mipW[k]) * l.mipH[k] * 4u * kTexel);
    }
    l.fixD = take(n * kTexel);
    l.fixS = take(n * kTexel);
    l.varD = take(n * kTexel);
    l.varS = take(n * kTexel);
    if (keepIntermediates) {
        for (u32 k = 0; k < kRdnMaxAtrous; ++k) {
            l.atrousD[k] = take(n * kTexel);
            l.atrousS[k] = take(n * kTexel);
        }
        for (u64& g : l.gradient) {
            g = take(ns * kTexel);
        }
    } else {
        const u64 pd[2] = {take(n * kTexel), take(n * kTexel)};
        const u64 ps[2] = {take(n * kTexel), take(n * kTexel)};
        for (u32 k = 0; k < kRdnMaxAtrous; ++k) {
            l.atrousD[k] = pd[k & 1u];
            l.atrousS[k] = ps[k & 1u];
        }
        const u64 g[2] = {take(ns * kTexel), take(ns * kTexel)};
        for (u32 k = 0; k <= kRdnMaxGradientIterations; ++k) {
            l.gradient[k] = g[k & 1u];
        }
    }
    l.workBytes = cursor;
    cursor = 0;
    l.outD = take(n * kTexel);
    l.outS = take(n * kTexel);
    l.outputBytes = cursor;
    return l;
}

void rdn_bind_arenas(RdnFrame& c, const RdnBufferLayout& l, u64 state, u64 work, u64 output, u32 parity) {
    const u32 cur = parity & 1u;
    const u32 prev = cur ^ 1u;
    c.guideCur = state + l.guide[cur];
    c.guidePrev = state + l.guide[prev];
    c.auxCur = state + l.aux[cur];
    c.auxPrev = state + l.aux[prev];
    c.histDCur = state + l.histD[cur];
    c.histDPrev = state + l.histD[prev];
    c.histSCur = state + l.histS[cur];
    c.histSPrev = state + l.histS[prev];
    c.momCur = state + l.mom[cur];
    c.momPrev = state + l.mom[prev];
    c.preD = work + l.preD;
    c.preS = work + l.preS;
    c.blurD = work + l.blurD;
    c.blurS = work + l.blurS;
    c.accD = work + l.accD;
    c.accS = work + l.accS;
    c.mip1 = work + l.mip[0];
    c.mip2 = work + l.mip[1];
    c.mip3 = work + l.mip[2];
    c.fixD = work + l.fixD;
    c.fixS = work + l.fixS;
    c.varD = work + l.varD;
    c.varS = work + l.varS;
    c.lambda = work + l.gradient[c.gradientIterations];
    (void)output;
}

u32 rdn_build_passes(const RdnFrame& c, const RdnBufferLayout& l, u64 work, u64 output, RdnPassDesc* out, u32 max) {
    u32 n = 0;
    auto add = [&](const char* name, u32 pass, u32 gw, u32 gh) -> RdnPassDesc* {
        if (n >= max) {
            return nullptr;
        }
        RdnPassDesc* p = &out[n++];
        *p = RdnPassDesc{};
        p->name = name;
        p->args.pass = pass;
        p->gridW = gw;
        p->gridH = gh;
        return p;
    };
    const u32 w = c.width;
    const u32 h = c.height;
    if (RdnPassDesc* p = add("rdn.prepare", FUSE_RDN_PASS_PREPARE, w, h)) {
        p->readsInputs = true;
    }
    add("rdn.preblur", FUSE_RDN_PASS_PREBLUR, w, h);
    if ((c.flags & FUSE_RDN_FLAG_GRADIENTS) != 0u) {
        if (RdnPassDesc* p = add("rdn.gradient.prepare", FUSE_RDN_PASS_GRAD_PREPARE, c.strataW, c.strataH)) {
            p->args.a[2] = work + l.gradient[0];
            p->readsInputs = true;
        }
        for (u32 k = 0; k < c.gradientIterations; ++k) {
            if (RdnPassDesc* p = add("rdn.gradient.atrous", FUSE_RDN_PASS_GRAD_ATROUS, c.strataW, c.strataH)) {
                p->args.a[0] = work + l.gradient[k];
                p->args.a[2] = work + l.gradient[k + 1u];
                p->args.step = 1u << k;
            }
        }
    }
    if (RdnPassDesc* p = add("rdn.temporal", FUSE_RDN_PASS_TEMPORAL, w, h)) {
        p->readsInputs = true;
    }
    for (u32 k = 0; k < kRdnMipLevels; ++k) {
        if (RdnPassDesc* p = add("rdn.mip", FUSE_RDN_PASS_MIP, l.mipW[k], l.mipH[k])) {
            p->args.a[0] = k == 0u ? 0u : work + l.mip[k - 1u];
            p->args.a[2] = work + l.mip[k];
            p->args.step = k + 1u;
        }
    }
    add("rdn.historyfix", FUSE_RDN_PASS_HISTFIX, w, h);
    add("rdn.variance", FUSE_RDN_PASS_VARIANCE, w, h);
    for (u32 k = 0; k < c.atrousIterations; ++k) {
        RdnPassDesc* p = add("rdn.atrous", FUSE_RDN_PASS_ATROUS, w, h);
        if (p == nullptr) {
            break;
        }
        const bool last = k + 1u == c.atrousIterations;
        p->args.a[0] = k == 0u ? c.varD : work + l.atrousD[k - 1u];
        p->args.a[1] = k == 0u ? c.varS : work + l.atrousS[k - 1u];
        p->args.a[2] = last ? output + l.outD : work + l.atrousD[k];
        p->args.a[3] = last ? output + l.outS : work + l.atrousS[k];
        p->args.a[4] = k == c.historyTap ? c.histDCur : 0u;
        p->args.a[5] = k == c.historyTap ? c.histSCur : 0u;
        p->args.step = 1u << k;
        p->writesOutput = last;
    }
    return n;
}

void RdnReference::init(u32 width, u32 height, const RdnSettings& settings, bool keepIntermediates) {
    m_settings = settings;
    m_layout = RdnBufferLayout::compute(width, height, keepIntermediates);
    m_state.assign(m_layout.stateBytes / 16u, rdnk::float4{});
    m_work.assign(m_layout.workBytes / 16u, rdnk::float4{});
    m_output.assign(m_layout.outputBytes / 16u, rdnk::float4{});
    m_history = false;
    m_parity = 1u;
}

void RdnReference::runPass(const RdnFrame& c, const RdnPassDesc& pass, kernel::Backend backend) {
    rdnk::RdnKernelParams p{};
    p.frame = c;
    p.args = pass.args;
    const kernel::KernelLaunch launch{pass.name, kernel::extent2(pass.gridW, pass.gridH), {kRdnTile, kRdnTile, 1u}};
    kernel::launch(backend, launch, rdnk::RdnKernel{}, p);
}

bool RdnReference::runFrame(const RdnReferenceInputs& in, const RdnCamera& camera, const RdnCamera& prevCamera,
                            kernel::Backend backend) {
    if (m_layout.width == 0u || in.diffuse == nullptr || in.specular == nullptr || in.normal == nullptr ||
        in.depth == nullptr || in.motion == nullptr || (m_settings.gradients && in.gradient == nullptr) ||
        (m_settings.instanceTest && in.instance == nullptr)) {
        return false;
    }
    if (in.reset) {
        m_history = false;
    }
    m_parity ^= 1u;
    RdnFrame c = rdn_resolve(m_settings, m_layout.width, m_layout.height, m_history, camera, prevCamera);
    c.inDiffuse = rdnk::rdnAddr(in.diffuse);
    c.inSpecular = rdnk::rdnAddr(in.specular);
    c.inNormal = rdnk::rdnAddr(in.normal);
    c.inDepth = rdnk::rdnAddr(in.depth);
    c.inMotion = rdnk::rdnAddr(in.motion);
    c.inInstance = m_settings.instanceTest ? rdnk::rdnAddr(in.instance) : 0u;
    c.inGradient = m_settings.gradients ? rdnk::rdnAddr(in.gradient) : 0u;
    const u64 state = rdnk::rdnAddr(m_state.data());
    const u64 work = rdnk::rdnAddr(m_work.data());
    const u64 output = rdnk::rdnAddr(m_output.data());
    rdn_bind_arenas(c, m_layout, state, work, output, m_parity);
    m_constants = c;
    const u32 count = rdn_build_passes(c, m_layout, work, output, m_passes, kRdnMaxPasses);
    for (u32 k = 0; k < count; ++k) {
        runPass(c, m_passes[k], backend);
    }
    m_history = true;
    return true;
}

} // namespace fuse::renderer::denoise
