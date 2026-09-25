// FUSE Relight RL-2.2: bridge client unit tests (rl_bridge_client_unit).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0). New code.
//
// Portable (Linux + Windows): D3DFORMAT layout, DirectInput -> window-message translation
// (the di_hook semantics), schema shapes the client relies on.
// Windows (MinGW, run under Wine): cross-architecture wire forms of the D3D structures, shadow
// device-state defaults and state-block masks, the import-table hook and the Win32 input hooks
// while the renderer UI is active, window subclassing (forwarding, swallowing, the game
// re-subclassing below the bridge), DirectInput vtable hooks on a real Wine dinput8 device.
#include <fuse/relight/bridge/client/format_layout.hpp>
#include <fuse/relight/bridge/client/input_translate.hpp>
#include <fuse/relight/bridge/client/protocol.hpp>
#include <fuse/relight/bridge/client/wire_types.hpp>
#include <fuse/relight/bridge/schema/commands.gen.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include "../src/device_state.hpp"
#include "../src/window.hpp"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

namespace client = fuse::relight::bridge::client;
namespace schema = fuse::relight::bridge::schema;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(cond)) {                                                                     \
            ++g_failures;                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                                  \
    } while (0)

void testFormatLayout() {
    using client::formatLayout;
    const auto argb = formatLayout(21);  // A8R8G8B8
    CHECK(argb.known && argb.bytesPerBlock == 4 && client::rowPitch(argb, 128) == 512);
    const auto dxt1 = formatLayout(client::fourCC('D', 'X', 'T', '1'));
    CHECK(dxt1.blockWidth == 4 && client::rowPitch(dxt1, 128) == 256 && client::rowCount(dxt1, 96) == 24);
    CHECK(client::rowPitch(dxt1, 2) == 8 && client::rowCount(dxt1, 1) == 1);
    const auto dxt5 = formatLayout(client::fourCC('D', 'X', 'T', '5'));
    CHECK(client::subresourceBytes(dxt5, 64, 64, 1) == 64 * 64);
    CHECK(formatLayout(50).bytesPerBlock == 1 && formatLayout(51).bytesPerBlock == 2);  // L8, A8L8
    CHECK(formatLayout(116).bytesPerBlock == 16 && formatLayout(113).bytesPerBlock == 8);
    CHECK(!formatLayout(0x12345678).known);

    client::RegionLayout rl;
    CHECK(client::regionLayout(argb, 16, 8, 1, client::Region {4, 2, 12, 6, 0, 1}, rl));
    CHECK(rl.offset == 2 * 64 + 4 * 4 && rl.rowBytes == 32 && rl.rows == 4 && rl.pitch == 64);
    CHECK(!client::regionLayout(argb, 16, 8, 1, client::Region {4, 2, 20, 6, 0, 1}, rl));  // outside
    CHECK(!client::regionLayout(argb, 16, 8, 1, client::Region {4, 4, 4, 6, 0, 1}, rl));   // empty
    CHECK(client::regionLayout(dxt1, 16, 16, 1, client::Region {4, 8, 12, 16, 0, 1}, rl));
    CHECK(rl.offset == 2 * 32 + 8 && rl.rowBytes == 16 && rl.rows == 2);
    CHECK(!client::regionLayout(dxt1, 16, 16, 1, client::Region {2, 0, 8, 4, 0, 1}, rl));  // unaligned BC
    CHECK(client::regionLayout(dxt1, 6, 6, 1, client::Region {4, 4, 6, 6, 0, 1}, rl));     // edge blocks
    CHECK(client::regionLayout(argb, 4, 4, 4, client::Region {0, 0, 4, 4, 1, 3}, rl));
    CHECK(rl.slices == 2 && rl.slicePitch == 64 && rl.offset == 64);
}

struct Collect {
    std::vector<client::WindowMessage> msgs;
};

client::InputTranslation testTranslation() {
    client::InputTranslation t;
    t.scanToVk = [](uint32_t scan) -> uint32_t {
        switch (scan) {
        case 0x1E: return 'A';
        case 0x2A: return 0x10;  // VK_SHIFT
        case 0x01: return 0x1B;  // VK_ESCAPE
        default: return 0;
        }
    };
    t.toChar = [](uint32_t vk, uint32_t, const uint8_t* ks) -> int32_t {
        if (vk == 'A') {
            return (ks[0x2A] & 0x80) ? 'A' : 'a';
        }
        return -1;
    };
    return t;
}

void testInputTranslator() {
    using T = client::DirectInputTranslator;
    Collect out;
    T t([&](const client::WindowMessage& m) { out.msgs.push_back(m); }, testTranslation());
    t.setPolicy(client::InputDevice::Keyboard, client::ForwardPolicy::Always);
    t.setPolicy(client::InputDevice::Mouse, client::ForwardPolicy::Always);

    uint8_t ks[256] = {};
    ks[0x1E] = 0x80;
    t.keyboardState(ks);
    CHECK(out.msgs.size() == 2 && out.msgs[0].msg == T::kWM_KEYDOWN && out.msgs[0].wParam == 'A' &&
          out.msgs[1].msg == T::kWM_CHAR && out.msgs[1].wParam == 'a');
    out.msgs.clear();
    t.keyboardState(ks);  // no change, no message
    CHECK(out.msgs.empty());
    ks[0x1E] = 0;
    ks[210] = 0x80;  // DIK_INSERT maps without the table
    ks[0x30] = 0x80; // untranslatable: skipped
    t.keyboardState(ks);
    CHECK(out.msgs.size() == 2 && out.msgs[0].msg == T::kWM_KEYUP && out.msgs[1].wParam == 0x2D);
    out.msgs.clear();
    t.keyboardEvent(0x2A, 0x80);  // buffered shift down
    CHECK(out.msgs.size() == 1 && out.msgs[0].msg == T::kWM_KEYDOWN && out.msgs[0].wParam == 0x10);
    out.msgs.clear();

    // Mouse: relative accumulation, clamping to the window, buttons, wheel, MK_SHIFT from the keyboard.
    t.setWindowSize(100, 50);
    client::MouseInput m;
    m.x = 30;
    m.y = 70;
    t.mouseState(m, false);
    CHECK(t.cursorX() == 30 && t.cursorY() == 50);
    CHECK(out.msgs.size() == 1 && out.msgs[0].msg == T::kWM_MOUSEMOVE && (out.msgs[0].wParam & 0x4) &&
          out.msgs[0].lParam == int64_t(30 | (50 << 16)));
    out.msgs.clear();
    m = client::MouseInput {};
    m.x = -100;
    m.buttons[0] = 0x80;
    t.mouseState(m, false);
    CHECK(t.cursorX() == 0);
    CHECK(out.msgs.size() == 2 && out.msgs[1].msg == T::kWM_LBUTTONDOWN);
    out.msgs.clear();
    m = client::MouseInput {};
    m.buttons[0] = 0x80;
    m.z = 120;
    t.mouseState(m, false);
    CHECK(out.msgs.size() == 1 && out.msgs[0].msg == T::kWM_MOUSEWHEEL && ((out.msgs[0].wParam >> 16) & 0xFFFF) == 120);
    out.msgs.clear();
    t.mouseEvent(T::kOfsButton0, 0, false);  // buffered left button up (a z of 0 also resets the wheel, as upstream)
    // MOUSEMOVE (MK_LBUTTON cleared), LBUTTONUP, MOUSEWHEEL
    CHECK(out.msgs.size() == 3 && out.msgs[0].msg == T::kWM_MOUSEMOVE && out.msgs[0].wParam == 0x4 &&
          out.msgs[1].msg == T::kWM_LBUTTONUP && out.msgs[2].msg == T::kWM_MOUSEWHEEL);
    out.msgs.clear();
    t.mouseState(client::MouseInput {}, false);  // wheel back to 0 is a change too
    out.msgs.clear();
    // Absolute mode.
    m = client::MouseInput {};
    m.x = 7;
    m.y = 9;
    t.mouseState(m, true);
    CHECK(t.cursorX() == 7 && t.cursorY() == 9);
    out.msgs.clear();

    // Policies and exclusivity (upstream DI::ForwardPolicy semantics).
    t.setPolicy(client::InputDevice::Keyboard, client::ForwardPolicy::UiActive);
    t.keyboardEvent(0x01, 0x80);
    CHECK(out.msgs.empty());  // UI inactive: not forwarded
    t.setUiActive(true);
    t.keyboardEvent(0x01, 0x00);
    CHECK(out.msgs.size() == 1 && out.msgs[0].msg == T::kWM_KEYUP && out.msgs[0].wParam == 0x1B);
    out.msgs.clear();
    t.setExclusive(client::InputDevice::Keyboard, false);
    t.keyboardEvent(0x01, 0x80);
    CHECK(out.msgs.empty());  // non-exclusive input reaches the window anyway: not forwarded
    // Mouse state is not tracked while the UI is active.
    m.x = 99;
    t.mouseState(m, true);
    CHECK(t.cursorX() == 7 && out.msgs.empty());
    t.setUiActive(false);
    t.setPolicy(client::InputDevice::Mouse, client::ForwardPolicy::Never);
    t.mouseState(m, true);
    CHECK(out.msgs.empty() && t.cursorX() == 99);
    t.resetMouse();
    CHECK(t.cursorX() == 0 && t.cursorY() == 0);
    // Window extents under 16 pixels are ignored (overlay windows).
    t.setWindowSize(8, 8);
    CHECK(t.windowWidth() == 100 && t.windowHeight() == 50);
}

void testSchemaShapes() {
    // The client and host agree on these through the schema; a few fixed shapes the client relies on.
    schema::cmd::IDirect3D9Ex_CreateDevice c;
    CHECK(c.presentParameters.size() == client::wire::kPresentParameterWords);
    schema::cmd::Reply_Caps r;
    CHECK(r.caps.size() == client::wire::kCapsWords);
    schema::cmd::IDirect3DDevice9Ex_SetLight l;
    CHECK(l.parameters.size() == client::wire::kLightFloats);
    schema::cmd::IDirect3DDevice9Ex_SetMaterial mt;
    CHECK(mt.material.size() == client::wire::kMaterialFloats);
    CHECK(schema::isKnownCommand(uint16_t(schema::CommandId::Bridge_WindowMessage)));
    // Explicit reply kinds ("-> Reply_X" in commands.table), which RL-2.3's host also infers by name.
    using Id = schema::CommandId;
    CHECK(schema::replyOf(Id::IDirect3D9Ex_CheckDeviceMultiSampleType) == Id::Reply_Value);
    CHECK(schema::replyOf(Id::IDirect3D9Ex_CheckDeviceFormat) == Id::Reply_Result);
    CHECK(schema::replyOf(Id::IDirect3DDevice9Ex_CheckDeviceState) == Id::Reply_Result);
    CHECK(schema::replyOf(Id::IDirect3DVertexBuffer9_Lock) == Id::Reply_Data);
    CHECK(schema::replyOf(Id::IDirect3DSurface9_LockRect) == Id::Reply_Data);
    CHECK(schema::replyOf(Id::IDirect3DVolume9_LockBox) == Id::Reply_Data);
    CHECK(schema::replyOf(Id::IDirect3DDevice9Ex_GetRenderTargetData) == Id::Reply_Data);
    CHECK(schema::replyOf(Id::IDirect3D9Ex_GetDeviceCaps) == Id::Reply_Caps);
    CHECK(schema::replyOf(Id::IDirect3DDevice9Ex_SetRenderState) == Id::Invalid);
    CHECK(schema::replyOf(Id::IDirect3DDevice9Ex_Present) == Id::Invalid);  // Reply_Result only when asked
    uint32_t typed = 0;
    for (uint16_t id = 1; id <= schema::kCommandCount; ++id) {
        typed += schema::replyOf(id) != 0 ? 1 : 0;
        CHECK(schema::replyOf(id) == 0 || std::strncmp(schema::commandName(schema::replyOf(id)), "Reply_", 6) == 0);
    }
    CHECK(typed == 42);
    CHECK(client::wire::handleFromWire<void*>(0xFFFFFFFEu) == reinterpret_cast<void*>(intptr_t(-2)));
}

#if defined(_WIN32)

void testWireForms() {
    namespace wire = client::wire;
    D3DPRESENT_PARAMETERS pp {};
    pp.BackBufferWidth = 128;
    pp.BackBufferHeight = 96;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 2;
    pp.SwapEffect = D3DSWAPEFFECT_FLIP;
    pp.hDeviceWindow = reinterpret_cast<HWND>(intptr_t(0x10074));
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const auto w = wire::toWire(pp);
    const D3DPRESENT_PARAMETERS back = wire::presentFromWire(w);
    CHECK(std::memcmp(&back, &pp, sizeof(pp)) == 0);

    RECT rs;
    CHECK(wire::rectFromWire(wire::rectToWire(nullptr), rs) == nullptr);
    const RECT r {1, 2, 3, 4};
    const RECT* rp = wire::rectFromWire(wire::rectToWire(&r), rs);
    CHECK(rp != nullptr && rp->left == 1 && rp->bottom == 4);

    std::vector<uint32_t> storage(64);
    auto* rgn = reinterpret_cast<RGNDATA*>(storage.data());
    rgn->rdh.dwSize = sizeof(RGNDATAHEADER);
    rgn->rdh.iType = RDH_RECTANGLES;
    rgn->rdh.nCount = 2;
    rgn->rdh.rcBound = RECT {0, 0, 10, 10};
    const RECT rects[2] = {{0, 0, 5, 5}, {5, 5, 10, 10}};
    std::memcpy(rgn->Buffer, rects, sizeof(rects));
    std::vector<uint32_t> out;
    const RGNDATA* back2 = wire::regionFromWire(wire::regionToWire(rgn), out);
    CHECK(back2 != nullptr && back2->rdh.nCount == 2 && std::memcmp(back2->Buffer, rects, sizeof(rects)) == 0);

    D3DLIGHT9 l {};
    l.Type = D3DLIGHT_SPOT;
    l.Range = 42.0f;
    l.Phi = 0.5f;
    const D3DLIGHT9 l2 = wire::lightFromWire(l.Type, wire::lightToWire(l));
    CHECK(std::memcmp(&l, &l2, sizeof(l)) == 0);
    D3DCAPS9 caps {};
    caps.MaxTextureWidth = 4096;
    caps.MaxPixelShader30InstructionSlots = 512;
    const D3DCAPS9 caps2 = wire::capsFromWire(wire::capsToWire(caps));
    CHECK(std::memcmp(&caps, &caps2, sizeof(caps)) == 0);
}

void testStateBlocks() {
    client::DeviceState s;
    s.sizeConstants(false);
    client::resetDeviceState(s, true);
    CHECK(s.renderStates[D3DRS_ZENABLE] == D3DZB_TRUE && s.renderStates[D3DRS_LIGHTING] == TRUE);
    CHECK(s.stageStates[0][D3DTSS_COLOROP] == D3DTOP_MODULATE && s.stageStates[1][D3DTSS_COLOROP] == D3DTOP_DISABLE);
    CHECK(s.samplerStates[3][D3DSAMP_ADDRESSU] == D3DTADDRESS_WRAP && s.streamFreqs[5] == 1);
    CHECK(s.transforms[D3DTS_VIEW]._11 == 1.0f && s.transforms[256]._44 == 1.0f);

    client::StateMask pixel, vertex, all;
    pixel.sizeConstants(false);
    vertex.sizeConstants(false);
    all.sizeConstants(false);
    client::stateBlockMask(D3DSBT_PIXELSTATE, pixel);
    client::stateBlockMask(D3DSBT_VERTEXSTATE, vertex);
    client::stateBlockMask(D3DSBT_ALL, all);
    CHECK(pixel.renderStates.test(D3DRS_ALPHABLENDENABLE) && !pixel.renderStates.test(D3DRS_LIGHTING));
    CHECK(vertex.renderStates.test(D3DRS_LIGHTING) && !vertex.renderStates.test(D3DRS_SRCBLEND));
    CHECK(all.transforms.test(D3DTS_WORLD) && !pixel.transforms.test(D3DTS_WORLD));
    CHECK(pixel.pixelShader && !pixel.vertexShader && vertex.vertexShader && all.indices);

    client::DeviceState block;
    block.sizeConstants(false);
    client::resetDeviceState(block, true);
    s.renderStates[D3DRS_ALPHABLENDENABLE] = TRUE;
    s.renderStates[D3DRS_LIGHTING] = FALSE;
    s.lights[3] = D3DLIGHT9 {};
    s.lights[3].Range = 7.0f;
    s.psFloat[4] = 2.5f;
    client::transferState(pixel, s, block);  // Capture
    CHECK(block.renderStates[D3DRS_ALPHABLENDENABLE] == TRUE && block.renderStates[D3DRS_LIGHTING] == TRUE);
    CHECK(block.psFloat[4] == 2.5f && block.lights.empty());
    client::transferState(vertex, s, block);
    CHECK(block.lights.count(3) == 1 && block.lights[3].Range == 7.0f && block.renderStates[D3DRS_LIGHTING] == FALSE);
}

// ---- window + input hooks ----------------------------------------------------------------------------
int g_gameMsgs = 0;
int g_gameMsgs2 = 0;
std::vector<UINT> g_sunk;

LRESULT CALLBACK gameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN || m == WM_MOUSEMOVE) {
        ++g_gameMsgs;
    }
    return DefWindowProcA(h, m, w, l);
}
WNDPROC g_prev2 = nullptr;
LRESULT CALLBACK gameProc2(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        ++g_gameMsgs2;
    }
    return CallWindowProcA(g_prev2, h, m, w, l);
}
void sink(HWND, UINT msg, WPARAM, LPARAM) { g_sunk.push_back(msg); }

void testWindowAndHooks() {
    client::setMessageSinkForTest(&sink);
    client::installInputHooks(GetModuleHandleA(nullptr));  // what windowAttach does for the game exe

    WNDCLASSA wc {};
    wc.lpfnWndProc = &gameProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "rl_bridge_client_test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("rl_bridge_client_test", "t", WS_OVERLAPPEDWINDOW, 0, 0, 128, 96, nullptr, nullptr,
                              wc.hInstance, nullptr);
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr) {
        return;
    }
    D3DPRESENT_PARAMETERS pp {};
    pp.Windowed = TRUE;
    pp.BackBufferWidth = 128;
    pp.BackBufferHeight = 96;
    D3DDEVICE_CREATION_PARAMETERS cp {};
    client::windowAttach(hwnd, pp, cp);

    // Forwarded to the host and delivered to the game.
    SendMessageA(hwnd, WM_KEYDOWN, 'A', 0);
    CHECK(g_gameMsgs == 1 && g_sunk.size() == 1 && g_sunk[0] == WM_KEYDOWN);
    // The game re-subclasses: its new procedure goes below the bridge, GetWindowLongPtr reports it.
    g_prev2 = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&gameProc2)));
    CHECK(g_prev2 == &gameProc);
    CHECK(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hwnd, GWLP_WNDPROC)) == &gameProc2);
    SendMessageA(hwnd, WM_KEYDOWN, 'B', 0);
    CHECK(g_gameMsgs2 == 1 && g_gameMsgs == 2 && g_sunk.size() == 2);

    // Renderer UI active: input is forwarded but swallowed; Win32 input reads as released.
    POINT before {};
    GetCursorPos(&before);
    client::inputSetUiActive(true);
    SendMessageA(hwnd, WM_KEYDOWN, 'C', 0);
    CHECK(g_gameMsgs2 == 1 && g_sunk.size() == 3);
    SendMessageA(hwnd, WM_KEYUP, VK_MENU, 0);  // ALT key-up passes
    CHECK(client::windowStats().swallowed == 1);
    CHECK(GetAsyncKeyState(VK_SHIFT) == 0 && GetKeyState(VK_SHIFT) == 0);
    BYTE keys[256];
    std::memset(keys, 0xFF, sizeof(keys));
    CHECK(GetKeyboardState(keys) && keys[0] == 0 && keys[255] == 0);
    CHECK(SetCursorPos(before.x + 10, before.y + 10));
    POINT during {};
    CHECK(GetCursorPos(&during) && during.x == before.x + 10 && during.y == before.y + 10);  // echoed, OS cursor unmoved
    client::inputSetUiActive(false);
    SendMessageA(hwnd, WM_KEYDOWN, 'D', 0);
    CHECK(g_gameMsgs2 == 2);

    // DirectInput: hooks on a real (Wine) dinput8 keyboard device.
    HMODULE di = LoadLibraryA("dinput8.dll");
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = di ? reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(di, "DirectInput8Create"))) : nullptr;
    IDirectInput8A* dinput = nullptr;
    IDirectInputDevice8A* kb = nullptr;
    if (create == nullptr || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                           reinterpret_cast<void**>(&dinput), nullptr)) ||
        FAILED(dinput->CreateDevice(GUID_SysKeyboard, &kb, nullptr))) {
        std::printf("note: DirectInput unavailable here; vtable hook test skipped\n");
    } else {
        client::hookDirectInputDevice(kb);
        kb->SetDataFormat(&c_dfDIKeyboard);
        kb->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        kb->Acquire();
        BYTE state[256];
        const HRESULT hr = kb->GetDeviceState(sizeof(state), state);
        CHECK(client::windowStats().directInputReads >= 1);
        client::inputSetUiActive(true);
        std::memset(state, 0x80, sizeof(state));
        if (kb->GetDeviceState(sizeof(state), state) == DI_OK) {
            CHECK(state[0] == 0 && state[255] == 0);  // wiped while the UI is active
        }
        client::inputSetUiActive(false);
        std::printf("  DirectInput keyboard: GetDeviceState hr=0x%08lx, %u hooked reads\n", static_cast<unsigned long>(hr),
                    client::windowStats().directInputReads);
        kb->Unacquire();
        kb->Release();
        dinput->Release();
    }

    client::windowDetach();
    CHECK(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hwnd, GWLP_WNDPROC)) == &gameProc2);
    DestroyWindow(hwnd);
    client::setMessageSinkForTest(nullptr);
}

#endif  // _WIN32

}  // namespace

int main() {
    testFormatLayout();
    testInputTranslator();
    testSchemaShapes();
#if defined(_WIN32)
    testWireForms();
    testStateBlocks();
    testWindowAndHooks();
#endif
    std::printf("rl_bridge_client_unit: %d checks, %d failures (pointer width %u)\n", g_checks, g_failures,
                unsigned(sizeof(void*) * 8));
    return g_failures == 0 ? 0 : 1;
}
