// FUSE Relight RL-2.2: cross-architecture wire forms of the D3D9 structures in commands.table.
// Copyright (c) 2026 FUSE contributors (MIT). New code: upstream (dxvk-remix bridge/src/client,
// util_serializable.h) memcpy'd the structs and patched the x86/x64 differences by hand (e.g. the
// "+4 bytes of padding" of D3DADAPTER_IDENTIFIER9 in d3d9_module.cpp).
//
// The table carries D3D structures as typed fields (see the "RL-2.2 wire conventions" block of
// commands.table). The Windows half of this header converts between those fields and the D3D
// structs and static_asserts, on whichever architecture includes it (the i686 client and the x64
// host both do), that every pointer-free struct shipped as words has the same size and field
// offsets on x86 and x64.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fuse::relight::bridge::client::wire {

inline constexpr size_t kPresentParameterWords = 14;  // D3DPRESENT_PARAMETERS
inline constexpr size_t kCapsWords = 76;              // D3DCAPS9 (304 bytes)
inline constexpr size_t kDisplayModeExWords = 6;      // D3DDISPLAYMODEEX
inline constexpr size_t kDisplayModeFilterWords = 3;  // D3DDISPLAYMODEFILTER
inline constexpr size_t kVertexElementWords = 2;      // D3DVERTEXELEMENT9 (8 bytes)
inline constexpr size_t kRectPatchWords = 7;          // D3DRECTPATCH_INFO
inline constexpr size_t kTriPatchWords = 4;           // D3DTRIPATCH_INFO
inline constexpr size_t kLightFloats = 25;            // D3DLIGHT9 after Type
inline constexpr size_t kMaterialFloats = 17;         // D3DMATERIAL9

// Win32 window/monitor/DC handles are 32-bit values on WOW64; the wire keeps the low 32 bits and the
// receiver sign-extends (what WOW64 itself does).
inline uint32_t handleToWire(const void* h) noexcept { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h)); }
template <class H>
inline H handleFromWire(uint32_t v) noexcept {
    return reinterpret_cast<H>(static_cast<intptr_t>(static_cast<int32_t>(v)));
}

}  // namespace fuse::relight::bridge::client::wire

#if defined(_WIN32)
#include <d3d9.h>

namespace fuse::relight::bridge::client::wire {

// ---- layout checks (evaluated by the x86 client and the x64 host alike) --------------------------
static_assert(sizeof(D3DCAPS9) == kCapsWords * 4, "D3DCAPS9 must be 304 pointer-free bytes");
static_assert(offsetof(D3DCAPS9, MaxPixelShader30InstructionSlots) == 300, "D3DCAPS9 layout");
static_assert(sizeof(D3DDISPLAYMODEEX) == kDisplayModeExWords * 4, "D3DDISPLAYMODEEX layout");
static_assert(sizeof(D3DDISPLAYMODEFILTER) == kDisplayModeFilterWords * 4, "D3DDISPLAYMODEFILTER layout");
static_assert(sizeof(D3DVERTEXELEMENT9) == kVertexElementWords * 4, "D3DVERTEXELEMENT9 layout");
static_assert(offsetof(D3DVERTEXELEMENT9, UsageIndex) == 7, "D3DVERTEXELEMENT9 layout");
static_assert(sizeof(D3DRECTPATCH_INFO) == kRectPatchWords * 4, "D3DRECTPATCH_INFO layout");
static_assert(sizeof(D3DTRIPATCH_INFO) == kTriPatchWords * 4, "D3DTRIPATCH_INFO layout");
static_assert(sizeof(D3DLIGHT9) == 4 + kLightFloats * 4, "D3DLIGHT9 layout");
static_assert(offsetof(D3DLIGHT9, Diffuse) == 4 && offsetof(D3DLIGHT9, Phi) == 100, "D3DLIGHT9 layout");
static_assert(sizeof(D3DMATERIAL9) == kMaterialFloats * 4, "D3DMATERIAL9 layout");
static_assert(sizeof(D3DMATRIX) == 64, "D3DMATRIX layout");
static_assert(sizeof(D3DVIEWPORT9) == 24, "D3DVIEWPORT9 layout");
static_assert(sizeof(RECT) == 16 && sizeof(POINT) == 8 && sizeof(D3DBOX) == 24, "RECT/POINT/D3DBOX layout");
static_assert(sizeof(D3DGAMMARAMP) == 768 * 2, "D3DGAMMARAMP layout");
static_assert(sizeof(PALETTEENTRY) == 4, "PALETTEENTRY layout");
static_assert(sizeof(D3DCLIPSTATUS9) == 8, "D3DCLIPSTATUS9 layout");
static_assert(sizeof(D3DSURFACE_DESC) == 32 && sizeof(D3DVOLUME_DESC) == 28, "resource descs are pointer-free");
static_assert(sizeof(D3DVERTEXBUFFER_DESC) == 24 && sizeof(D3DINDEXBUFFER_DESC) == 20, "buffer descs are pointer-free");
static_assert(sizeof(LUID) == 8, "LUID layout");
static_assert(sizeof(D3DRASTER_STATUS) == 8, "D3DRASTER_STATUS layout");
// D3DPRESENT_PARAMETERS holds an HWND, so it differs between x86 (56 bytes) and x64 (64 bytes) and
// travels field by field instead.
static_assert(sizeof(D3DPRESENT_PARAMETERS) == (sizeof(void*) == 4 ? 56 : 64), "D3DPRESENT_PARAMETERS layout");

using PresentWords = std::array<uint32_t, kPresentParameterWords>;

inline PresentWords toWire(const D3DPRESENT_PARAMETERS& p) noexcept {
    return {p.BackBufferWidth, p.BackBufferHeight, uint32_t(p.BackBufferFormat), p.BackBufferCount,
            uint32_t(p.MultiSampleType), p.MultiSampleQuality, uint32_t(p.SwapEffect), handleToWire(p.hDeviceWindow),
            uint32_t(p.Windowed), uint32_t(p.EnableAutoDepthStencil), uint32_t(p.AutoDepthStencilFormat), p.Flags,
            p.FullScreen_RefreshRateInHz, p.PresentationInterval};
}

inline D3DPRESENT_PARAMETERS presentFromWire(const PresentWords& w) noexcept {
    D3DPRESENT_PARAMETERS p {};
    p.BackBufferWidth = w[0];
    p.BackBufferHeight = w[1];
    p.BackBufferFormat = D3DFORMAT(w[2]);
    p.BackBufferCount = w[3];
    p.MultiSampleType = D3DMULTISAMPLE_TYPE(w[4]);
    p.MultiSampleQuality = w[5];
    p.SwapEffect = D3DSWAPEFFECT(w[6]);
    p.hDeviceWindow = handleFromWire<HWND>(w[7]);
    p.Windowed = BOOL(w[8]);
    p.EnableAutoDepthStencil = BOOL(w[9]);
    p.AutoDepthStencilFormat = D3DFORMAT(w[10]);
    p.Flags = w[11];
    p.FullScreen_RefreshRateInHz = w[12];
    p.PresentationInterval = w[13];
    return p;
}

// ---- optional structs as counted arrays (empty = NULL pointer) ------------------------------------
inline std::vector<int32_t> rectToWire(const RECT* r) {
    return r ? std::vector<int32_t> {r->left, r->top, r->right, r->bottom} : std::vector<int32_t> {};
}
inline const RECT* rectFromWire(const std::vector<int32_t>& v, RECT& storage) noexcept {
    if (v.size() != 4) {
        return nullptr;
    }
    storage = RECT {v[0], v[1], v[2], v[3]};
    return &storage;
}
inline std::vector<int32_t> pointToWire(const POINT* p) {
    return p ? std::vector<int32_t> {p->x, p->y} : std::vector<int32_t> {};
}
inline const POINT* pointFromWire(const std::vector<int32_t>& v, POINT& storage) noexcept {
    if (v.size() != 2) {
        return nullptr;
    }
    storage = POINT {v[0], v[1]};
    return &storage;
}
inline std::vector<int32_t> boxToWire(const D3DBOX* b) {
    return b ? std::vector<int32_t> {int32_t(b->Left), int32_t(b->Top), int32_t(b->Right), int32_t(b->Bottom),
                                     int32_t(b->Front), int32_t(b->Back)}
             : std::vector<int32_t> {};
}
inline const D3DBOX* boxFromWire(const std::vector<int32_t>& v, D3DBOX& storage) noexcept {
    if (v.size() != 6) {
        return nullptr;
    }
    storage = D3DBOX {UINT(v[0]), UINT(v[1]), UINT(v[2]), UINT(v[3]), UINT(v[4]), UINT(v[5])};
    return &storage;
}

// RGNDATA: rcBound followed by nCount rectangles.
inline std::vector<int32_t> regionToWire(const RGNDATA* r) {
    std::vector<int32_t> v;
    if (r == nullptr) {
        return v;
    }
    const RECT& b = r->rdh.rcBound;
    v = {b.left, b.top, b.right, b.bottom};
    const auto* rects = reinterpret_cast<const RECT*>(r->Buffer);
    for (DWORD i = 0; i < r->rdh.nCount; ++i) {
        v.insert(v.end(), {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom});
    }
    return v;
}
// Builds an RGNDATA in `storage` (aligned for RGNDATAHEADER) or returns nullptr for an empty vector.
inline const RGNDATA* regionFromWire(const std::vector<int32_t>& v, std::vector<uint32_t>& storage) {
    if (v.size() < 4 || v.size() % 4 != 0) {
        return nullptr;
    }
    const DWORD count = DWORD(v.size() / 4 - 1);
    const size_t bytes = sizeof(RGNDATAHEADER) + count * sizeof(RECT);
    storage.assign((bytes + 3) / 4, 0);
    auto* r = reinterpret_cast<RGNDATA*>(storage.data());
    r->rdh.dwSize = sizeof(RGNDATAHEADER);
    r->rdh.iType = RDH_RECTANGLES;
    r->rdh.nCount = count;
    r->rdh.nRgnSize = DWORD(count * sizeof(RECT));
    r->rdh.rcBound = RECT {v[0], v[1], v[2], v[3]};
    std::memcpy(r->Buffer, v.data() + 4, count * sizeof(RECT));
    return r;
}

template <class T, size_t N>
inline std::vector<uint32_t> wordsToWire(const T* s) {
    static_assert(sizeof(T) == N * 4, "struct must be N 32-bit words");
    std::vector<uint32_t> v;
    if (s != nullptr) {
        v.resize(N);
        std::memcpy(v.data(), s, sizeof(T));
    }
    return v;
}
template <class T, size_t N>
inline const T* wordsFromWire(const std::vector<uint32_t>& v, T& storage) noexcept {
    static_assert(sizeof(T) == N * 4, "struct must be N 32-bit words");
    if (v.size() != N) {
        return nullptr;
    }
    std::memcpy(&storage, v.data(), sizeof(T));
    return &storage;
}

inline std::vector<uint32_t> displayModeExToWire(const D3DDISPLAYMODEEX* m) {
    return wordsToWire<D3DDISPLAYMODEEX, kDisplayModeExWords>(m);
}
inline std::vector<uint32_t> filterToWire(const D3DDISPLAYMODEFILTER* f) {
    return wordsToWire<D3DDISPLAYMODEFILTER, kDisplayModeFilterWords>(f);
}

inline std::array<uint32_t, kCapsWords> capsToWire(const D3DCAPS9& c) noexcept {
    std::array<uint32_t, kCapsWords> w {};
    std::memcpy(w.data(), &c, sizeof(c));
    return w;
}
inline D3DCAPS9 capsFromWire(const std::array<uint32_t, kCapsWords>& w) noexcept {
    D3DCAPS9 c;
    std::memcpy(&c, w.data(), sizeof(c));
    return c;
}

inline std::array<float, kLightFloats> lightToWire(const D3DLIGHT9& l) noexcept {
    std::array<float, kLightFloats> f {};
    std::memcpy(f.data(), &l.Diffuse, sizeof(f));
    return f;
}
inline D3DLIGHT9 lightFromWire(uint32_t type, const std::array<float, kLightFloats>& f) noexcept {
    D3DLIGHT9 l;
    l.Type = D3DLIGHTTYPE(type);
    std::memcpy(&l.Diffuse, f.data(), sizeof(f));
    return l;
}

}  // namespace fuse::relight::bridge::client::wire
#endif  // _WIN32
