// FUSE Relight RL-5.6: the volumetrics / particle composite CPU reference (volumetrics.hpp).
#include <fuse/relight/render/volumetrics/volumetrics.hpp>

#include "vol_cpp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::relight::render::volumetrics {

namespace {

void normalize3(float v[3]) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// The path tracer's camera basis (PtCompiledScene::packParams): right x tanX, up x tanY, unit forward.
void cameraBasis(const pathtrace::PtCamera& c, u32 width, u32 height, float r[3], float u[3], float f[3]) {
    std::memcpy(f, c.forward, sizeof(float) * 3u);
    normalize3(f);
    const float up[3] = {c.up[0], c.up[1], c.up[2]};
    if (c.leftHanded) {
        cross3(up, f, r);
        normalize3(r);
        cross3(f, r, u);
    } else {
        cross3(f, up, r);
        normalize3(r);
        cross3(r, f, u);
    }
    normalize3(u);
    const float aspect = c.aspect > 0.f ? c.aspect : float(std::max(width, 1u)) / float(std::max(height, 1u));
    const float ty = std::tan(0.5f * c.fovY);
    for (int k = 0; k < 3; ++k) {
        r[k] *= ty * aspect;
        u[k] *= ty;
    }
}

} // namespace

VolMedium mediumFromD3dFog(const D3dFogState& fog, float sceneScale) {
    VolMedium m;
    const float scale = sceneScale > 0.f ? sceneScale : 1.f;
    float sigma = 0.f;
    switch (fog.mode) {
    case D3dFogMode::Exp:
    case D3dFogMode::Exp2:
        sigma = std::max(fog.density, 0.f);
        break;
    case D3dFogMode::Linear: {
        const float mid = 0.5f * (fog.start + fog.end);
        sigma = fog.end > fog.start && mid > 0.f ? 0.693147180559945f / mid : 0.f;
        break;
    }
    default:
        break;
    }
    m.density = sigma / scale;
    for (int k = 0; k < 3; ++k) {
        m.albedo[k] = 1.f;
        m.ambient[k] = std::max(fog.color[k], 0.f);
    }
    return m;
}

void packVolParams(const VolFrameDesc& d, u32 lightCount, Word* params, VolUint4* ints) {
    float r[3], u[3], f[3], pr[3], pu[3], pf[3];
    cameraBasis(d.camera, d.width, d.height, r, u, f);
    cameraBasis(d.prevCamera, d.width, d.height, pr, pu, pf);
    const float* o = d.camera.origin;
    const float* po = d.prevCamera.origin;
    const VolMedium& m = d.medium;
    const float g = std::min(std::max(m.anisotropy, -0.95f), 0.95f);
    const float nearZ = std::max(d.nearZ, 1e-4f);
    const float farZ = std::max(d.farZ, nearZ * 1.001f);
    params[0] = Word(o[0], o[1], o[2], nearZ);
    params[1] = Word(r[0], r[1], r[2], farZ);
    params[2] = Word(u[0], u[1], u[2], g);
    params[3] = Word(f[0], f[1], f[2], std::min(std::max(d.temporalAlpha, 0.f), 1.f));
    params[4] = Word(po[0], po[1], po[2], m.falloff);
    params[5] = Word(pr[0], pr[1], pr[2], m.baseHeight);
    params[6] = Word(pu[0], pu[1], pu[2], std::max(m.density, 0.f));
    params[7] = Word(pf[0], pf[1], pf[2], d.rayEps);
    params[8] = Word(m.albedo[0], m.albedo[1], m.albedo[2], d.mCap);
    params[9] = Word(m.ambient[0], m.ambient[1], m.ambient[2], d.lightScale);
    params[10] = Word(0.f, 0.f, 0.f, 0.f);
    ints[0] = VolUint4{std::max(d.gridX, 1u), std::max(d.gridY, 1u), std::max(d.gridZ, 1u), d.flags};
    ints[1] = VolUint4{d.frame, std::min(d.candidates, 32u), d.width, d.height};
    ints[2] = VolUint4{std::min(d.systemCount, kVolSystemCount), lightCount, 0u, 0u};
}

void VolumetricsCpu::resize(u32 gridX, u32 gridY, u32 gridZ, u32 width, u32 height) {
    const bool same = gridX == m_gx && gridY == m_gy && gridZ == m_gz && width == m_w && height == m_h;
    if (same) {
        return;
    }
    m_gx = gridX;
    m_gy = gridY;
    m_gz = gridZ;
    m_w = width;
    m_h = height;
    const std::size_t froxels = std::size_t(gridX) * gridY * gridZ;
    const std::size_t pixels = std::size_t(width) * height;
    const Word zero(0.f, 0.f, 0.f, 0.f);
    m_buffers[kVolCurrent].assign(froxels, zero);
    m_buffers[kVolHistPrev].assign(froxels, zero);
    m_buffers[kVolHistCur].assign(froxels, zero);
    m_buffers[kVolResPrev].assign(froxels * 2u, zero);
    m_buffers[kVolResCur].assign(froxels * 2u, zero);
    m_buffers[kVolIntegrated].assign(froxels, zero);
    m_buffers[kVolColorIn].assign(pixels, zero);
    m_buffers[kVolColorOut].assign(pixels, zero);
    m_historyValid = false;
}

void VolumetricsCpu::swapHistory() {
    m_buffers[kVolHistPrev].swap(m_buffers[kVolHistCur]);
    m_buffers[kVolResPrev].swap(m_buffers[kVolResCur]);
    m_historyValid = true;
}

bool VolumetricsCpu::runStage(u32 stage, const VolFrameDesc& desc, const VolCpuInputs& in, bool history) {
    if (desc.gridX != m_gx || desc.gridY != m_gy || desc.gridZ != m_gz || desc.width != m_w || desc.height != m_h ||
        m_gx == 0u || m_gy == 0u || m_gz == 0u) {
        return false;
    }
    const u32 lightCount = in.lights != nullptr ? in.lights->lightCount() : 0u;
    Word params[kVolParamWordCount];
    VolUint4 ints[kVolIntWordCount];
    VolFrameDesc d = desc;
    d.flags = history ? (d.flags | kVolHistory) : (d.flags & ~u32(kVolHistory));
    packVolParams(d, lightCount, params, ints);
    volk::uint4 iw[kVolIntWordCount];
    for (u32 k = 0; k < kVolIntWordCount; ++k) {
        iw[k] = volk::uint4{ints[k].x, ints[k].y, ints[k].z, ints[k].w};
    }
    const volk::VolParams P = volk::volParamsUnpack(params, iw);
    volk::uint4 systems[kVolSystemCount];
    for (u32 s = 0; s < kVolSystemCount; ++s) {
        systems[s] = volk::uint4{desc.systems[s].firstQuad, desc.systems[s].quadCount, desc.systems[s].blend,
                                 desc.systems[s].flags};
    }
    volk::VolCpuContext ctx;
    for (u32 b = 0; b < kVolBufferCount; ++b) {
        ctx.buffers[b] = m_buffers[b].data();
    }
    ctx.depth = in.depth;
    ctx.vertices = in.vertices.data();
    ctx.vertexCount = static_cast<u32>(in.vertices.size());
    ctx.systems = systems;
    ctx.lights = in.lights;
    ctx.scene = in.scene;
    switch (stage) {
    case kVolInject:
        for (u32 y = 0; y < m_gy; ++y) {
            for (u32 x = 0; x < m_gx; ++x) {
                for (u32 z = 0; z < m_gz; ++z) {
                    volk::volInject(ctx, P, x, y, z);
                }
            }
        }
        return true;
    case kVolTemporal:
        for (u32 y = 0; y < m_gy; ++y) {
            for (u32 x = 0; x < m_gx; ++x) {
                for (u32 z = 0; z < m_gz; ++z) {
                    volk::volTemporal(ctx, P, x, y, z);
                }
            }
        }
        return true;
    case kVolIntegrate:
        for (u32 y = 0; y < m_gy; ++y) {
            for (u32 x = 0; x < m_gx; ++x) {
                volk::volIntegrate(ctx, P, x, y);
            }
        }
        return true;
    case kVolApply:
        if (in.color != nullptr) {
            std::copy(in.color, in.color + std::size_t(m_w) * m_h, m_buffers[kVolColorIn].begin());
        }
        for (u32 y = 0; y < m_h; ++y) {
            for (u32 x = 0; x < m_w; ++x) {
                volk::volApply(ctx, P, x, y);
            }
        }
        return true;
    default:
        return false;
    }
}

bool VolumetricsCpu::run(const VolFrameDesc& desc, const VolCpuInputs& in) {
    const bool history = m_historyValid;
    for (u32 s = kVolInject; s <= kVolApply; ++s) {
        if (!runStage(s, desc, in, history)) {
            return false;
        }
    }
    swapHistory();
    return true;
}

lk::float3 referenceInScatter(const lights::RelightLightSet& set, const lk::float3& p, const lk::float3& view, float g,
                              u32 samples, lk::float3* standardError) {
    lk::float3 mean(0.f, 0.f, 0.f);
    double var[3] = {0.0, 0.0, 0.0};
    const float gc = std::min(std::max(g, -0.95f), 0.95f);
    for (u32 l = 0; l < set.lightCount(); ++l) {
        const lk::RlLight L = set.light(l);
        double sum[3] = {0.0, 0.0, 0.0};
        double sq[3] = {0.0, 0.0, 0.0};
        for (u32 s = 0; s < samples; ++s) {
            const float u1 = (float(volk::volHash(s * 2u + 1u + l * 7919u) >> 8u) + 0.5f) * (1.f / 16777216.f);
            const float u2 = (float(volk::volHash(s * 2u + 2u + l * 104729u) >> 8u) + 0.5f) * (1.f / 16777216.f);
            const lk::RlLightSample ls = lk::rlLightSample(L, p, u1, u2);
            double c[3] = {0.0, 0.0, 0.0};
            if ((ls.flags & lk::kRlSampleValid) != 0u && ls.pdf > 0.f) {
                const float ph = volk::volPhaseHG(gc, lk::dot(ls.wi, view)) * L.volumetricScale / ls.pdf;
                c[0] = double(ls.radiance.x) * ph;
                c[1] = double(ls.radiance.y) * ph;
                c[2] = double(ls.radiance.z) * ph;
            }
            for (int k = 0; k < 3; ++k) {
                sum[k] += c[k];
                sq[k] += c[k] * c[k];
            }
        }
        for (int k = 0; k < 3; ++k) {
            const double m = sum[k] / samples;
            mean[k] += static_cast<float>(m);
            var[k] += std::max(sq[k] / samples - m * m, 0.0) / samples;
        }
    }
    if (standardError != nullptr) {
        *standardError = lk::float3(float(std::sqrt(var[0])), float(std::sqrt(var[1])), float(std::sqrt(var[2])));
    }
    return mean;
}

} // namespace fuse::relight::render::volumetrics
