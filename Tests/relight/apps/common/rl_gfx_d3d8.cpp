// FUSE Relight test-app kit (RL-0.4): the D3D8 backend of rl::Gfx (the d3d8_* twin apps).
// d3d8.dll is loaded at run time (MinGW ships no libd3d8 import library).
//
// D3D8 differences handled here, so the scenes stay D3D9-shaped:
//   - sampler states are texture-stage states (D3DSAMP_* -> D3DTSS_ADDRESSU..MAXANISOTROPY);
//   - SetFVF is SetVertexShader(fvf); programmable shaders carry their D3DVSD declaration,
//     built from the rl::VertexElement list (element i binds register v<i>);
//   - SetIndices takes the BaseVertexIndex (applied at draw time; negative values do not exist);
//   - SetStreamSource has no offset (must be 0);
//   - SetRenderTarget sets colour and depth together;
//   - read-back uses a lockable back buffer (no GetRenderTargetData) and CopyRects for render
//     targets.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d8.h>

#include <algorithm>
#include <map>
#include <stdio.h>
#include <string.h>

#include "rl_app.h"

namespace rl {
namespace {

template <class T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

class BackendD3D8 final : public Backend {
public:
    ~BackendD3D8() override
    {
        for (auto& kv : m_tex) kv.second->Release();
        for (auto& kv : m_surf) kv.second->Release();
        for (auto& kv : m_vb) kv.second->Release();
        for (auto& kv : m_ib) kv.second->Release();
        if (m_dev) {
            for (auto& kv : m_vs) m_dev->DeleteVertexShader(kv.second);
            for (auto& kv : m_ps) m_dev->DeletePixelShader(kv.second);
        }
        release(m_curRT);
        release(m_curDS);
        release(m_backBuffer);
        release(m_defaultDepth);
        release(m_dev);
        release(m_d3d);
    }

    Api api() const override { return API_D3D8; }

    long init(void* hwnd, uint32_t devFlags) override
    {
        HMODULE mod = LoadLibraryA("d3d8.dll");
        if (!mod)
            return E_FAIL;
        typedef IDirect3D8*(WINAPI * CreateFn)(UINT);
        CreateFn create = (CreateFn)(void*)GetProcAddress(mod, "Direct3DCreate8");
        if (!create)
            return E_FAIL;
        m_d3d = create(D3D_SDK_VERSION);
        if (!m_d3d)
            return E_FAIL;
        D3DPRESENT_PARAMETERS pp{};
        pp.BackBufferWidth = kWidth;
        pp.BackBufferHeight = kHeight;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = (HWND)hwnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
        DWORD flags = D3DCREATE_FPU_PRESERVE |
                      ((devFlags & DEV_SOFTWARE_VP) ? D3DCREATE_SOFTWARE_VERTEXPROCESSING : D3DCREATE_HARDWARE_VERTEXPROCESSING);
        HRESULT hr = m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)hwnd, flags, &pp, &m_dev);
        if (FAILED(hr))
            return hr;
        hr = m_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
        if (FAILED(hr))
            return hr;
        hr = m_dev->GetDepthStencilSurface(&m_defaultDepth);
        if (FAILED(hr))
            return hr;
        m_curRT = m_backBuffer;
        m_curRT->AddRef();
        m_curDS = m_defaultDepth;
        m_curDS->AddRef();
        return m_dev->SetVertexShader(m_fvf);
    }

    std::string adapter() override
    {
        D3DADAPTER_IDENTIFIER8 id{};
        m_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
        return std::string(id.Description) + " / " + id.Driver;
    }

    Caps caps() override
    {
        D3DCAPS8 c{};
        m_dev->GetDeviceCaps(&c);
        Caps r;
        r.vsVersion = c.VertexShaderVersion;
        r.psVersion = c.PixelShaderVersion;
        r.maxVertexBlendMatrices = c.MaxVertexBlendMatrices;
        r.maxVertexBlendMatrixIndex = c.MaxVertexBlendMatrixIndex;
        r.maxTextureBlendStages = c.MaxTextureBlendStages;
        r.maxSimultaneousTextures = c.MaxSimultaneousTextures;
        r.maxStreams = c.MaxStreams;
        return r;
    }

    bool formatSupported(uint32_t format, uint32_t usage, uint32_t rtype) override
    {
        return SUCCEEDED(m_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage,
                                                  (D3DRESOURCETYPE)rtype, (D3DFORMAT)format));
    }

    long beginScene() override { return m_dev->BeginScene(); }
    long endScene() override { return m_dev->EndScene(); }
    long present() override { return m_dev->Present(nullptr, nullptr, nullptr, nullptr); }
    long clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) override
    {
        return m_dev->Clear(0, nullptr, flags, color, z, stencil);
    }
    long setRenderState(uint32_t s, uint32_t v) override { return m_dev->SetRenderState((D3DRENDERSTATETYPE)s, v); }
    long setTextureStageState(uint32_t stage, uint32_t t, uint32_t v) override
    {
        return m_dev->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)t, v);
    }
    long setSamplerState(uint32_t s, uint32_t t, uint32_t v) override
    {
        // D3D9 D3DSAMP_* (1..10) -> D3D8 D3DTSS_*.
        static const DWORD map[] = {0, D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_ADDRESSW, D3DTSS_BORDERCOLOR,
                                    D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER, D3DTSS_MIPMAPLODBIAS,
                                    D3DTSS_MAXMIPLEVEL, D3DTSS_MAXANISOTROPY};
        if (t == 0 || t > 10)
            return D3DERR_INVALIDCALL;
        return m_dev->SetTextureStageState(s, (D3DTEXTURESTAGESTATETYPE)map[t], v);
    }
    long setTransform(uint32_t t, const Mat4& m) override
    {
        D3DMATRIX d;
        memcpy(&d, m.m, sizeof d);
        return m_dev->SetTransform((D3DTRANSFORMSTATETYPE)t, &d);
    }
    long setLight(uint32_t i, const Light& l) override
    {
        static_assert(sizeof(Light) == sizeof(D3DLIGHT8), "Light layout");
        D3DLIGHT8 d;
        memcpy(&d, &l, sizeof d);
        return m_dev->SetLight(i, &d);
    }
    long lightEnable(uint32_t i, bool on) override { return m_dev->LightEnable(i, on ? TRUE : FALSE); }
    long setMaterial(const Material& m) override
    {
        static_assert(sizeof(Material) == sizeof(D3DMATERIAL8), "Material layout");
        D3DMATERIAL8 d;
        memcpy(&d, &m, sizeof d);
        return m_dev->SetMaterial(&d);
    }
    long setViewport(const Viewport& v) override
    {
        static_assert(sizeof(Viewport) == sizeof(D3DVIEWPORT8), "Viewport layout");
        D3DVIEWPORT8 d;
        memcpy(&d, &v, sizeof d);
        return m_dev->SetViewport(&d);
    }

    long createTexture(int id, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) override
    {
        IDirect3DTexture8* t = nullptr;
        HRESULT hr = m_dev->CreateTexture(w, h, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &t);
        if (SUCCEEDED(hr))
            m_tex[id] = t;
        return hr;
    }
    long writeTexture(int id, uint32_t level, const uint8_t* data, uint32_t rowBytes, uint32_t rows) override
    {
        IDirect3DTexture8* t = m_tex.at(id);
        D3DLOCKED_RECT lr;
        HRESULT hr = t->LockRect(level, &lr, nullptr, 0);
        if (FAILED(hr))
            return hr;
        for (uint32_t r = 0; r < rows; ++r)
            memcpy((uint8_t*)lr.pBits + size_t(r) * lr.Pitch, data + size_t(r) * rowBytes, rowBytes);
        return t->UnlockRect(level);
    }
    long updateTexture(int src, int dst) override { return m_dev->UpdateTexture(m_tex.at(src), m_tex.at(dst)); }
    long updateSurface(int src, uint32_t srcLevel, int dst, uint32_t dstLevel) override
    {
        IDirect3DSurface8 *s = nullptr, *d = nullptr;
        HRESULT hr = m_tex.at(src)->GetSurfaceLevel(srcLevel, &s);
        if (SUCCEEDED(hr))
            hr = m_tex.at(dst)->GetSurfaceLevel(dstLevel, &d);
        if (SUCCEEDED(hr))
            hr = m_dev->CopyRects(s, nullptr, 0, d, nullptr);
        release(s);
        release(d);
        return hr;
    }
    long createDepthStencil(int id, uint32_t w, uint32_t h, uint32_t fmt) override
    {
        IDirect3DSurface8* s = nullptr;
        HRESULT hr = m_dev->CreateDepthStencilSurface(w, h, (D3DFORMAT)fmt, D3DMULTISAMPLE_NONE, &s);
        if (SUCCEEDED(hr))
            m_surf[id] = s;
        return hr;
    }
    long applyTargets(IDirect3DSurface8* rt, IDirect3DSurface8* ds)
    {
        HRESULT hr = m_dev->SetRenderTarget(rt, ds);
        if (FAILED(hr))
            return hr;
        rt->AddRef();
        if (ds)
            ds->AddRef();
        release(m_curRT);
        release(m_curDS);
        m_curRT = rt;
        m_curDS = ds;
        return hr;
    }
    long setRenderTarget(int texId) override
    {
        if (texId == RT_BACKBUFFER)
            return applyTargets(m_backBuffer, m_curDS);
        IDirect3DSurface8* s = nullptr;
        HRESULT hr = m_tex.at(texId)->GetSurfaceLevel(0, &s);
        if (SUCCEEDED(hr))
            hr = applyTargets(s, m_curDS);
        release(s);
        return hr;
    }
    long setDepthStencil(int id) override { return applyTargets(m_curRT, id == DS_DEFAULT ? m_defaultDepth : m_surf.at(id)); }
    long createVertexBuffer(int id, uint32_t size, uint32_t usage, uint32_t fvf, uint32_t pool) override
    {
        IDirect3DVertexBuffer8* b = nullptr;
        HRESULT hr = m_dev->CreateVertexBuffer(size, usage, fvf, (D3DPOOL)pool, &b);
        if (SUCCEEDED(hr))
            m_vb[id] = b;
        return hr;
    }
    long createIndexBuffer(int id, uint32_t size, uint32_t usage, uint32_t fmt, uint32_t pool) override
    {
        IDirect3DIndexBuffer8* b = nullptr;
        HRESULT hr = m_dev->CreateIndexBuffer(size, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &b);
        if (SUCCEEDED(hr))
            m_ib[id] = b;
        return hr;
    }
    long writeBuffer(int id, uint32_t offset, const void* data, uint32_t size, uint32_t flags) override
    {
        BYTE* p = nullptr;
        auto vb = m_vb.find(id);
        if (vb != m_vb.end()) {
            HRESULT hr = vb->second->Lock(offset, size, &p, flags);
            if (FAILED(hr))
                return hr;
            memcpy(p, data, size);
            return vb->second->Unlock();
        }
        IDirect3DIndexBuffer8* ib = m_ib.at(id);
        HRESULT hr = ib->Lock(offset, size, &p, flags);
        if (FAILED(hr))
            return hr;
        memcpy(p, data, size);
        return ib->Unlock();
    }
    long setStreamSource(uint32_t stream, int id, uint32_t offset, uint32_t stride) override
    {
        if (offset != 0)
            return D3DERR_INVALIDCALL; // D3D8 has no stream offset
        return m_dev->SetStreamSource(stream, id == NONE ? nullptr : m_vb.at(id), stride);
    }
    long setIndices(int id) override
    {
        m_indices = id;
        return m_dev->SetIndices(id == NONE ? nullptr : m_ib.at(id), 0);
    }
    long setFVF(uint32_t fvf) override
    {
        m_fvf = fvf;
        return m_curVS == NONE ? m_dev->SetVertexShader(fvf) : D3D_OK;
    }
    long createVertexDeclaration(int id, const std::vector<VertexElement>& elems) override
    {
        m_declElems[id] = elems;
        return D3D_OK;
    }
    long setVertexDeclaration(int) override { return D3D_OK; } // D3D8: the declaration belongs to the shader
    long createVertexShader(int id, const std::vector<uint32_t>& code, int declId) override
    {
        const std::vector<VertexElement>& e = m_declElems.at(declId);
        std::vector<DWORD> decl;
        for (uint32_t stream = 0; stream < 16; ++stream) {
            std::vector<std::pair<uint32_t, uint32_t>> items; // (offset, element index)
            for (uint32_t i = 0; i < e.size(); ++i)
                if (e[i].Stream == stream)
                    items.emplace_back(e[i].Offset, i);
            if (items.empty())
                continue;
            std::sort(items.begin(), items.end());
            decl.push_back(D3DVSD_STREAM(stream));
            uint32_t at = 0;
            for (auto& it : items) {
                static const uint32_t sizes[] = {4, 8, 12, 16, 4, 4, 4, 8};
                const VertexElement& v = e[it.second];
                if (v.Type > 7)
                    return D3DERR_INVALIDCALL;
                if (it.first > at)
                    decl.push_back(D3DVSD_SKIP((it.first - at) / 4));
                decl.push_back(D3DVSD_REG(it.second, v.Type)); // D3DVSDT_* == D3DDECLTYPE_* for types 0..7
                at = it.first + sizes[v.Type];
            }
        }
        decl.push_back(D3DVSD_END());
        DWORD h = 0;
        HRESULT hr = m_dev->CreateVertexShader(decl.data(), (const DWORD*)code.data(), &h, 0);
        if (SUCCEEDED(hr))
            m_vs[id] = h;
        return hr;
    }
    long createPixelShader(int id, const std::vector<uint32_t>& code) override
    {
        DWORD h = 0;
        HRESULT hr = m_dev->CreatePixelShader((const DWORD*)code.data(), &h);
        if (SUCCEEDED(hr))
            m_ps[id] = h;
        return hr;
    }
    long setVertexShader(int id) override
    {
        m_curVS = id;
        return m_dev->SetVertexShader(id == NONE ? m_fvf : m_vs.at(id));
    }
    long setPixelShader(int id) override { return m_dev->SetPixelShader(id == NONE ? 0 : m_ps.at(id)); }
    long setVSConstF(uint32_t start, const float* v, uint32_t count) override
    {
        return m_dev->SetVertexShaderConstant(start, v, count);
    }
    long setVSConstI(uint32_t, const int*, uint32_t) override { return D3DERR_INVALIDCALL; }
    long setVSConstB(uint32_t, const int*, uint32_t) override { return D3DERR_INVALIDCALL; }
    long setPSConstF(uint32_t start, const float* v, uint32_t count) override
    {
        return m_dev->SetPixelShaderConstant(start, v, count);
    }
    long setTexture(uint32_t stage, int id) override { return m_dev->SetTexture(stage, id == NONE ? nullptr : m_tex.at(id)); }
    long drawPrimitive(uint32_t type, uint32_t startVertex, uint32_t primCount) override
    {
        return m_dev->DrawPrimitive((D3DPRIMITIVETYPE)type, startVertex, primCount);
    }
    long drawIndexedPrimitive(uint32_t type, int baseVertex, uint32_t minIndex, uint32_t numVertices, uint32_t startIndex,
                              uint32_t primCount) override
    {
        if (baseVertex < 0)
            return D3DERR_INVALIDCALL;
        HRESULT hr = m_dev->SetIndices(m_indices == NONE ? nullptr : m_ib.at(m_indices), (UINT)baseVertex);
        if (FAILED(hr))
            return hr;
        return m_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)type, minIndex, numVertices, startIndex, primCount);
    }
    long drawPrimitiveUP(uint32_t type, uint32_t primCount, const void* v, uint32_t stride) override
    {
        return m_dev->DrawPrimitiveUP((D3DPRIMITIVETYPE)type, primCount, v, stride);
    }
    long drawIndexedPrimitiveUP(uint32_t type, uint32_t minIndex, uint32_t numVertices, uint32_t primCount, const void* idx,
                                uint32_t idxFmt, const void* v, uint32_t stride) override
    {
        m_indices = NONE;
        return m_dev->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)type, minIndex, numVertices, primCount, idx,
                                             (D3DFORMAT)idxFmt, v, stride);
    }

    static void toRGBA(const D3DLOCKED_RECT& lr, uint32_t w, uint32_t h, bool keepAlpha, std::vector<uint8_t>& rgba)
    {
        rgba.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* row = (const uint8_t*)lr.pBits + size_t(y) * lr.Pitch;
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                o[0] = row[4 * x + 2];
                o[1] = row[4 * x + 1];
                o[2] = row[4 * x + 0];
                o[3] = keepAlpha ? row[4 * x + 3] : 255;
            }
        }
    }
    long readBackBuffer(std::vector<uint8_t>& rgba) override
    {
        D3DLOCKED_RECT lr;
        HRESULT hr = m_backBuffer->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
            return hr;
        toRGBA(lr, kWidth, kHeight, false, rgba);
        return m_backBuffer->UnlockRect();
    }
    long readRenderTarget(int texId, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) override
    {
        IDirect3DSurface8 *s = nullptr, *sys = nullptr;
        HRESULT hr = m_tex.at(texId)->GetSurfaceLevel(0, &s);
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(hr))
            hr = s->GetDesc(&desc);
        if (SUCCEEDED(hr) && desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
            hr = E_NOTIMPL;
        if (SUCCEEDED(hr))
            hr = m_dev->CreateImageSurface(desc.Width, desc.Height, desc.Format, &sys);
        if (SUCCEEDED(hr))
            hr = m_dev->CopyRects(s, nullptr, 0, sys, nullptr);
        if (SUCCEEDED(hr)) {
            D3DLOCKED_RECT lr;
            hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            if (SUCCEEDED(hr)) {
                w = desc.Width;
                h = desc.Height;
                toRGBA(lr, w, h, desc.Format == D3DFMT_A8R8G8B8, rgba);
                sys->UnlockRect();
            }
        }
        release(sys);
        release(s);
        return hr;
    }

private:
    IDirect3D8* m_d3d = nullptr;
    IDirect3DDevice8* m_dev = nullptr;
    IDirect3DSurface8 *m_backBuffer = nullptr, *m_defaultDepth = nullptr, *m_curRT = nullptr, *m_curDS = nullptr;
    DWORD m_fvf = D3DFVF_XYZ;
    int m_curVS = NONE;
    int m_indices = NONE;
    std::map<int, IDirect3DTexture8*> m_tex;
    std::map<int, IDirect3DSurface8*> m_surf;
    std::map<int, IDirect3DVertexBuffer8*> m_vb;
    std::map<int, IDirect3DIndexBuffer8*> m_ib;
    std::map<int, std::vector<VertexElement>> m_declElems;
    std::map<int, DWORD> m_vs, m_ps;
};

} // namespace

Backend* createBackend() { return new BackendD3D8(); }

} // namespace rl
