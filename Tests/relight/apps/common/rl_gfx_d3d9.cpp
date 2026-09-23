// FUSE Relight test-app kit (RL-0.4): the D3D9 backend of rl::Gfx.
// d3d9.dll is loaded at run time (LoadLibrary + Direct3DCreate9), so the same exe runs on Wine's
// builtin d3d9 (wined3d), on Relight's d3d9.dll placed next to it, or on Windows.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

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

class BackendD3D9 final : public Backend {
public:
    ~BackendD3D9() override
    {
        for (auto& kv : m_tex) kv.second->Release();
        for (auto& kv : m_surf) kv.second->Release();
        for (auto& kv : m_vb) kv.second->Release();
        for (auto& kv : m_ib) kv.second->Release();
        for (auto& kv : m_decl) kv.second->Release();
        for (auto& kv : m_vs) kv.second->Release();
        for (auto& kv : m_ps) kv.second->Release();
        release(m_backBuffer);
        release(m_defaultDepth);
        release(m_readback);
        release(m_dev);
        release(m_d3d);
    }

    Api api() const override { return API_D3D9; }

    long init(void* hwnd, uint32_t devFlags) override
    {
        HMODULE mod = LoadLibraryA("d3d9.dll");
        if (!mod)
            return E_FAIL;
        typedef IDirect3D9*(WINAPI * CreateFn)(UINT);
        CreateFn create = (CreateFn)(void*)GetProcAddress(mod, "Direct3DCreate9");
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
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = (HWND)hwnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        DWORD flags = D3DCREATE_FPU_PRESERVE |
                      ((devFlags & DEV_SOFTWARE_VP) ? D3DCREATE_SOFTWARE_VERTEXPROCESSING : D3DCREATE_HARDWARE_VERTEXPROCESSING);
        HRESULT hr = m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)hwnd, flags, &pp, &m_dev);
        if (FAILED(hr))
            return hr;
        hr = m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBuffer);
        if (FAILED(hr))
            return hr;
        return m_dev->GetDepthStencilSurface(&m_defaultDepth);
    }

    std::string adapter() override
    {
        D3DADAPTER_IDENTIFIER9 id{};
        m_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
        return std::string(id.Description) + " / " + id.Driver;
    }

    Caps caps() override
    {
        D3DCAPS9 c{};
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
        return m_dev->SetSamplerState(s, (D3DSAMPLERSTATETYPE)t, v);
    }
    long setTransform(uint32_t t, const Mat4& m) override
    {
        D3DMATRIX d;
        memcpy(&d, m.m, sizeof d);
        return m_dev->SetTransform((D3DTRANSFORMSTATETYPE)t, &d);
    }
    long setLight(uint32_t i, const Light& l) override
    {
        static_assert(sizeof(Light) == sizeof(D3DLIGHT9), "Light layout");
        D3DLIGHT9 d;
        memcpy(&d, &l, sizeof d);
        return m_dev->SetLight(i, &d);
    }
    long lightEnable(uint32_t i, bool on) override { return m_dev->LightEnable(i, on ? TRUE : FALSE); }
    long setMaterial(const Material& m) override
    {
        static_assert(sizeof(Material) == sizeof(D3DMATERIAL9), "Material layout");
        D3DMATERIAL9 d;
        memcpy(&d, &m, sizeof d);
        return m_dev->SetMaterial(&d);
    }
    long setViewport(const Viewport& v) override
    {
        static_assert(sizeof(Viewport) == sizeof(D3DVIEWPORT9), "Viewport layout");
        D3DVIEWPORT9 d;
        memcpy(&d, &v, sizeof d);
        return m_dev->SetViewport(&d);
    }

    long createTexture(int id, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) override
    {
        IDirect3DTexture9* t = nullptr;
        HRESULT hr = m_dev->CreateTexture(w, h, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &t, nullptr);
        if (SUCCEEDED(hr))
            m_tex[id] = t;
        return hr;
    }
    long writeTexture(int id, uint32_t level, const uint8_t* data, uint32_t rowBytes, uint32_t rows) override
    {
        IDirect3DTexture9* t = m_tex.at(id);
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
        IDirect3DSurface9 *s = nullptr, *d = nullptr;
        HRESULT hr = m_tex.at(src)->GetSurfaceLevel(srcLevel, &s);
        if (SUCCEEDED(hr))
            hr = m_tex.at(dst)->GetSurfaceLevel(dstLevel, &d);
        if (SUCCEEDED(hr))
            hr = m_dev->UpdateSurface(s, nullptr, d, nullptr);
        release(s);
        release(d);
        return hr;
    }
    long createDepthStencil(int id, uint32_t w, uint32_t h, uint32_t fmt) override
    {
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = m_dev->CreateDepthStencilSurface(w, h, (D3DFORMAT)fmt, D3DMULTISAMPLE_NONE, 0, TRUE, &s, nullptr);
        if (SUCCEEDED(hr))
            m_surf[id] = s;
        return hr;
    }
    long setRenderTarget(int texId) override
    {
        if (texId == RT_BACKBUFFER)
            return m_dev->SetRenderTarget(0, m_backBuffer);
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = m_tex.at(texId)->GetSurfaceLevel(0, &s);
        if (SUCCEEDED(hr))
            hr = m_dev->SetRenderTarget(0, s);
        release(s);
        return hr;
    }
    long setDepthStencil(int id) override
    {
        return m_dev->SetDepthStencilSurface(id == DS_DEFAULT ? m_defaultDepth : m_surf.at(id));
    }
    long createVertexBuffer(int id, uint32_t size, uint32_t usage, uint32_t fvf, uint32_t pool) override
    {
        IDirect3DVertexBuffer9* b = nullptr;
        HRESULT hr = m_dev->CreateVertexBuffer(size, usage, fvf, (D3DPOOL)pool, &b, nullptr);
        if (SUCCEEDED(hr))
            m_vb[id] = b;
        return hr;
    }
    long createIndexBuffer(int id, uint32_t size, uint32_t usage, uint32_t fmt, uint32_t pool) override
    {
        IDirect3DIndexBuffer9* b = nullptr;
        HRESULT hr = m_dev->CreateIndexBuffer(size, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &b, nullptr);
        if (SUCCEEDED(hr))
            m_ib[id] = b;
        return hr;
    }
    long writeBuffer(int id, uint32_t offset, const void* data, uint32_t size, uint32_t flags) override
    {
        void* p = nullptr;
        auto vb = m_vb.find(id);
        if (vb != m_vb.end()) {
            HRESULT hr = vb->second->Lock(offset, size, &p, flags);
            if (FAILED(hr))
                return hr;
            memcpy(p, data, size);
            return vb->second->Unlock();
        }
        IDirect3DIndexBuffer9* ib = m_ib.at(id);
        HRESULT hr = ib->Lock(offset, size, &p, flags);
        if (FAILED(hr))
            return hr;
        memcpy(p, data, size);
        return ib->Unlock();
    }
    long setStreamSource(uint32_t stream, int id, uint32_t offset, uint32_t stride) override
    {
        return m_dev->SetStreamSource(stream, id == NONE ? nullptr : m_vb.at(id), offset, stride);
    }
    long setIndices(int id) override { return m_dev->SetIndices(id == NONE ? nullptr : m_ib.at(id)); }
    long setFVF(uint32_t fvf) override { return m_dev->SetFVF(fvf); }
    long createVertexDeclaration(int id, const std::vector<VertexElement>& elems) override
    {
        static_assert(sizeof(VertexElement) == sizeof(D3DVERTEXELEMENT9), "VertexElement layout");
        std::vector<D3DVERTEXELEMENT9> e(elems.size() + 1);
        memcpy(e.data(), elems.data(), elems.size() * sizeof(D3DVERTEXELEMENT9));
        e.back() = D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0};
        IDirect3DVertexDeclaration9* d = nullptr;
        HRESULT hr = m_dev->CreateVertexDeclaration(e.data(), &d);
        if (SUCCEEDED(hr))
            m_decl[id] = d;
        return hr;
    }
    long setVertexDeclaration(int id) override { return m_dev->SetVertexDeclaration(id == NONE ? nullptr : m_decl.at(id)); }
    long createVertexShader(int id, const std::vector<uint32_t>& code, int) override
    {
        IDirect3DVertexShader9* s = nullptr;
        HRESULT hr = m_dev->CreateVertexShader((const DWORD*)code.data(), &s);
        if (SUCCEEDED(hr))
            m_vs[id] = s;
        return hr;
    }
    long createPixelShader(int id, const std::vector<uint32_t>& code) override
    {
        IDirect3DPixelShader9* s = nullptr;
        HRESULT hr = m_dev->CreatePixelShader((const DWORD*)code.data(), &s);
        if (SUCCEEDED(hr))
            m_ps[id] = s;
        return hr;
    }
    long setVertexShader(int id) override { return m_dev->SetVertexShader(id == NONE ? nullptr : m_vs.at(id)); }
    long setPixelShader(int id) override { return m_dev->SetPixelShader(id == NONE ? nullptr : m_ps.at(id)); }
    long setVSConstF(uint32_t start, const float* v, uint32_t count) override
    {
        return m_dev->SetVertexShaderConstantF(start, v, count);
    }
    long setVSConstI(uint32_t start, const int* v, uint32_t count) override
    {
        return m_dev->SetVertexShaderConstantI(start, v, count);
    }
    long setVSConstB(uint32_t start, const int* v, uint32_t count) override
    {
        return m_dev->SetVertexShaderConstantB(start, (const BOOL*)v, count);
    }
    long setPSConstF(uint32_t start, const float* v, uint32_t count) override
    {
        return m_dev->SetPixelShaderConstantF(start, v, count);
    }
    long setTexture(uint32_t stage, int id) override { return m_dev->SetTexture(stage, id == NONE ? nullptr : m_tex.at(id)); }
    long drawPrimitive(uint32_t type, uint32_t startVertex, uint32_t primCount) override
    {
        return m_dev->DrawPrimitive((D3DPRIMITIVETYPE)type, startVertex, primCount);
    }
    long drawIndexedPrimitive(uint32_t type, int baseVertex, uint32_t minIndex, uint32_t numVertices, uint32_t startIndex,
                              uint32_t primCount) override
    {
        return m_dev->DrawIndexedPrimitive((D3DPRIMITIVETYPE)type, baseVertex, minIndex, numVertices, startIndex, primCount);
    }
    long drawPrimitiveUP(uint32_t type, uint32_t primCount, const void* v, uint32_t stride) override
    {
        return m_dev->DrawPrimitiveUP((D3DPRIMITIVETYPE)type, primCount, v, stride);
    }
    long drawIndexedPrimitiveUP(uint32_t type, uint32_t minIndex, uint32_t numVertices, uint32_t primCount, const void* idx,
                                uint32_t idxFmt, const void* v, uint32_t stride) override
    {
        return m_dev->DrawIndexedPrimitiveUP((D3DPRIMITIVETYPE)type, minIndex, numVertices, primCount, idx,
                                             (D3DFORMAT)idxFmt, v, stride);
    }

    long readSurface(IDirect3DSurface9* src, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, bool keepAlpha)
    {
        D3DSURFACE_DESC desc;
        HRESULT hr = src->GetDesc(&desc);
        if (FAILED(hr))
            return hr;
        if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
            return E_NOTIMPL;
        IDirect3DSurface9* sys = nullptr;
        hr = m_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        if (FAILED(hr))
            return hr;
        hr = m_dev->GetRenderTargetData(src, sys);
        if (SUCCEEDED(hr)) {
            D3DLOCKED_RECT lr;
            hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            if (SUCCEEDED(hr)) {
                w = desc.Width;
                h = desc.Height;
                rgba.resize(size_t(w) * h * 4);
                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t* row = (const uint8_t*)lr.pBits + size_t(y) * lr.Pitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                        o[0] = row[4 * x + 2];
                        o[1] = row[4 * x + 1];
                        o[2] = row[4 * x + 0];
                        o[3] = keepAlpha && desc.Format == D3DFMT_A8R8G8B8 ? row[4 * x + 3] : 255;
                    }
                }
                sys->UnlockRect();
            }
        }
        release(sys);
        return hr;
    }
    long readBackBuffer(std::vector<uint8_t>& rgba) override
    {
        uint32_t w, h;
        return readSurface(m_backBuffer, rgba, w, h, false);
    }
    long readRenderTarget(int texId, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) override
    {
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = m_tex.at(texId)->GetSurfaceLevel(0, &s);
        if (SUCCEEDED(hr))
            hr = readSurface(s, rgba, w, h, true);
        release(s);
        return hr;
    }

private:
    IDirect3D9* m_d3d = nullptr;
    IDirect3DDevice9* m_dev = nullptr;
    IDirect3DSurface9 *m_backBuffer = nullptr, *m_defaultDepth = nullptr, *m_readback = nullptr;
    std::map<int, IDirect3DTexture9*> m_tex;
    std::map<int, IDirect3DSurface9*> m_surf;
    std::map<int, IDirect3DVertexBuffer9*> m_vb;
    std::map<int, IDirect3DIndexBuffer9*> m_ib;
    std::map<int, IDirect3DVertexDeclaration9*> m_decl;
    std::map<int, IDirect3DVertexShader9*> m_vs;
    std::map<int, IDirect3DPixelShader9*> m_ps;
};

} // namespace

Backend* createBackend() { return new BackendD3D9(); }

} // namespace rl
