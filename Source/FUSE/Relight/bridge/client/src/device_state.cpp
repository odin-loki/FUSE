/*
 * Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/client/d3d9_device.cpp (ResetState,
// StateBlockSet{Pixel,Vertex,}CaptureFlags), bridge/src/client/d3d9_stateblock.cpp
// (StateTransfer)@0867d3c

// FUSE Relight RL-2.2: shadow device state defaults and state-block transfer (device_state.hpp).
#include "device_state.hpp"

#include "objects.hpp"

#include <cstring>

namespace fuse::relight::bridge::client {

namespace {
DWORD f2dw(float f) {
    DWORD d;
    std::memcpy(&d, &f, 4);
    return d;
}
}  // namespace

void PrivateRef::reset(BridgeObject* p) {
    if (p == p_) {
        return;
    }
    if (p != nullptr) {
        p->addRefPrivate();
    }
    BridgeObject* old = p_;
    p_ = p;
    if (old != nullptr) {
        old->releasePrivate();
    }
}

void resetDeviceState(DeviceState& s, bool autoDepth) {
    for (uint32_t stage = 0; stage < kStages; ++stage) {
        auto& t = s.stageStates[stage];
        t.fill(0);
        t[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_BUMPENVMAT00] = f2dw(0.0f);
        t[D3DTSS_BUMPENVMAT01] = f2dw(0.0f);
        t[D3DTSS_BUMPENVMAT10] = f2dw(0.0f);
        t[D3DTSS_BUMPENVMAT11] = f2dw(0.0f);
        t[D3DTSS_TEXCOORDINDEX] = stage;
        t[D3DTSS_BUMPENVLSCALE] = f2dw(0.0f);
        t[D3DTSS_BUMPENVLOFFSET] = f2dw(0.0f);
        t[D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
        t[D3DTSS_COLORARG0] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
        t[D3DTSS_CONSTANT] = 0;
    }
    for (uint32_t i = 0; i < kSamplers; ++i) {
        auto& ss = s.samplerStates[i];
        ss.fill(0);
        ss[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_BORDERCOLOR] = 0;
        ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[D3DSAMP_MIPMAPLODBIAS] = 0;
        ss[D3DSAMP_MAXMIPLEVEL] = 0;
        ss[D3DSAMP_MAXANISOTROPY] = 1;
        ss[D3DSAMP_SRGBTEXTURE] = 0;
        ss[D3DSAMP_ELEMENTINDEX] = 0;
        ss[D3DSAMP_DMAPOFFSET] = 0;
    }
    auto& rs = s.renderStates;
    rs.fill(0);
    rs[D3DRS_ZENABLE] = autoDepth ? D3DZB_TRUE : D3DZB_FALSE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_ALPHATESTENABLE] = FALSE;
    rs[D3DRS_LASTPIXEL] = TRUE;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAREF] = 0;
    rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_DITHERENABLE] = FALSE;
    rs[D3DRS_ALPHABLENDENABLE] = FALSE;
    rs[D3DRS_FOGENABLE] = FALSE;
    rs[D3DRS_SPECULARENABLE] = FALSE;
    rs[D3DRS_FOGCOLOR] = 0;
    rs[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
    rs[D3DRS_FOGSTART] = f2dw(0.0f);
    rs[D3DRS_FOGEND] = f2dw(1.0f);
    rs[D3DRS_FOGDENSITY] = f2dw(1.0f);
    rs[D3DRS_RANGEFOGENABLE] = FALSE;
    rs[D3DRS_STENCILENABLE] = FALSE;
    rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILREF] = 0;
    rs[D3DRS_STENCILMASK] = 0xFFFFFFFF;
    rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[D3DRS_CLIPPING] = TRUE;
    rs[D3DRS_LIGHTING] = TRUE;
    rs[D3DRS_AMBIENT] = 0;
    rs[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
    rs[D3DRS_COLORVERTEX] = TRUE;
    rs[D3DRS_LOCALVIEWER] = TRUE;
    rs[D3DRS_NORMALIZENORMALS] = FALSE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
    rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    rs[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
    rs[D3DRS_CLIPPLANEENABLE] = 0;
    rs[D3DRS_POINTSIZE] = f2dw(1.0f);
    rs[D3DRS_POINTSIZE_MIN] = f2dw(1.0f);
    rs[D3DRS_POINTSPRITEENABLE] = FALSE;
    rs[D3DRS_POINTSCALEENABLE] = FALSE;
    rs[D3DRS_POINTSCALE_A] = f2dw(1.0f);
    rs[D3DRS_POINTSCALE_B] = f2dw(0.0f);
    rs[D3DRS_POINTSCALE_C] = f2dw(0.0f);
    rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    rs[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
    rs[D3DRS_DEBUGMONITORTOKEN] = D3DDMT_ENABLE;
    rs[D3DRS_POINTSIZE_MAX] = f2dw(8192.0f);
    rs[D3DRS_INDEXEDVERTEXBLENDENABLE] = FALSE;
    rs[D3DRS_COLORWRITEENABLE] = 0x0000000F;
    rs[D3DRS_TWEENFACTOR] = f2dw(0.0f);
    rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    rs[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
    rs[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
    rs[D3DRS_SCISSORTESTENABLE] = FALSE;
    rs[D3DRS_SLOPESCALEDEPTHBIAS] = 0;
    rs[D3DRS_ANTIALIASEDLINEENABLE] = FALSE;
    rs[D3DRS_MINTESSELLATIONLEVEL] = f2dw(1.0f);
    rs[D3DRS_MAXTESSELLATIONLEVEL] = f2dw(1.0f);
    rs[D3DRS_ADAPTIVETESS_X] = f2dw(0.0f);
    rs[D3DRS_ADAPTIVETESS_Y] = f2dw(0.0f);
    rs[D3DRS_ADAPTIVETESS_Z] = f2dw(1.0f);
    rs[D3DRS_ADAPTIVETESS_W] = f2dw(0.0f);
    rs[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
    rs[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
    rs[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_COLORWRITEENABLE1] = 0x0000000F;
    rs[D3DRS_COLORWRITEENABLE2] = 0x0000000F;
    rs[D3DRS_COLORWRITEENABLE3] = 0x0000000F;
    rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    rs[D3DRS_SRGBWRITEENABLE] = 0;
    rs[D3DRS_DEPTHBIAS] = f2dw(0.0f);
    rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
    rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;

    D3DMATRIX identity {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    for (D3DMATRIX& m : s.transforms) {
        m = identity;
    }
    s.material = D3DMATERIAL9 {};
    s.lights.clear();
    s.lightEnables.clear();
    for (auto& p : s.clipPlanes) {
        p.fill(0.0f);
    }
    for (auto& t : s.textures) {
        t.reset(nullptr);
    }
    s.vertexDecl.reset(nullptr);
    s.fvf = 0;
    s.vertexShader.reset(nullptr);
    s.pixelShader.reset(nullptr);
    s.indices.reset(nullptr);
    for (uint32_t i = 0; i < kStreams; ++i) {
        s.streams[i].reset(nullptr);
        s.streamOffsets[i] = 0;
        s.streamStrides[i] = 0;
        s.streamFreqs[i] = 1;
    }
    std::fill(s.vsFloat.begin(), s.vsFloat.end(), 0.0f);
    std::fill(s.vsInt.begin(), s.vsInt.end(), 0);
    std::fill(s.vsBool.begin(), s.vsBool.end(), FALSE);
    std::fill(s.psFloat.begin(), s.psFloat.end(), 0.0f);
    std::fill(s.psInt.begin(), s.psInt.end(), 0);
    std::fill(s.psBool.begin(), s.psBool.end(), FALSE);
}

namespace {

void pixelMask(StateMask& m) {
    static const D3DRENDERSTATETYPE kPixelRs[] = {
        D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE,
        D3DRS_LASTPIXEL, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
        D3DRS_DITHERENABLE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_ALPHABLENDENABLE,
        D3DRS_DEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS,
        D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR,
        D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7,
        D3DRS_WRAP8, D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12, D3DRS_WRAP13, D3DRS_WRAP14,
        D3DRS_WRAP15, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE, D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
        D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA};
    for (D3DRENDERSTATETYPE rs : kPixelRs) {
        m.renderStates.set(rs);
    }
    static const D3DSAMPLERSTATETYPE kPixelSs[] = {
        D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_BORDERCOLOR, D3DSAMP_MAGFILTER,
        D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY,
        D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX};
    for (uint32_t i = 0; i < 17; ++i) {  // pixel samplers + DMAP (upstream MaxTexturesPS + 1)
        for (D3DSAMPLERSTATETYPE t : kPixelSs) {
            m.samplerStates[i].set(t);
        }
    }
    std::fill(m.psFloat.begin(), m.psFloat.end(), true);
    std::fill(m.psInt.begin(), m.psInt.end(), true);
    std::fill(m.psBool.begin(), m.psBool.end(), true);
    for (auto& st : m.stageStates) {
        st.set();
    }
    m.pixelShader = true;  // DXVK/D3D9 capture the pixel shader with the pixel state (upstream did not)
}

void vertexMask(StateMask& m) {
    static const D3DRENDERSTATETYPE kVertexRs[] = {
        D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND,
        D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE,
        D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_LOCALVIEWER, D3DRS_EMISSIVEMATERIALSOURCE,
        D3DRS_AMBIENTMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_VERTEXBLEND,
        D3DRS_CLIPPLANEENABLE, D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE,
        D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C,
        D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_PATCHEDGESTYLE, D3DRS_POINTSIZE_MAX,
        D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR, D3DRS_POSITIONDEGREE, D3DRS_NORMALDEGREE,
        D3DRS_MINTESSELLATIONLEVEL, D3DRS_MAXTESSELLATIONLEVEL, D3DRS_ADAPTIVETESS_X, D3DRS_ADAPTIVETESS_Y,
        D3DRS_ADAPTIVETESS_Z, D3DRS_ADAPTIVETESS_W, D3DRS_ENABLEADAPTIVETESSELLATION, D3DRS_NORMALIZENORMALS,
        D3DRS_SPECULARENABLE, D3DRS_SHADEMODE};
    for (D3DRENDERSTATETYPE rs : kVertexRs) {
        m.renderStates.set(rs);
    }
    m.vertexDecl = true;
    m.vertexShader = true;  // as DXVK/D3D9 (upstream did not capture shaders)
    m.streamFreqs.set();
    m.allLights = true;
    for (uint32_t i = 17; i < kSamplers; ++i) {  // vertex samplers: DMAPOFFSET (upstream loop)
        m.samplerStates[i].set(D3DSAMP_DMAPOFFSET);
    }
    std::fill(m.vsFloat.begin(), m.vsFloat.end(), true);
    std::fill(m.vsInt.begin(), m.vsInt.end(), true);
    std::fill(m.vsBool.begin(), m.vsBool.end(), true);
}

template <class T>
void copyMasked(const std::vector<bool>& mask, const std::vector<T>& src, std::vector<T>& dst, size_t perEntry) {
    for (size_t i = 0; i < mask.size(); ++i) {
        if (mask[i]) {
            for (size_t k = 0; k < perEntry && i * perEntry + k < dst.size() && i * perEntry + k < src.size(); ++k) {
                dst[i * perEntry + k] = src[i * perEntry + k];
            }
        }
    }
}

}  // namespace

void stateBlockMask(D3DSTATEBLOCKTYPE type, StateMask& m) {
    if (type == D3DSBT_PIXELSTATE || type == D3DSBT_ALL) {
        pixelMask(m);
    }
    if (type == D3DSBT_VERTEXSTATE || type == D3DSBT_ALL) {
        vertexMask(m);
    }
    if (type == D3DSBT_ALL) {
        m.textures.set();
        m.streams.set();
        m.indices = true;
        m.viewport = true;
        m.scissorRect = true;
        m.clipPlanes.set();
        m.transforms.set();
        m.material = true;
    }
}

void transferState(const StateMask& m, const DeviceState& src, DeviceState& dst) {
    for (uint32_t i = 0; i < kRenderStates; ++i) {
        if (m.renderStates.test(i)) {
            dst.renderStates[i] = src.renderStates[i];
        }
    }
    for (uint32_t s = 0; s < kSamplers; ++s) {
        for (uint32_t t = 0; t < kSamplerStates; ++t) {
            if (m.samplerStates[s].test(t)) {
                dst.samplerStates[s][t] = src.samplerStates[s][t];
            }
        }
        if (m.textures.test(s)) {
            dst.textures[s] = src.textures[s];
        }
    }
    for (uint32_t s = 0; s < kStages; ++s) {
        for (uint32_t t = 0; t < kStageStates; ++t) {
            if (m.stageStates[s].test(t)) {
                dst.stageStates[s][t] = src.stageStates[s][t];
            }
        }
    }
    for (uint32_t i = 0; i < kTransforms; ++i) {
        if (m.transforms.test(i)) {
            dst.transforms[i] = src.transforms[i];
        }
    }
    if (m.viewport) {
        dst.viewport = src.viewport;
    }
    if (m.material) {
        dst.material = src.material;
    }
    if (m.allLights) {
        for (const auto& [k, v] : src.lights) {
            dst.lights[k] = v;
        }
        for (const auto& [k, v] : src.lightEnables) {
            dst.lightEnables[k] = v;
        }
    }
    for (const auto& [k, on] : m.lights) {
        auto it = src.lights.find(k);
        if (on && it != src.lights.end()) {
            dst.lights[k] = it->second;
        }
    }
    for (const auto& [k, on] : m.lightEnables) {
        auto it = src.lightEnables.find(k);
        if (on && it != src.lightEnables.end()) {
            dst.lightEnables[k] = it->second;
        }
    }
    for (uint32_t i = 0; i < kClipPlanes; ++i) {
        if (m.clipPlanes.test(i)) {
            dst.clipPlanes[i] = src.clipPlanes[i];
        }
    }
    if (m.scissorRect) {
        dst.scissorRect = src.scissorRect;
    }
    if (m.vertexDecl) {
        dst.vertexDecl = src.vertexDecl;
        dst.fvf = src.fvf;
    }
    if (m.vertexShader) {
        dst.vertexShader = src.vertexShader;
    }
    if (m.pixelShader) {
        dst.pixelShader = src.pixelShader;
    }
    if (m.indices) {
        dst.indices = src.indices;
    }
    for (uint32_t i = 0; i < kStreams; ++i) {
        if (m.streams.test(i)) {
            dst.streams[i] = src.streams[i];
            dst.streamOffsets[i] = src.streamOffsets[i];
            dst.streamStrides[i] = src.streamStrides[i];
        }
        if (m.streamFreqs.test(i)) {
            dst.streamFreqs[i] = src.streamFreqs[i];
        }
    }
    copyMasked(m.vsFloat, src.vsFloat, dst.vsFloat, 4);
    copyMasked(m.vsInt, src.vsInt, dst.vsInt, 4);
    copyMasked(m.vsBool, src.vsBool, dst.vsBool, 1);
    copyMasked(m.psFloat, src.psFloat, dst.psFloat, 4);
    copyMasked(m.psInt, src.psInt, dst.psInt, 4);
    copyMasked(m.psBool, src.psBool, dst.psBool, 1);
}

}  // namespace fuse::relight::bridge::client
