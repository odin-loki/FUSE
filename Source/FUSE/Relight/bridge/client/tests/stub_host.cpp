// FUSE Relight RL-2.2: test-local stub host for the bridge client tests (rl_bridge_*).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0). New code; the replay semantics follow dxvk-remix
// bridge/src/server/main.cpp (MIT, @0867d3c): handles name host objects, children are fetched from
// their containers, Unlock payloads are written through a host Lock, queries answer from the real
// device.
//
// The real host is RL-2.3 (bridge/host). This stub implements the client contract of
// bridge/client/include/fuse/relight/bridge/client/protocol.hpp against a real D3D9 in this process
// (the vendored DXVK d3d9.dll, path from --d3d9 or FUSE_RELIGHT_STUB_D3D9), decoding and
// dispatching every command of the generated schema:
//
//   stub_host.exe --bridge-session <name> --client-pid <pid> [--d3d9 <dll>] [--record <file>]
//
// --record (or FUSE_RELIGHT_STUB_RECORD) writes the received command stream, one line per command:
// "<name> <handle> <payloadBytes>" (window messages excluded: their count depends on the window
// manager). The tests compare the streams of the x86 and x64 builds of an app.
//
// Unhandled commands (none are expected) are counted and reported at exit with exit code 3.
#include <fuse/relight/bridge/client/format_layout.hpp>
#include <fuse/relight/bridge/client/protocol.hpp>
#include <fuse/relight/bridge/client/wire_types.hpp>
#include <fuse/relight/bridge/ipc/session.hpp>

#include "d3d9/d3d9_include.h"
#include "d3d9/d3d9_bridge.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipc = fuse::relight::bridge::ipc;
namespace schema = fuse::relight::bridge::schema;
namespace cmd = fuse::relight::bridge::schema::cmd;
namespace client = fuse::relight::bridge::client;
namespace wire = fuse::relight::bridge::client::wire;

namespace {

constexpr HRESULT kReplied = HRESULT(0x7FFF0001);  // handler sent its own typed reply

class StubHost {
public:
    StubHost(ipc::Session& s, HMODULE d3d9, FILE* record) : s_(s), d3d9_(d3d9), record_(record) {}

    int run() {
        for (;;) {
            ipc::Session::Incoming in;
            const ipc::Result r = s_.receive(in, ipc::kInfinite);
            if (r == ipc::Result::PeerClosed || r == ipc::Result::PeerDead) {
                break;
            }
            if (r != ipc::Result::Success) {
                std::fprintf(stderr, "stub host: receive failed (%s)\n", ipc::toString(r));
                return 2;
            }
            header_ = in.header;
            if (record_ != nullptr && in.header.command != uint16_t(schema::CommandId::Bridge_WindowMessage)) {
                std::fprintf(record_, "%s %u %u\n", schema::commandName(in.header.command), unsigned(in.header.handle),
                             unsigned(in.header.dataSize));
            }
            bool stop = false;
            const schema::DecodeStatus st = schema::dispatch(
                in.header.command, in.data, in.header.dataSize,
                [&](const auto& c) {
                    using C = std::decay_t<decltype(c)>;
                    if constexpr (std::is_same_v<C, cmd::Bridge_Terminate>) {
                        stop = true;
                    } else {
                        finish(on(c));
                    }
                });
            s_.release(in);
            if (st != schema::DecodeStatus::Ok) {
                std::fprintf(stderr, "stub host: malformed %s\n", schema::commandName(in.header.command));
                return 2;
            }
            if (stop || failed_) {
                break;
            }
        }
        for (auto it = objects_.rbegin(); it != objects_.rend(); ++it) {
            if (it->second != nullptr) {
                it->second->Release();
            }
        }
        objects_.clear();
        if (unhandled_ != 0) {
            std::fprintf(stderr, "stub host: %u unhandled command(s), first %s\n", unhandled_, firstUnhandled_.c_str());
            return 3;
        }
        return failed_ ? 2 : 0;
    }

    uint32_t windowMessages() const { return windowMessages_; }

private:
    // ---- plumbing ----------------------------------------------------------------------------------------
    void finish(HRESULT hr) {
        if (hr == kReplied) {
            return;
        }
        if (header_.flags & client::kFlagWantsReply) {
            cmd::Reply_Result r;
            r.requestUid = header_.uid;
            r.hresult = hr;
            send(r);
        }
    }

    template <class R>
    void send(const R& r) {
        if (s_.send(r, 0, 0, 60000) != ipc::Result::Success) {
            failed_ = true;
        }
    }

    HRESULT sendData(HRESULT hr, const std::vector<uint8_t>& data) {
        size_t off = 0;
        do {
            const size_t n = std::min<size_t>(client::kReplyChunkMax, data.size() - off);
            cmd::Reply_Data d;
            d.requestUid = header_.uid;
            d.hresult = hr;
            d.totalBytes = uint32_t(data.size());
            d.offset = uint32_t(off);
            d.data.assign(data.begin() + off, data.begin() + off + n);
            send(d);
            off += n;
        } while (off < data.size());
        return kReplied;
    }

    template <class T>
    T* get(uint32_t h) {
        auto it = objects_.find(h);
        return it == objects_.end() ? nullptr : static_cast<T*>(it->second);
    }
    IUnknown* obj(uint32_t h) {
        auto it = objects_.find(h);
        return it == objects_.end() ? nullptr : it->second;
    }
    IDirect3DDevice9Ex* dev() { return get<IDirect3DDevice9Ex>(header_.handle); }
    IDirect3D9Ex* itf() { return get<IDirect3D9Ex>(header_.handle); }
    void put(uint32_t h, IUnknown* o) {
        if (h == 0 || o == nullptr) {
            return;
        }
        IUnknown*& slot = objects_[h];
        if (slot != nullptr) {
            slot->Release();
        }
        slot = o;
    }
    void drop(uint32_t h) {
        auto it = objects_.find(h);
        if (it != objects_.end()) {
            if (it->second != nullptr) {
                it->second->Release();
            }
            objects_.erase(it);
        }
    }

    // Upload payload: inline bytes or a shared-heap run (released after the copy).
    template <class C>
    const uint8_t* payload(const C& c, size_t& size) {
        if (c.heapBytes == 0) {
            size = c.data.size();
            return c.data.data();
        }
        ipc::HeapRef ref {c.heapChunk, c.heapBytes};
        const uint8_t* p = s_.heap().resolve(ref);
        size = p ? c.heapBytes : 0;
        pendingRelease_ = ref;
        return p;
    }
    void releasePayload() {
        if (pendingRelease_.valid()) {
            s_.heap().release(pendingRelease_);
            pendingRelease_ = ipc::HeapRef {};
        }
    }

    static void writeRows(uint8_t* dst, INT pitch, INT slicePitch, const uint8_t* src, uint32_t rowBytes, uint32_t rows,
                          uint32_t slices) {
        for (uint32_t z = 0; z < slices; ++z) {
            for (uint32_t y = 0; y < rows; ++y) {
                std::memcpy(dst + size_t(z) * slicePitch + size_t(y) * pitch, src, rowBytes);
                src += rowBytes;
            }
        }
    }
    static void readRows(std::vector<uint8_t>& out, const uint8_t* src, INT pitch, INT slicePitch, uint32_t rowBytes,
                         uint32_t rows, uint32_t slices) {
        out.resize(size_t(rowBytes) * rows * slices);
        uint8_t* d = out.data();
        for (uint32_t z = 0; z < slices; ++z) {
            for (uint32_t y = 0; y < rows; ++y) {
                std::memcpy(d, src + size_t(z) * slicePitch + size_t(y) * pitch, rowBytes);
                d += rowBytes;
            }
        }
    }

    // Region layout of a lock request as the client computes it.
    static bool clientLayout(D3DFORMAT fmt, uint32_t w, uint32_t h, uint32_t d, const std::vector<int32_t>& wr,
                             client::RegionLayout& rl) {
        client::Region r {0, 0, w, h, 0, d};
        if (wr.size() == 4) {
            r = client::Region {uint32_t(wr[0]), uint32_t(wr[1]), uint32_t(wr[2]), uint32_t(wr[3]), 0, 1};
        } else if (wr.size() == 6) {
            r = client::Region {uint32_t(wr[0]), uint32_t(wr[1]), uint32_t(wr[2]), uint32_t(wr[3]), uint32_t(wr[4]),
                                uint32_t(wr[5])};
        }
        return client::regionLayout(client::formatLayout(fmt), w, h, d, r, rl);
    }

    HRESULT readSurface(IDirect3DSurface9* s, const std::vector<int32_t>& wr, DWORD flags) {
        std::vector<uint8_t> out;
        D3DSURFACE_DESC desc;
        client::RegionLayout rl;
        if (s == nullptr || FAILED(s->GetDesc(&desc)) || !clientLayout(desc.Format, desc.Width, desc.Height, 1, wr, rl)) {
            return sendData(D3DERR_INVALIDCALL, out);
        }
        RECT rs;
        D3DLOCKED_RECT lr;
        HRESULT hr = s->LockRect(&lr, wire::rectFromWire(wr, rs), (flags & ~DWORD(D3DLOCK_DISCARD)) | D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            readRows(out, static_cast<const uint8_t*>(lr.pBits), lr.Pitch, 0, rl.rowBytes, rl.rows, 1);
            s->UnlockRect();
        }
        return sendData(hr, out);
    }

    HRESULT readVolume(IDirect3DVolume9* v, const std::vector<int32_t>& wr, DWORD flags) {
        std::vector<uint8_t> out;
        D3DVOLUME_DESC desc;
        client::RegionLayout rl;
        if (v == nullptr || FAILED(v->GetDesc(&desc)) ||
            !clientLayout(desc.Format, desc.Width, desc.Height, desc.Depth, wr, rl)) {
            return sendData(D3DERR_INVALIDCALL, out);
        }
        D3DBOX bs;
        D3DLOCKED_BOX lb;
        HRESULT hr = v->LockBox(&lb, wire::boxFromWire(wr, bs), (flags & ~DWORD(D3DLOCK_DISCARD)) | D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            readRows(out, static_cast<const uint8_t*>(lb.pBits), lb.RowPitch, lb.SlicePitch, rl.rowBytes, rl.rows, rl.slices);
            v->UnlockBox();
        }
        return sendData(hr, out);
    }

    template <class C>
    HRESULT writeSurface(IDirect3DSurface9* s, const C& c) {
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (s != nullptr && p != nullptr && size == size_t(c.rowBytes) * c.rows) {
            RECT rs;
            D3DLOCKED_RECT lr;
            hr = s->LockRect(&lr, wire::rectFromWire(c.rect, rs), c.flags & ~DWORD(D3DLOCK_READONLY));
            if (SUCCEEDED(hr)) {
                writeRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, 0, p, c.rowBytes, c.rows, 1);
                hr = s->UnlockRect();
            }
        }
        releasePayload();
        return hr;
    }

    template <class C>
    HRESULT writeVolume(IDirect3DVolume9* v, const C& c) {
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (v != nullptr && p != nullptr && size == size_t(c.rowBytes) * c.rows * c.slices) {
            D3DBOX bs;
            D3DLOCKED_BOX lb;
            hr = v->LockBox(&lb, wire::boxFromWire(c.box, bs), c.flags & ~DWORD(D3DLOCK_READONLY));
            if (SUCCEEDED(hr)) {
                writeRows(static_cast<uint8_t*>(lb.pBits), lb.RowPitch, lb.SlicePitch, p, c.rowBytes, c.rows, c.slices);
                hr = v->UnlockBox();
            }
        }
        releasePayload();
        return hr;
    }

    template <class B, class C>
    HRESULT writeBuffer(B* b, const C& c) {
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        void* dst = nullptr;
        if (b != nullptr && p != nullptr && SUCCEEDED(hr = b->Lock(c.offset, UINT(size), &dst, c.flags & ~DWORD(D3DLOCK_READONLY)))) {
            std::memcpy(dst, p, size);
            hr = b->Unlock();
        }
        releasePayload();
        return hr;
    }

    template <class B, class C>
    HRESULT readBuffer(B* b, const C& c) {
        std::vector<uint8_t> out;
        void* src = nullptr;
        HRESULT hr = D3DERR_INVALIDCALL;
        if (b != nullptr && SUCCEEDED(hr = b->Lock(c.offset, c.size, &src, D3DLOCK_READONLY))) {
            out.assign(static_cast<uint8_t*>(src), static_cast<uint8_t*>(src) + c.size);
            b->Unlock();
        }
        return sendData(hr, out);
    }

    HRESULT displayModeReply(HRESULT hr, const D3DDISPLAYMODEEX& m, D3DDISPLAYROTATION rot) {
        cmd::Reply_DisplayMode r;
        r.requestUid = header_.uid;
        r.hresult = hr;
        r.width = m.Width;
        r.height = m.Height;
        r.refreshRate = m.RefreshRate;
        r.format = m.Format;
        r.scanLineOrdering = m.ScanLineOrdering;
        r.rotation = rot;
        send(r);
        return kReplied;
    }
    HRESULT displayModeReply(HRESULT hr, const D3DDISPLAYMODE& m) {
        D3DDISPLAYMODEEX x {sizeof(x), m.Width, m.Height, m.RefreshRate, m.Format, D3DSCANLINEORDERING_PROGRESSIVE};
        return displayModeReply(hr, x, D3DDISPLAYROTATION_IDENTITY);
    }
    HRESULT valueReply(HRESULT hr, uint32_t v) {
        cmd::Reply_Value r;
        r.requestUid = header_.uid;
        r.hresult = hr;
        r.value = v;
        send(r);
        return kReplied;
    }

    // ---- windows ------------------------------------------------------------------------------------------
    // Wine 9 cannot create a Vulkan surface for another process's window (winevulkan:
    // VK_ERROR_OUT_OF_HOST_MEMORY), so DXVK fails CreateDevice with the game's HWND. The stub then
    // renders into a proxy window it owns, one per game window, sized like the game's client area.
    // Read-backs (GetRenderTargetData, locks) are unaffected; only the on-screen image is.
    HWND mapWindow(uint32_t w) {
        HWND game = wire::handleFromWire<HWND>(w);
        if (!proxy_ || game == nullptr) {
            return game;
        }
        auto it = proxies_.find(w);
        if (it != proxies_.end()) {
            return it->second;
        }
        RECT r {0, 0, 640, 480};
        ::GetClientRect(game, &r);
        static bool registered = false;
        if (!registered) {
            WNDCLASSA wc {};
            wc.lpfnWndProc = ::DefWindowProcA;
            wc.hInstance = ::GetModuleHandleA(nullptr);
            wc.lpszClassName = "fuse_relight_stub_host_proxy";
            ::RegisterClassA(&wc);
            registered = true;
        }
        HWND h = ::CreateWindowA("fuse_relight_stub_host_proxy", "FUSE Relight stub host", WS_OVERLAPPEDWINDOW, 0, 0,
                                 std::max<LONG>(r.right - r.left, 1), std::max<LONG>(r.bottom - r.top, 1), nullptr, nullptr,
                                 ::GetModuleHandleA(nullptr), nullptr);
        proxies_[w] = h;
        return h;
    }
    template <class Create>
    HRESULT createWithWindowFallback(Create&& create) {
        HRESULT hr = create();
        if (FAILED(hr) && !proxy_) {
            std::fprintf(stderr, "stub host: device creation on the game window failed (0x%08lx); using a proxy window\n",
                         static_cast<unsigned long>(hr));
            proxy_ = true;
            hr = create();
        }
        return hr;
    }
    D3DPRESENT_PARAMETERS present(const wire::PresentWords& w) {
        D3DPRESENT_PARAMETERS pp = wire::presentFromWire(w);
        pp.hDeviceWindow = mapWindow(w[7]);
        return pp;
    }

    // ---- catch-all ---------------------------------------------------------------------------------------
    template <class C>
    HRESULT on(const C&) {
        if (unhandled_++ == 0) {
            firstUnhandled_ = schema::commandName(C::kId);
        }
        return D3DERR_INVALIDCALL;
    }

    // ---- bridge control --------------------------------------------------------------------------------------
    HRESULT on(const cmd::Bridge_Sync&) { return D3D_OK; }
    HRESULT on(const cmd::Bridge_DebugMessage& c) {
        std::fprintf(stderr, "client: %s\n", c.text.c_str());
        return D3D_OK;
    }
    HRESULT on(const cmd::Bridge_WindowMessage&) {
        ++windowMessages_;
        return D3D_OK;
    }
    HRESULT on(const cmd::Bridge_UnlinkResource& c) {
        drop(c.resource);
        return D3D_OK;
    }
    HRESULT on(const cmd::Bridge_UnlinkVolumeResource& c) {
        drop(c.resource);
        return D3D_OK;
    }

    // ---- interface ---------------------------------------------------------------------------------------------
    HRESULT on(const cmd::Direct3DCreate9& c) {
        if (c.ex) {
            using Fn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
            auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(d3d9_, "Direct3DCreate9Ex")));
            IDirect3D9Ex* d = nullptr;
            const HRESULT hr = fn ? fn(c.sdkVersion, &d) : E_FAIL;
            put(c.result, d);
            return hr;
        }
        using Fn = IDirect3D9*(WINAPI*)(UINT);
        auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(d3d9_, "Direct3DCreate9")));
        IDirect3D9* d = fn ? fn(c.sdkVersion) : nullptr;
        put(c.result, d);
        return d ? D3D_OK : E_FAIL;
    }
    HRESULT on(const cmd::IDirect3D9Ex_SetD3DCompatibility& c) {
        IDxvkLegacyD3DInterfaceBridge* b = nullptr;
        if (itf() == nullptr || FAILED(itf()->QueryInterface(__uuidof(IDxvkLegacyD3DInterfaceBridge), reinterpret_cast<void**>(&b)))) {
            return E_NOINTERFACE;
        }
        b->SetD3DCompatibility(DxvkD3DCompatibility(c.compatibility));
        b->Release();
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3D9Ex_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterCount&) { return valueReply(D3D_OK, itf()->GetAdapterCount()); }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterIdentifier& c) {
        D3DADAPTER_IDENTIFIER9 id {};
        const HRESULT hr = itf()->GetAdapterIdentifier(c.adapter, c.flags, &id);
        cmd::Reply_AdapterIdentifier r;
        r.requestUid = header_.uid;
        r.hresult = hr;
        r.driver = id.Driver;
        r.description = id.Description;
        r.deviceName = id.DeviceName;
        r.driverVersion = uint64_t(id.DriverVersion.QuadPart);
        r.vendorId = id.VendorId;
        r.deviceId = id.DeviceId;
        r.subSysId = id.SubSysId;
        r.revision = id.Revision;
        std::memcpy(r.deviceIdentifier.data(), &id.DeviceIdentifier, 16);
        r.whqlLevel = id.WHQLLevel;
        send(r);
        return kReplied;
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterModeCount& c) {
        return valueReply(D3D_OK, itf()->GetAdapterModeCount(c.adapter, D3DFORMAT(c.format)));
    }
    HRESULT on(const cmd::IDirect3D9Ex_EnumAdapterModes& c) {
        D3DDISPLAYMODE m {};
        return displayModeReply(itf()->EnumAdapterModes(c.adapter, D3DFORMAT(c.format), c.mode, &m), m);
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterDisplayMode& c) {
        D3DDISPLAYMODE m {};
        return displayModeReply(itf()->GetAdapterDisplayMode(c.adapter, &m), m);
    }
    HRESULT on(const cmd::IDirect3D9Ex_CheckDeviceType& c) {
        return itf()->CheckDeviceType(c.adapter, D3DDEVTYPE(c.deviceType), D3DFORMAT(c.adapterFormat),
                                      D3DFORMAT(c.backBufferFormat), BOOL(c.windowed));
    }
    HRESULT on(const cmd::IDirect3D9Ex_CheckDeviceFormat& c) {
        return itf()->CheckDeviceFormat(c.adapter, D3DDEVTYPE(c.deviceType), D3DFORMAT(c.adapterFormat), c.usage,
                                        D3DRESOURCETYPE(c.resourceType), D3DFORMAT(c.checkFormat));
    }
    HRESULT on(const cmd::IDirect3D9Ex_CheckDeviceMultiSampleType& c) {
        DWORD q = 0;
        const HRESULT hr = itf()->CheckDeviceMultiSampleType(c.adapter, D3DDEVTYPE(c.deviceType), D3DFORMAT(c.surfaceFormat),
                                                             BOOL(c.windowed), D3DMULTISAMPLE_TYPE(c.multiSampleType), &q);
        return valueReply(hr, q);
    }
    HRESULT on(const cmd::IDirect3D9Ex_CheckDepthStencilMatch& c) {
        return itf()->CheckDepthStencilMatch(c.adapter, D3DDEVTYPE(c.deviceType), D3DFORMAT(c.adapterFormat),
                                             D3DFORMAT(c.renderTargetFormat), D3DFORMAT(c.depthStencilFormat));
    }
    HRESULT on(const cmd::IDirect3D9Ex_CheckDeviceFormatConversion& c) {
        return itf()->CheckDeviceFormatConversion(c.adapter, D3DDEVTYPE(c.deviceType), D3DFORMAT(c.sourceFormat),
                                                  D3DFORMAT(c.targetFormat));
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetDeviceCaps& c) {
        D3DCAPS9 caps {};
        cmd::Reply_Caps r;
        r.requestUid = header_.uid;
        r.hresult = itf()->GetDeviceCaps(c.adapter, D3DDEVTYPE(c.deviceType), &caps);
        r.caps = wire::capsToWire(caps);
        send(r);
        return kReplied;
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterMonitor& c) {
        return valueReply(D3D_OK, wire::handleToWire(itf()->GetAdapterMonitor(c.adapter)));
    }
    HRESULT on(const cmd::IDirect3D9Ex_CreateDevice& c) {
        IDirect3DDevice9* d = nullptr;
        const HRESULT hr = createWithWindowFallback([&] {
            D3DPRESENT_PARAMETERS pp = present(c.presentParameters);
            return itf()->CreateDevice(c.adapter, D3DDEVTYPE(c.deviceType), mapWindow(c.focusWindow), c.behaviorFlags, &pp, &d);
        });
        put(c.result, d);
        return hr;
    }
    HRESULT on(const cmd::IDirect3D9Ex_CreateDeviceEx& c) {
        D3DDISPLAYMODEEX ms;
        IDirect3DDevice9Ex* d = nullptr;
        const HRESULT hr = createWithWindowFallback([&] {
            D3DPRESENT_PARAMETERS pp = present(c.presentParameters);
            return itf()->CreateDeviceEx(
                c.adapter, D3DDEVTYPE(c.deviceType), mapWindow(c.focusWindow), c.behaviorFlags, &pp,
                const_cast<D3DDISPLAYMODEEX*>(wire::wordsFromWire<D3DDISPLAYMODEEX, wire::kDisplayModeExWords>(c.fullscreenDisplayMode, ms)),
                &d);
        });
        put(c.result, d);
        return hr;
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterModeCountEx& c) {
        D3DDISPLAYMODEFILTER f;
        return valueReply(D3D_OK, itf()->GetAdapterModeCountEx(
                                      c.adapter, wire::wordsFromWire<D3DDISPLAYMODEFILTER, wire::kDisplayModeFilterWords>(c.filter, f)));
    }
    HRESULT on(const cmd::IDirect3D9Ex_EnumAdapterModesEx& c) {
        D3DDISPLAYMODEFILTER f;
        D3DDISPLAYMODEEX m {sizeof(m)};
        const HRESULT hr = itf()->EnumAdapterModesEx(
            c.adapter, wire::wordsFromWire<D3DDISPLAYMODEFILTER, wire::kDisplayModeFilterWords>(c.filter, f), c.mode, &m);
        return displayModeReply(hr, m, D3DDISPLAYROTATION_IDENTITY);
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterDisplayModeEx& c) {
        D3DDISPLAYMODEEX m {sizeof(m)};
        D3DDISPLAYROTATION rot = D3DDISPLAYROTATION_IDENTITY;
        return displayModeReply(itf()->GetAdapterDisplayModeEx(c.adapter, &m, &rot), m, rot);
    }
    HRESULT on(const cmd::IDirect3D9Ex_GetAdapterLUID& c) {
        LUID l {};
        cmd::Reply_Luid r;
        r.requestUid = header_.uid;
        r.hresult = itf()->GetAdapterLUID(c.adapter, &l);
        r.lowPart = l.LowPart;
        r.highPart = l.HighPart;
        send(r);
        return kReplied;
    }

    // ---- device ----------------------------------------------------------------------------------------------
    HRESULT on(const cmd::IDirect3DDevice9Ex_LinkSwapchain& c) {
        IDirect3DSwapChain9* sc = nullptr;
        const HRESULT hr = dev()->GetSwapChain(c.index, &sc);
        put(c.swapChain, sc);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_LinkBackBuffer& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->GetBackBuffer(c.swapChain, c.index, D3DBACKBUFFER_TYPE_MONO, &s);
        put(c.surface, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_LinkAutoDepthStencil& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->GetDepthStencilSurface(&s);
        put(c.surface, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_UpdateTextureFromBuffer& c) {
        IDxvkLegacyD3DDeviceBridge* b = nullptr;
        if (FAILED(dev()->QueryInterface(__uuidof(IDxvkLegacyD3DDeviceBridge), reinterpret_cast<void**>(&b)))) {
            return E_NOINTERFACE;
        }
        RECT rs;
        POINT ps;
        const HRESULT hr = b->UpdateTextureFromBuffer(get<IDirect3DSurface9>(c.destination), get<IDirect3DSurface9>(c.source),
                                                      wire::rectFromWire(c.sourceRect, rs), wire::pointFromWire(c.destPoint, ps));
        b->Release();
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_IsSupportedSurfaceFormat& c) {
        IDxvkLegacyD3DDeviceBridge* b = nullptr;
        if (FAILED(dev()->QueryInterface(__uuidof(IDxvkLegacyD3DDeviceBridge), reinterpret_cast<void**>(&b)))) {
            return valueReply(E_NOINTERFACE, 0);
        }
        const bool ok = b->IsSupportedSurfaceFormat(D3DFORMAT(c.format));
        b->Release();
        return valueReply(D3D_OK, ok ? 1 : 0);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_GetAvailableTextureMem&) { return valueReply(D3D_OK, dev()->GetAvailableTextureMem()); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_EvictManagedResources&) { return dev()->EvictManagedResources(); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_GetDisplayMode& c) {
        D3DDISPLAYMODE m {};
        return displayModeReply(dev()->GetDisplayMode(c.swapChain, &m), m);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_GetDisplayModeEx& c) {
        D3DDISPLAYMODEEX m {sizeof(m)};
        D3DDISPLAYROTATION rot = D3DDISPLAYROTATION_IDENTITY;
        return displayModeReply(dev()->GetDisplayModeEx(c.swapChain, &m, &rot), m, rot);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetCursorProperties& c) {
        return dev()->SetCursorProperties(c.xHotSpot, c.yHotSpot, get<IDirect3DSurface9>(c.cursorBitmap));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetCursorPosition& c) {
        dev()->SetCursorPosition(c.x, c.y, c.flags);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_ShowCursor& c) { return valueReply(D3D_OK, uint32_t(dev()->ShowCursor(BOOL(c.show)))); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateAdditionalSwapChain& c) {
        D3DPRESENT_PARAMETERS pp = present(c.presentParameters);
        IDirect3DSwapChain9* sc = nullptr;
        const HRESULT hr = dev()->CreateAdditionalSwapChain(&pp, &sc);
        put(c.result, sc);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_Reset& c) {
        D3DPRESENT_PARAMETERS pp = present(c.presentParameters);
        return dev()->Reset(&pp);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_ResetEx& c) {
        D3DPRESENT_PARAMETERS pp = present(c.presentParameters);
        D3DDISPLAYMODEEX ms;
        return dev()->ResetEx(&pp, const_cast<D3DDISPLAYMODEEX*>(
                                        wire::wordsFromWire<D3DDISPLAYMODEEX, wire::kDisplayModeExWords>(c.fullscreenDisplayMode, ms)));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_Present& c) {
        RECT a, b;
        std::vector<uint32_t> rgn;
        return dev()->Present(wire::rectFromWire(c.sourceRect, a), wire::rectFromWire(c.destRect, b),
                              mapWindow(c.destWindowOverride), wire::regionFromWire(c.dirtyRegion, rgn));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_PresentEx& c) {
        RECT a, b;
        std::vector<uint32_t> rgn;
        return dev()->PresentEx(wire::rectFromWire(c.sourceRect, a), wire::rectFromWire(c.destRect, b),
                                mapWindow(c.destWindowOverride), wire::regionFromWire(c.dirtyRegion, rgn),
                                c.flags);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_GetRasterStatus& c) {
        D3DRASTER_STATUS rs {};
        cmd::Reply_RasterStatus r;
        r.requestUid = header_.uid;
        r.hresult = dev()->GetRasterStatus(c.swapChain, &rs);
        r.inVBlank = uint32_t(rs.InVBlank);
        r.scanLine = rs.ScanLine;
        send(r);
        return kReplied;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetDialogBoxMode& c) { return dev()->SetDialogBoxMode(BOOL(c.enable)); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetGammaRamp& c) {
        D3DGAMMARAMP r;
        std::memcpy(&r, c.ramp.data(), sizeof(r));
        dev()->SetGammaRamp(c.swapChain, c.flags, &r);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateTexture& c) {
        IDirect3DTexture9* t = nullptr;
        const HRESULT hr = dev()->CreateTexture(c.width, c.height, c.levels, c.usage, D3DFORMAT(c.format), D3DPOOL(c.pool), &t, nullptr);
        put(c.result, t);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateVolumeTexture& c) {
        IDirect3DVolumeTexture9* t = nullptr;
        const HRESULT hr = dev()->CreateVolumeTexture(c.width, c.height, c.depth, c.levels, c.usage, D3DFORMAT(c.format),
                                                      D3DPOOL(c.pool), &t, nullptr);
        put(c.result, t);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateCubeTexture& c) {
        IDirect3DCubeTexture9* t = nullptr;
        const HRESULT hr = dev()->CreateCubeTexture(c.edgeLength, c.levels, c.usage, D3DFORMAT(c.format), D3DPOOL(c.pool), &t, nullptr);
        put(c.result, t);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateVertexBuffer& c) {
        IDirect3DVertexBuffer9* b = nullptr;
        const HRESULT hr = dev()->CreateVertexBuffer(c.length, c.usage, c.fvf, D3DPOOL(c.pool), &b, nullptr);
        put(c.result, b);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateIndexBuffer& c) {
        IDirect3DIndexBuffer9* b = nullptr;
        const HRESULT hr = dev()->CreateIndexBuffer(c.length, c.usage, D3DFORMAT(c.format), D3DPOOL(c.pool), &b, nullptr);
        put(c.result, b);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateRenderTarget& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->CreateRenderTarget(c.width, c.height, D3DFORMAT(c.format), D3DMULTISAMPLE_TYPE(c.multiSample),
                                                     c.multisampleQuality, BOOL(c.lockable), &s, nullptr);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateDepthStencilSurface& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->CreateDepthStencilSurface(c.width, c.height, D3DFORMAT(c.format),
                                                            D3DMULTISAMPLE_TYPE(c.multiSample), c.multisampleQuality,
                                                            BOOL(c.discard), &s, nullptr);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurface& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->CreateOffscreenPlainSurface(c.width, c.height, D3DFORMAT(c.format), D3DPOOL(c.pool), &s, nullptr);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateRenderTargetEx& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->CreateRenderTargetEx(c.width, c.height, D3DFORMAT(c.format), D3DMULTISAMPLE_TYPE(c.multiSample),
                                                       c.multisampleQuality, BOOL(c.lockable), &s, nullptr, c.usage);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurfaceEx& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->CreateOffscreenPlainSurfaceEx(c.width, c.height, D3DFORMAT(c.format), D3DPOOL(c.pool), &s,
                                                                nullptr, c.usage);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = dev()->CreateDepthStencilSurfaceEx(c.width, c.height, D3DFORMAT(c.format),
                                                              D3DMULTISAMPLE_TYPE(c.multiSample), c.multisampleQuality,
                                                              BOOL(c.discard), &s, nullptr, c.usage);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_UpdateSurface& c) {
        RECT r;
        POINT p;
        return dev()->UpdateSurface(get<IDirect3DSurface9>(c.source), wire::rectFromWire(c.sourceRect, r),
                                    get<IDirect3DSurface9>(c.destination), wire::pointFromWire(c.destPoint, p));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_UpdateTexture& c) {
        return dev()->UpdateTexture(get<IDirect3DBaseTexture9>(c.source), get<IDirect3DBaseTexture9>(c.destination));
    }
    // Read-backs answer with the whole destination surface (Reply_Data).
    HRESULT readBack(HRESULT hr, uint32_t destination) {
        if (FAILED(hr)) {
            return sendData(hr, {});
        }
        return readSurface(get<IDirect3DSurface9>(destination), {}, 0);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_GetRenderTargetData& c) {
        return readBack(dev()->GetRenderTargetData(get<IDirect3DSurface9>(c.renderTarget), get<IDirect3DSurface9>(c.destination)),
                        c.destination);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_GetFrontBufferData& c) {
        return readBack(dev()->GetFrontBufferData(c.swapChain, get<IDirect3DSurface9>(c.destination)), c.destination);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_StretchRect& c) {
        RECT a, b;
        return dev()->StretchRect(get<IDirect3DSurface9>(c.source), wire::rectFromWire(c.sourceRect, a),
                                  get<IDirect3DSurface9>(c.destination), wire::rectFromWire(c.destRect, b),
                                  D3DTEXTUREFILTERTYPE(c.filter));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_ColorFill& c) {
        RECT r;
        return dev()->ColorFill(get<IDirect3DSurface9>(c.surface), wire::rectFromWire(c.rect, r), c.color);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetRenderTarget& c) {
        return dev()->SetRenderTarget(c.index, get<IDirect3DSurface9>(c.surface));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetDepthStencilSurface& c) {
        return dev()->SetDepthStencilSurface(get<IDirect3DSurface9>(c.surface));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_BeginScene&) { return dev()->BeginScene(); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_EndScene&) { return dev()->EndScene(); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_Clear& c) {
        return dev()->Clear(DWORD(c.rects.size() / 4), c.rects.empty() ? nullptr : reinterpret_cast<const D3DRECT*>(c.rects.data()),
                            c.flags, c.color, c.z, c.stencil);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetTransform& c) {
        return dev()->SetTransform(D3DTRANSFORMSTATETYPE(c.state), reinterpret_cast<const D3DMATRIX*>(c.matrix.data()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_MultiplyTransform& c) {
        return dev()->MultiplyTransform(D3DTRANSFORMSTATETYPE(c.state), reinterpret_cast<const D3DMATRIX*>(c.matrix.data()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetViewport& c) {
        D3DVIEWPORT9 v {c.x, c.y, c.width, c.height, c.minZ, c.maxZ};
        return dev()->SetViewport(&v);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetMaterial& c) {
        return dev()->SetMaterial(reinterpret_cast<const D3DMATERIAL9*>(c.material.data()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetLight& c) {
        const D3DLIGHT9 l = wire::lightFromWire(c.type, c.parameters);
        return dev()->SetLight(c.index, &l);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_LightEnable& c) { return dev()->LightEnable(c.index, BOOL(c.enable)); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetClipPlane& c) { return dev()->SetClipPlane(c.index, c.plane.data()); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetRenderState& c) {
        return dev()->SetRenderState(D3DRENDERSTATETYPE(c.state), c.value);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateStateBlock& c) {
        IDirect3DStateBlock9* sb = nullptr;
        const HRESULT hr = dev()->CreateStateBlock(D3DSTATEBLOCKTYPE(c.type), &sb);
        put(c.result, sb);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_BeginStateBlock&) { return dev()->BeginStateBlock(); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_EndStateBlock& c) {
        IDirect3DStateBlock9* sb = nullptr;
        const HRESULT hr = dev()->EndStateBlock(&sb);
        put(c.result, sb);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetClipStatus& c) {
        D3DCLIPSTATUS9 s {c.clipUnion, c.clipIntersection};
        return dev()->SetClipStatus(&s);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetTexture& c) {
        return dev()->SetTexture(c.stage, get<IDirect3DBaseTexture9>(c.texture));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetTextureStageState& c) {
        return dev()->SetTextureStageState(c.stage, D3DTEXTURESTAGESTATETYPE(c.type), c.value);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetSamplerState& c) {
        return dev()->SetSamplerState(c.sampler, D3DSAMPLERSTATETYPE(c.type), c.value);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_ValidateDevice&) {
        DWORD passes = 0;
        const HRESULT hr = dev()->ValidateDevice(&passes);
        return valueReply(hr, passes);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetPaletteEntries& c) {
        return dev()->SetPaletteEntries(c.paletteNumber, reinterpret_cast<const PALETTEENTRY*>(c.entries.data()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetCurrentTexturePalette& c) { return dev()->SetCurrentTexturePalette(c.paletteNumber); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetScissorRect& c) {
        RECT r {c.rect[0], c.rect[1], c.rect[2], c.rect[3]};
        return dev()->SetScissorRect(&r);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetSoftwareVertexProcessing& c) {
        return dev()->SetSoftwareVertexProcessing(BOOL(c.software));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetNPatchMode& c) { return dev()->SetNPatchMode(c.segments); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawPrimitive& c) {
        return dev()->DrawPrimitive(D3DPRIMITIVETYPE(c.primitiveType), c.startVertex, c.primitiveCount);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawIndexedPrimitive& c) {
        return dev()->DrawIndexedPrimitive(D3DPRIMITIVETYPE(c.primitiveType), c.baseVertexIndex, c.minVertexIndex,
                                           c.numVertices, c.startIndex, c.primitiveCount);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawPrimitiveUP& c) {
        return dev()->DrawPrimitiveUP(D3DPRIMITIVETYPE(c.primitiveType), c.primitiveCount, c.vertexData.data(),
                                      c.vertexStreamZeroStride);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawPrimitiveUPHeap& c) {
        ipc::HeapRef ref {c.heapChunk, c.heapBytes};
        const uint8_t* p = s_.heap().resolve(ref);
        const HRESULT hr = p ? dev()->DrawPrimitiveUP(D3DPRIMITIVETYPE(c.primitiveType), c.primitiveCount, p,
                                                      c.vertexStreamZeroStride)
                             : D3DERR_INVALIDCALL;
        if (p != nullptr) {
            s_.heap().release(ref);
        }
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawIndexedPrimitiveUP& c) {
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (p != nullptr && c.indexBytes <= size) {
            hr = dev()->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(c.primitiveType), c.minVertexIndex, c.numVertices,
                                               c.primitiveCount, p, D3DFORMAT(c.indexDataFormat), p + c.indexBytes,
                                               c.vertexStreamZeroStride);
        }
        releasePayload();
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_ProcessVertices& c) {
        return dev()->ProcessVertices(c.srcStartIndex, c.destIndex, c.vertexCount, get<IDirect3DVertexBuffer9>(c.destBuffer),
                                      get<IDirect3DVertexDeclaration9>(c.vertexDecl), c.flags);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateVertexDeclaration& c) {
        IDirect3DVertexDeclaration9* d = nullptr;
        const HRESULT hr = dev()->CreateVertexDeclaration(reinterpret_cast<const D3DVERTEXELEMENT9*>(c.elements.data()), &d);
        put(c.result, d);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetVertexDeclaration& c) {
        return dev()->SetVertexDeclaration(get<IDirect3DVertexDeclaration9>(c.declaration));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetFVF& c) { return dev()->SetFVF(c.fvf); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateVertexShader& c) {
        IDirect3DVertexShader9* s = nullptr;
        const HRESULT hr = dev()->CreateVertexShader(reinterpret_cast<const DWORD*>(c.function.data()), &s);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetVertexShader& c) {
        return dev()->SetVertexShader(get<IDirect3DVertexShader9>(c.shader));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetVertexShaderConstantF& c) {
        return dev()->SetVertexShaderConstantF(c.startRegister, c.data.data(), UINT(c.data.size() / 4));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetVertexShaderConstantI& c) {
        return dev()->SetVertexShaderConstantI(c.startRegister, reinterpret_cast<const int*>(c.data.data()), UINT(c.data.size() / 4));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetVertexShaderConstantB& c) {
        return dev()->SetVertexShaderConstantB(c.startRegister, reinterpret_cast<const BOOL*>(c.data.data()), UINT(c.data.size()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetStreamSource& c) {
        return dev()->SetStreamSource(c.streamNumber, get<IDirect3DVertexBuffer9>(c.vertexBuffer), c.offsetInBytes, c.stride);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetStreamSourceFreq& c) { return dev()->SetStreamSourceFreq(c.streamNumber, c.setting); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetIndices& c) { return dev()->SetIndices(get<IDirect3DIndexBuffer9>(c.indexBuffer)); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreatePixelShader& c) {
        IDirect3DPixelShader9* s = nullptr;
        const HRESULT hr = dev()->CreatePixelShader(reinterpret_cast<const DWORD*>(c.function.data()), &s);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetPixelShader& c) {
        return dev()->SetPixelShader(get<IDirect3DPixelShader9>(c.shader));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetPixelShaderConstantF& c) {
        return dev()->SetPixelShaderConstantF(c.startRegister, c.data.data(), UINT(c.data.size() / 4));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetPixelShaderConstantI& c) {
        return dev()->SetPixelShaderConstantI(c.startRegister, reinterpret_cast<const int*>(c.data.data()), UINT(c.data.size() / 4));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetPixelShaderConstantB& c) {
        return dev()->SetPixelShaderConstantB(c.startRegister, reinterpret_cast<const BOOL*>(c.data.data()), UINT(c.data.size()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawRectPatch& c) {
        D3DRECTPATCH_INFO info;
        return dev()->DrawRectPatch(c.patchHandle, c.numSegs.empty() ? nullptr : c.numSegs.data(),
                                    wire::wordsFromWire<D3DRECTPATCH_INFO, wire::kRectPatchWords>(c.rectPatchInfo, info));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DrawTriPatch& c) {
        D3DTRIPATCH_INFO info;
        return dev()->DrawTriPatch(c.patchHandle, c.numSegs.empty() ? nullptr : c.numSegs.data(),
                                   wire::wordsFromWire<D3DTRIPATCH_INFO, wire::kTriPatchWords>(c.triPatchInfo, info));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_DeletePatch& c) { return dev()->DeletePatch(c.patchHandle); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CreateQuery& c) {
        if (c.result == 0) {
            return dev()->CreateQuery(D3DQUERYTYPE(c.type), nullptr);
        }
        IDirect3DQuery9* q = nullptr;
        const HRESULT hr = dev()->CreateQuery(D3DQUERYTYPE(c.type), &q);
        put(c.result, q);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetConvolutionMonoKernel& c) {
        std::vector<float> rows = c.rows, cols = c.columns;
        return dev()->SetConvolutionMonoKernel(c.width, c.height, rows.empty() ? nullptr : rows.data(),
                                               cols.empty() ? nullptr : cols.data());
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_ComposeRects& c) {
        return dev()->ComposeRects(get<IDirect3DSurface9>(c.source), get<IDirect3DSurface9>(c.destination),
                                   get<IDirect3DVertexBuffer9>(c.srcRectDescs), c.numRects,
                                   get<IDirect3DVertexBuffer9>(c.dstRectDescs), D3DCOMPOSERECTSOP(c.operation), c.xOffset,
                                   c.yOffset);
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetGPUThreadPriority& c) { return dev()->SetGPUThreadPriority(c.priority); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_WaitForVBlank& c) { return dev()->WaitForVBlank(c.swapChain); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CheckResourceResidency& c) {
        std::vector<IDirect3DResource9*> rs;
        for (uint32_t h : c.resources) {
            rs.push_back(get<IDirect3DResource9>(h));
        }
        return dev()->CheckResourceResidency(rs.empty() ? nullptr : rs.data(), UINT32(rs.size()));
    }
    HRESULT on(const cmd::IDirect3DDevice9Ex_SetMaximumFrameLatency& c) { return dev()->SetMaximumFrameLatency(c.maxLatency); }
    HRESULT on(const cmd::IDirect3DDevice9Ex_CheckDeviceState& c) {
        return dev()->CheckDeviceState(mapWindow(c.destinationWindow));
    }

    // ---- state blocks, swap chains -----------------------------------------------------------------------------
    HRESULT on(const cmd::IDirect3DStateBlock9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DStateBlock9_Capture&) { return get<IDirect3DStateBlock9>(header_.handle)->Capture(); }
    HRESULT on(const cmd::IDirect3DStateBlock9_Apply&) { return get<IDirect3DStateBlock9>(header_.handle)->Apply(); }
    IDirect3DSwapChain9* sc() { return get<IDirect3DSwapChain9>(header_.handle); }
    HRESULT on(const cmd::IDirect3DSwapChain9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_Present& c) {
        RECT a, b;
        std::vector<uint32_t> rgn;
        return sc()->Present(wire::rectFromWire(c.sourceRect, a), wire::rectFromWire(c.destRect, b),
                             mapWindow(c.destWindowOverride), wire::regionFromWire(c.dirtyRegion, rgn), c.flags);
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_GetFrontBufferData& c) {
        return readBack(sc()->GetFrontBufferData(get<IDirect3DSurface9>(c.destination)), c.destination);
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_GetBackBuffer& c) {
        IDirect3DSurface9* s = nullptr;
        const HRESULT hr = sc()->GetBackBuffer(c.backBuffer, D3DBACKBUFFER_TYPE(c.type), &s);
        put(c.result, s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_GetRasterStatus&) {
        D3DRASTER_STATUS rs {};
        cmd::Reply_RasterStatus r;
        r.requestUid = header_.uid;
        r.hresult = sc()->GetRasterStatus(&rs);
        r.inVBlank = uint32_t(rs.InVBlank);
        r.scanLine = rs.ScanLine;
        send(r);
        return kReplied;
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_GetDisplayMode&) {
        D3DDISPLAYMODE m {};
        return displayModeReply(sc()->GetDisplayMode(&m), m);
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_GetLastPresentCount&) {
        IDirect3DSwapChain9Ex* ex = nullptr;
        UINT n = 0;
        HRESULT hr = sc()->QueryInterface(__uuidof(IDirect3DSwapChain9Ex), reinterpret_cast<void**>(&ex));
        if (SUCCEEDED(hr)) {
            hr = ex->GetLastPresentCount(&n);
            ex->Release();
        }
        return valueReply(hr, n);
    }
    HRESULT on(const cmd::IDirect3DSwapChain9_GetDisplayModeEx&) {
        IDirect3DSwapChain9Ex* ex = nullptr;
        D3DDISPLAYMODEEX m {sizeof(m)};
        D3DDISPLAYROTATION rot = D3DDISPLAYROTATION_IDENTITY;
        HRESULT hr = sc()->QueryInterface(__uuidof(IDirect3DSwapChain9Ex), reinterpret_cast<void**>(&ex));
        if (SUCCEEDED(hr)) {
            hr = ex->GetDisplayModeEx(&m, &rot);
            ex->Release();
        }
        return displayModeReply(hr, m, rot);
    }

    // ---- resources ---------------------------------------------------------------------------------------------
    HRESULT on(const cmd::IDirect3DResource9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DResource9_SetPriority& c) {
        get<IDirect3DResource9>(header_.handle)->SetPriority(c.priority);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DResource9_PreLoad&) {
        get<IDirect3DResource9>(header_.handle)->PreLoad();
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DBaseTexture9_SetLOD& c) {
        get<IDirect3DBaseTexture9>(header_.handle)->SetLOD(c.lod);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DBaseTexture9_SetAutoGenFilterType& c) {
        return get<IDirect3DBaseTexture9>(header_.handle)->SetAutoGenFilterType(D3DTEXTUREFILTERTYPE(c.filterType));
    }
    HRESULT on(const cmd::IDirect3DBaseTexture9_GenerateMipSubLevels&) {
        get<IDirect3DBaseTexture9>(header_.handle)->GenerateMipSubLevels();
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DVertexDeclaration9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DVertexShader9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DPixelShader9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }

    IDirect3DSurface9* texLevel(uint32_t face, uint32_t level, bool cube) {
        IDirect3DSurface9* s = nullptr;
        if (cube) {
            auto* t = get<IDirect3DCubeTexture9>(header_.handle);
            if (t != nullptr) {
                t->GetCubeMapSurface(D3DCUBEMAP_FACES(face), level, &s);
            }
        } else {
            auto* t = get<IDirect3DTexture9>(header_.handle);
            if (t != nullptr) {
                t->GetSurfaceLevel(level, &s);
            }
        }
        return s;
    }
    IDirect3DVolume9* volLevel(uint32_t level) {
        IDirect3DVolume9* v = nullptr;
        auto* t = get<IDirect3DVolumeTexture9>(header_.handle);
        if (t != nullptr) {
            t->GetVolumeLevel(level, &v);
        }
        return v;
    }
    template <class T>
    static void release(T* p) {
        if (p != nullptr) {
            p->Release();
        }
    }

    HRESULT on(const cmd::IDirect3DTexture9_GetSurfaceLevel& c) {
        put(c.result, texLevel(0, c.level, false));
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DTexture9_LockRect& c) {
        IDirect3DSurface9* s = texLevel(0, c.level, false);
        const HRESULT hr = readSurface(s, c.rect, c.flags);
        release(s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DTexture9_UnlockRect& c) {
        // Through the texture's own LockRect (managed textures track dirty regions per level).
        auto* t = get<IDirect3DTexture9>(header_.handle);
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (t != nullptr && p != nullptr && size == size_t(c.rowBytes) * c.rows) {
            RECT rs;
            D3DLOCKED_RECT lr;
            hr = t->LockRect(c.level, &lr, wire::rectFromWire(c.rect, rs), c.flags & ~DWORD(D3DLOCK_READONLY));
            if (SUCCEEDED(hr)) {
                writeRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, 0, p, c.rowBytes, c.rows, 1);
                hr = t->UnlockRect(c.level);
            }
        }
        releasePayload();
        return hr;
    }
    HRESULT on(const cmd::IDirect3DTexture9_AddDirtyRect& c) {
        RECT r;
        return get<IDirect3DTexture9>(header_.handle)->AddDirtyRect(wire::rectFromWire(c.rect, r));
    }
    HRESULT on(const cmd::IDirect3DCubeTexture9_GetCubeMapSurface& c) {
        put(c.result, texLevel(c.face, c.level, true));
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DCubeTexture9_LockRect& c) {
        IDirect3DSurface9* s = texLevel(c.face, c.level, true);
        const HRESULT hr = readSurface(s, c.rect, c.flags);
        release(s);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DCubeTexture9_UnlockRect& c) {
        auto* t = get<IDirect3DCubeTexture9>(header_.handle);
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (t != nullptr && p != nullptr && size == size_t(c.rowBytes) * c.rows) {
            RECT rs;
            D3DLOCKED_RECT lr;
            hr = t->LockRect(D3DCUBEMAP_FACES(c.face), c.level, &lr, wire::rectFromWire(c.rect, rs), c.flags & ~DWORD(D3DLOCK_READONLY));
            if (SUCCEEDED(hr)) {
                writeRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, 0, p, c.rowBytes, c.rows, 1);
                hr = t->UnlockRect(D3DCUBEMAP_FACES(c.face), c.level);
            }
        }
        releasePayload();
        return hr;
    }
    HRESULT on(const cmd::IDirect3DCubeTexture9_AddDirtyRect& c) {
        RECT r;
        return get<IDirect3DCubeTexture9>(header_.handle)->AddDirtyRect(D3DCUBEMAP_FACES(c.face), wire::rectFromWire(c.rect, r));
    }
    HRESULT on(const cmd::IDirect3DVolumeTexture9_GetVolumeLevel& c) {
        put(c.result, volLevel(c.level));
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DVolumeTexture9_LockBox& c) {
        IDirect3DVolume9* v = volLevel(c.level);
        const HRESULT hr = readVolume(v, c.box, c.flags);
        release(v);
        return hr;
    }
    HRESULT on(const cmd::IDirect3DVolumeTexture9_UnlockBox& c) {
        auto* t = get<IDirect3DVolumeTexture9>(header_.handle);
        size_t size = 0;
        const uint8_t* p = payload(c, size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (t != nullptr && p != nullptr && size == size_t(c.rowBytes) * c.rows * c.slices) {
            D3DBOX bs;
            D3DLOCKED_BOX lb;
            hr = t->LockBox(c.level, &lb, wire::boxFromWire(c.box, bs), c.flags & ~DWORD(D3DLOCK_READONLY));
            if (SUCCEEDED(hr)) {
                writeRows(static_cast<uint8_t*>(lb.pBits), lb.RowPitch, lb.SlicePitch, p, c.rowBytes, c.rows, c.slices);
                hr = t->UnlockBox(c.level);
            }
        }
        releasePayload();
        return hr;
    }
    HRESULT on(const cmd::IDirect3DVolumeTexture9_AddDirtyBox& c) {
        D3DBOX b;
        return get<IDirect3DVolumeTexture9>(header_.handle)->AddDirtyBox(wire::boxFromWire(c.box, b));
    }
    HRESULT on(const cmd::IDirect3DVertexBuffer9_Lock& c) { return readBuffer(get<IDirect3DVertexBuffer9>(header_.handle), c); }
    HRESULT on(const cmd::IDirect3DVertexBuffer9_Unlock& c) { return writeBuffer(get<IDirect3DVertexBuffer9>(header_.handle), c); }
    HRESULT on(const cmd::IDirect3DIndexBuffer9_Lock& c) { return readBuffer(get<IDirect3DIndexBuffer9>(header_.handle), c); }
    HRESULT on(const cmd::IDirect3DIndexBuffer9_Unlock& c) { return writeBuffer(get<IDirect3DIndexBuffer9>(header_.handle), c); }
    HRESULT on(const cmd::IDirect3DSurface9_LockRect& c) { return readSurface(get<IDirect3DSurface9>(header_.handle), c.rect, c.flags); }
    HRESULT on(const cmd::IDirect3DSurface9_UnlockRect& c) { return writeSurface(get<IDirect3DSurface9>(header_.handle), c); }
    HRESULT on(const cmd::IDirect3DVolume9_LockBox& c) { return readVolume(get<IDirect3DVolume9>(header_.handle), c.box, c.flags); }
    HRESULT on(const cmd::IDirect3DVolume9_UnlockBox& c) { return writeVolume(get<IDirect3DVolume9>(header_.handle), c); }

    // ---- queries -----------------------------------------------------------------------------------------------
    HRESULT on(const cmd::IDirect3DQuery9_Destroy&) {
        drop(header_.handle);
        return D3D_OK;
    }
    HRESULT on(const cmd::IDirect3DQuery9_Issue& c) { return get<IDirect3DQuery9>(header_.handle)->Issue(c.issueFlags); }
    HRESULT on(const cmd::IDirect3DQuery9_GetData& c) {
        auto* q = get<IDirect3DQuery9>(header_.handle);
        std::vector<uint8_t> out(c.size);
        HRESULT hr = D3DERR_INVALIDCALL;
        if (q != nullptr) {
            // The client polls; a flushing GetData that is not ready yet answers S_FALSE like D3D9.
            hr = q->GetData(out.empty() ? nullptr : out.data(), c.size, c.getDataFlags);
        }
        if (hr != S_OK) {
            out.clear();
        }
        return sendData(hr, out);
    }

    ipc::Session& s_;
    HMODULE d3d9_;
    FILE* record_;
    ipc::MessageHeader header_ {};
    std::map<uint32_t, IUnknown*> objects_;
    ipc::HeapRef pendingRelease_ {};
    uint32_t unhandled_ = 0;
    std::string firstUnhandled_;
    uint32_t windowMessages_ = 0;
    bool failed_ = false;
    bool proxy_ = false;
    std::map<uint32_t, HWND> proxies_;
};

std::string envString(const char* name) {
    char buf[2048];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n == 0 || n >= sizeof(buf)) ? std::string() : std::string(buf, n);
}

}  // namespace

int main(int argc, char** argv) {
    std::string session, d3d9Path = envString("FUSE_RELIGHT_STUB_D3D9"), recordPath = envString("FUSE_RELIGHT_STUB_RECORD");
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == client::kArgSession) {
            session = argv[++i];
        } else if (a == "--d3d9") {
            d3d9Path = argv[++i];
        } else if (a == "--record") {
            recordPath = argv[++i];
        } else if (a == client::kArgClientPid) {
            ++i;
        }
    }
    if (session.empty()) {
        std::fprintf(stderr, "usage: stub_host --bridge-session <name> [--d3d9 <dll>] [--record <file>]\n");
        return 2;
    }
    HMODULE d3d9 = ::LoadLibraryA(d3d9Path.empty() ? "d3d9.dll" : d3d9Path.c_str());
    if (d3d9 == nullptr) {
        std::fprintf(stderr, "stub host: cannot load %s\n", d3d9Path.c_str());
        return 2;
    }
    std::unique_ptr<ipc::Session> s;
    ipc::Result r = ipc::Session::open(session, 30000, s);
    if (r == ipc::Result::Success) {
        r = s->handshake(30000);
    }
    if (r != ipc::Result::Success) {
        std::fprintf(stderr, "stub host: session %s: %s\n", session.c_str(), ipc::toString(r));
        return 2;
    }
    FILE* record = recordPath.empty() ? nullptr : std::fopen(recordPath.c_str(), "w");
    StubHost host(*s, d3d9, record);
    const int rc = host.run();
    if (record != nullptr) {
        std::fclose(record);
    }
    s->close();
    std::fprintf(stderr, "stub host: done (exit %d, %u window messages)\n", rc, host.windowMessages());
    return rc;
}
