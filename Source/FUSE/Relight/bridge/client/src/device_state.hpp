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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix bridge/src/client/d3d9_device_base.h (State, StateCaptureDirtyFlags),
// bridge/src/client/d3d9_stateblock.cpp (StateTransfer), bridge/src/client/d3d9_device.cpp
// (ResetState, StateBlockSet{Pixel,Vertex,}CaptureFlags)@0867d3c

// FUSE Relight RL-2.2: the client's copy of the device state (answers every Get* without a round
// trip) and the state-block masks.
//
// Semantics kept from upstream: the D3D9 default values of ResetState; the pixel / vertex / all
// capture sets of CreateStateBlock; Capture copies the masked state from the device into the block,
// Apply copies it back; lights and light enables are captured per index.
//
// Changes (revamp):
// - While BeginStateBlock recording is active, Set* calls go into the recorded block (value + mask)
//   and leave the device state unchanged, as D3D9 and DXVK do. Upstream kept applying them to the
//   device copy, so Get* during recording returned the recorded values.
// - Masks are bitsets with the same shape as the state (one struct, one transfer function).
// - The render target / depth-stencil bindings are not part of state blocks (they are not in D3D9).
#pragma once

#include <d3d9.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <vector>

namespace fuse::relight::bridge::client {

class BridgeObject;

// A private (internal) reference: keeps a bound object alive in the client without changing the
// refcount the game sees (upstream D3DAutoPtr).
class PrivateRef {
public:
    PrivateRef() = default;
    explicit PrivateRef(BridgeObject* p) { reset(p); }
    PrivateRef(const PrivateRef& o) { reset(o.p_); }
    PrivateRef& operator=(const PrivateRef& o) {
        if (this != &o) {
            reset(o.p_);
        }
        return *this;
    }
    ~PrivateRef() { reset(nullptr); }
    void reset(BridgeObject* p);
    BridgeObject* get() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

private:
    BridgeObject* p_ = nullptr;
};

inline constexpr uint32_t kRenderStates = 256;
inline constexpr uint32_t kSamplers = 21;        // 0..15, DMAP, vertex 0..3 (protocol.hpp samplerSlot)
inline constexpr uint32_t kSamplerStates = 14;   // D3DSAMP_ADDRESSU (1) .. D3DSAMP_DMAPOFFSET (13)
inline constexpr uint32_t kStages = 8;
inline constexpr uint32_t kStageStates = 33;     // .. D3DTSS_CONSTANT (32)
inline constexpr uint32_t kTransforms = 512;     // D3DTS_* < 256, D3DTS_WORLDMATRIX(0..255) = 256..511
inline constexpr uint32_t kClipPlanes = 6;
inline constexpr uint32_t kStreams = 16;
inline constexpr uint32_t kVsFloatConstsHw = 256;
inline constexpr uint32_t kVsFloatConstsSw = 8192;
inline constexpr uint32_t kPsFloatConsts = 224;
inline constexpr uint32_t kIntConsts = 16;
inline constexpr uint32_t kBoolConsts = 16;
inline constexpr uint32_t kBoolConstsSw = 2048;
inline constexpr uint32_t kIntConstsSw = 2048;
inline constexpr uint32_t kRenderTargets = 4;

struct DeviceState {
    std::array<DWORD, kRenderStates> renderStates {};
    std::array<std::array<DWORD, kSamplerStates>, kSamplers> samplerStates {};
    std::array<std::array<DWORD, kStageStates>, kStages> stageStates {};
    std::array<PrivateRef, kSamplers> textures;
    std::vector<D3DMATRIX> transforms = std::vector<D3DMATRIX>(kTransforms);
    D3DVIEWPORT9 viewport {};
    D3DMATERIAL9 material {};
    std::map<DWORD, D3DLIGHT9> lights;
    std::map<DWORD, BOOL> lightEnables;
    std::array<std::array<float, 4>, kClipPlanes> clipPlanes {};
    RECT scissorRect {};
    PrivateRef vertexDecl;
    DWORD fvf = 0;
    PrivateRef vertexShader;
    PrivateRef pixelShader;
    PrivateRef indices;
    std::array<PrivateRef, kStreams> streams;
    std::array<UINT, kStreams> streamOffsets {};
    std::array<UINT, kStreams> streamStrides {};
    std::array<UINT, kStreams> streamFreqs {};
    std::vector<float> vsFloat;   // 4 per register
    std::vector<int> vsInt;       // 4 per register
    std::vector<BOOL> vsBool;
    std::vector<float> psFloat = std::vector<float>(kPsFloatConsts * 4);
    std::vector<int> psInt = std::vector<int>(kIntConsts * 4);
    std::vector<BOOL> psBool = std::vector<BOOL>(kBoolConsts);

    void sizeConstants(bool softwareVp) {
        vsFloat.assign((softwareVp ? kVsFloatConstsSw : kVsFloatConstsHw) * 4, 0.0f);
        vsInt.assign((softwareVp ? kIntConstsSw : kIntConsts) * 4, 0);
        vsBool.assign(softwareVp ? kBoolConstsSw : kBoolConsts, FALSE);
    }
};

// Which parts of a DeviceState a state block holds.
struct StateMask {
    std::bitset<kRenderStates> renderStates;
    std::array<std::bitset<kSamplerStates>, kSamplers> samplerStates;
    std::array<std::bitset<kStageStates>, kStages> stageStates;
    std::bitset<kSamplers> textures;
    std::bitset<kTransforms> transforms;
    bool viewport = false;
    bool material = false;
    std::map<DWORD, bool> lights;         // captured light indices
    std::map<DWORD, bool> lightEnables;
    bool allLights = false;               // D3DSBT_VERTEXSTATE / ALL: every light that exists
    std::bitset<kClipPlanes> clipPlanes;
    bool scissorRect = false;
    bool vertexDecl = false;
    bool vertexShader = false;
    bool pixelShader = false;
    bool indices = false;
    std::bitset<kStreams> streams;
    std::bitset<kStreams> streamFreqs;
    std::vector<bool> vsFloat, vsInt, vsBool;
    std::vector<bool> psFloat = std::vector<bool>(kPsFloatConsts);
    std::vector<bool> psInt = std::vector<bool>(kIntConsts);
    std::vector<bool> psBool = std::vector<bool>(kBoolConsts);

    void sizeConstants(bool softwareVp) {
        vsFloat.assign(softwareVp ? kVsFloatConstsSw : kVsFloatConstsHw, false);
        vsInt.assign(softwareVp ? kIntConstsSw : kIntConsts, false);
        vsBool.assign(softwareVp ? kBoolConstsSw : kBoolConsts, false);
    }
};

// D3D9 defaults (upstream ResetState). `autoDepth`: D3DRS_ZENABLE follows EnableAutoDepthStencil.
void resetDeviceState(DeviceState& s, bool autoDepth);
// The CreateStateBlock(type) capture set (upstream StateBlockSetCaptureFlags).
void stateBlockMask(D3DSTATEBLOCKTYPE type, StateMask& m);
// Copies the masked state from `src` into `dst` (upstream StateTransfer).
void transferState(const StateMask& m, const DeviceState& src, DeviceState& dst);

// Transform index of a D3DTRANSFORMSTATETYPE (false: not a valid transform).
inline bool transformSlot(D3DTRANSFORMSTATETYPE t, uint32_t& slot) {
    const uint32_t v = static_cast<uint32_t>(t);
    if (v < kTransforms) {
        slot = v;
        return true;
    }
    return false;
}

}  // namespace fuse::relight::bridge::client
