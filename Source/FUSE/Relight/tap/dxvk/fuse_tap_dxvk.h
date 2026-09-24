// FUSE Relight RL-1.1: entry points the patched DXVK d3d9 front end calls (FUSE-DXVK patches
// RL-1.1-NN, Engine/lib/dxvk/PATCHES.md). Included from d3d9_device.h (patch RL-1.1-03); the
// implementation (fuse_tap_dxvk.cpp) converts DXVK's state into IRelightTap events
// (Source/FUSE/Relight/tap/include/fuse/relight/tap/relight_tap.hpp).
//
// D3D9DeviceEx::m_fuseTap is the per-device dispatcher. It is null when the tap is off, and every
// hook inside D3D9DeviceEx tests it before calling in; the hooks in other classes call the static
// functions below, which test it themselves. This header is compiled as part of vendored DXVK
// sources: it must stay self-contained and warning-free, with nothing but declarations.
#pragma once

namespace dxvk {

  class D3D9DeviceEx;
  class D3D9CommonTexture;
  class D3D9CommonBuffer;
  class D3D9SwapChainEx;
  class FuseTapContext;

  struct FuseTap {
    enum DrawCall : uint32_t { Draw = 0, DrawIndexed = 1, DrawUP = 2, DrawIndexedUP = 3 };

    // Device lifetime. The first successful ResetSwapChain (InitialReset) attaches the tap when
    // relight.tap.mode is not off and reports onDeviceCreate; later ones report onDeviceReset.
    static void SwapChainReset(D3D9DeviceEx* pDevice, const D3DPRESENT_PARAMETERS* pParams);
    static void DeviceDestroy(D3D9DeviceEx* pDevice);

    // Resources (null-check the tap themselves).
    static void TextureCreate(D3D9DeviceEx* pDevice, D3D9CommonTexture* pTexture);
    static void TextureDestroy(D3D9DeviceEx* pDevice, D3D9CommonTexture* pTexture);
    static void BufferCreate(D3D9DeviceEx* pDevice, D3D9CommonBuffer* pBuffer);

    // Uploads (called only when the tap is on).
    static void TextureLock(D3D9DeviceEx* pDevice, D3D9CommonTexture* pTexture, UINT Face, UINT MipLevel,
                            const D3DLOCKED_BOX* pLockedBox, const D3DBOX* pBox, DWORD Flags);
    static void TextureUnlock(D3D9DeviceEx* pDevice, D3D9CommonTexture* pTexture, UINT Face, UINT MipLevel);
    static void UpdateTexture(D3D9DeviceEx* pDevice, D3D9CommonTexture* pSource, D3D9CommonTexture* pDest);
    static void UpdateSurface(D3D9DeviceEx* pDevice, IDirect3DSurface9* pSource, const RECT* pSourceRect,
                              IDirect3DSurface9* pDest, const POINT* pDestPoint);
    static void BufferLock(D3D9DeviceEx* pDevice, D3D9CommonBuffer* pBuffer, UINT Offset, UINT Size, DWORD Flags);
    static void BufferUnlock(D3D9DeviceEx* pDevice, D3D9CommonBuffer* pBuffer);

    // Draws (called only when the tap is on). Returns true when the tap decided Ignore: the
    // caller returns D3D_OK without drawing.
    static bool SkipDraw(D3D9DeviceEx* pDevice, DrawCall Call, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                         UINT StartVertex, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices,
                         UINT StartIndex, const void* pIndexData, D3DFORMAT IndexDataFormat,
                         const void* pVertexData, UINT VertexStride);

    // Other events (called only when the tap is on, except Present / QueryIssue).
    static void Clear(D3D9DeviceEx* pDevice, DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color,
                      float Z, DWORD Stencil);
    static void SetRenderTarget(D3D9DeviceEx* pDevice, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget);
    static void Present(D3D9DeviceEx* pDevice, D3D9SwapChainEx* pSwapChain);
    static void QueryIssue(D3D9DeviceEx* pDevice, const void* pQuery, D3DQUERYTYPE Type, DWORD IssueFlags);
  };

}
