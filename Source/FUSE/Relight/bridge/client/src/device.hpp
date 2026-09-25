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
// Ported from dxvk-remix bridge/src/client/{d3d9_device.h,d3d9_device_base.h}@0867d3c

// FUSE Relight RL-2.2: the client IDirect3DDevice9Ex (see device.cpp for the per-method notes).
#pragma once

#include "objects.hpp"

#include <array>
#include <map>

namespace fuse::relight::bridge::client {

class Device final : public IDirect3DDevice9Ex, public BridgeObject {
public:
    Device(uint32_t handle, Interface* parent, bool ex, const D3DDEVICE_CREATION_PARAMETERS& cp, const D3DCAPS9& caps);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3DDevice9Ex*>(this); }

    // Creates the implicit swap chain / back buffers / auto depth-stencil and links them on the host.
    void initImplicit(const D3DPRESENT_PARAMETERS& pp);

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override;
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* caps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT swapChain, D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* bitmap) override;
    void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) override;
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override;
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index, IDirect3DSwapChain9** ppSwapChain) override;
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type,
                                            IDirect3DSurface9** ppBackBuffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* status) override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enable) override;
    void STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) override;
    void STDMETHODCALLTYPE GetGammaRamp(UINT swapChain, D3DGAMMARAMP* ramp) override;
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                            IDirect3DTexture9** ppTexture, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, IDirect3DVolumeTexture9** ppTexture, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DCubeTexture9** ppTexture, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer9** ppBuffer, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DIndexBuffer9** ppBuffer, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD quality,
                                                 BOOL lockable, IDirect3DSurface9** ppSurface, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                        DWORD quality, BOOL discard, IDirect3DSurface9** ppSurface,
                                                        HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst,
                                            const POINT* dstPoint) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT swapChain, IDirect3DSurface9* dst) override;
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst,
                                          const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool,
                                                          IDirect3DSurface9** ppSurface, HANDLE* shared) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index, IDirect3DSurface9* surface) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* surface) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppSurface) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                                    DWORD stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* viewport) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* viewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* material) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* material) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT9* light) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL* enable) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float* plane) override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float* plane) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** ppBlock) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppBlock) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* status) override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* status) override;
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture9** ppTexture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override;
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY* entries) override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) override;
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* palette) override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* rect) override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* rect) override;
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL software) override;
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override;
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override;
    float STDMETHODCALLTYPE GetNPatchMode() override;
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices,
                                                   UINT startIndex, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count,
                                                     const void* indices, D3DFORMAT indexFormat, const void* data,
                                                     UINT stride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT srcStart, UINT dstIndex, UINT count, IDirect3DVertexBuffer9* dst,
                                              IDirect3DVertexDeclaration9* decl, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements,
                                                      IDirect3DVertexDeclaration9** ppDecl) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* fvf) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer9** ppBuffer, UINT* offset, UINT* stride) override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT setting) override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* setting) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* buffer) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppBuffer) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, int* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* data, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float* segs, const D3DRECTPATCH_INFO* info) override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float* segs, const D3DTRIPATCH_INFO* info) override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT handle) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** ppQuery) override;
    // IDirect3DDevice9Ex
    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT w, UINT h, float* rows, float* columns) override;
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9* src, IDirect3DSurface9* dst, IDirect3DVertexBuffer9* srcDescs,
                                           UINT count, IDirect3DVertexBuffer9* dstDescs, D3DCOMPOSERECTSOP op, INT x,
                                           INT y) override;
    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty,
                                        DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* priority) override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override;
    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT swapChain) override;
    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9** resources, UINT32 count) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* latency) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND window) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                   DWORD quality, BOOL lockable, IDirect3DSurface9** ppSurface,
                                                   HANDLE* shared, DWORD usage) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool,
                                                            IDirect3DSurface9** ppSurface, HANDLE* shared,
                                                            DWORD usage) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                          DWORD quality, BOOL discard, IDirect3DSurface9** ppSurface,
                                                          HANDLE* shared, DWORD usage) override;
    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT swapChain, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) override;

    // ---- used by the resource objects and the d3d8 interop --------------------------------------
    bool isEx() const noexcept { return ex_; }
    Interface* parent() const noexcept { return parent_; }
    const D3DDEVICE_CREATION_PARAMETERS& creationParams() const noexcept { return createParams_; }
    SwapChain* implicitSwapChain() const noexcept { return swapChain_; }
    DeviceState& shadowState() noexcept { return state_; }
    bool recording() const noexcept { return recording_ != nullptr; }
    bool softwareVp() const noexcept { return softwareVpConsts_; }
    HRESULT presentCommon(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty, DWORD flags, bool ex);
    // IDxvkLegacyD3DDeviceBridge
    HRESULT updateTextureFromBuffer(IDirect3DSurface9* dst, IDirect3DSurface9* src, const RECT* srcRect,
                                    const POINT* dstPoint);
    bool isSupportedSurfaceFormat(D3DFORMAT format);

protected:
    void destroyOnHost() override;
    void finalRelease() override;

private:
    HRESULT createSurface(uint32_t command, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD quality,
                          BOOL flag, DWORD usage, D3DPOOL pool, IDirect3DSurface9** ppSurface);
    HRESULT resetCommon(D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, bool ex);
    void releaseImplicit();
    DeviceState& target();  // the state Set* writes: the recording block or the device
    template <class Sb>
    void recordMask(Sb&& setMask);
    void markTargetDirty(BridgeObject* target);

    Interface* parent_;
    bool ex_;
    D3DDEVICE_CREATION_PARAMETERS createParams_;
    D3DCAPS9 caps_;
    SwapChain* swapChain_ = nullptr;   // implicit swap chain 0 (private ref)
    Surface* autoDepth_ = nullptr;     // implicit depth-stencil (private ref)
    DeviceState state_;
    StateBlock* recording_ = nullptr;
    std::array<PrivateRef, kRenderTargets> renderTargets_;
    PrivateRef depthStencil_;
    D3DCLIPSTATUS9 clipStatus_ {};
    std::map<UINT, std::array<PALETTEENTRY, 256>> palettes_;
    UINT currentPalette_ = 0xFFFF;
    BOOL softwareVp_ = FALSE;
    bool softwareVpConsts_ = false;  // software or mixed VP device: software constant ranges
    float nPatch_ = 0.0f;
    D3DGAMMARAMP gamma_ {};
    UINT maxLatency_ = 3;
    INT gpuPriority_ = 0;
    BOOL cursorVisible_ = FALSE;
    bool inScene_ = false;
    std::map<D3DFORMAT, bool> surfaceFormats_;
    IUnknown* dxvkBridge_ = nullptr;   // IDxvkLegacyD3DDeviceBridge (dxvk_interop.cpp)
};

}  // namespace fuse::relight::bridge::client
