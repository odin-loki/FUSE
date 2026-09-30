// UNI-INPUT-1: Win32 XInput gamepad backend.
//
// XInput is loaded at run time (xinput1_4.dll, then xinput1_3.dll, then xinput9_1_0.dll), so the
// executable has no import-time dependency and runs where no XInput DLL exists (null pads).
// The undocumented XInputGetStateEx (ordinal 100) is preferred because it also reports the
// Guide button. Each poll reads the 4 user slots and turns the change against the previous
// snapshot into events (diffXInputSnapshot, shared with the portable tests). XInputGetState on
// an empty slot is slow (device enumeration), so empty slots are probed at most once a second.

#include <fuse/platform/gamepad.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

namespace fuse::platform {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

constexpr ULONGLONG kEmptySlotProbeMs = 1000;

class XInputGamepadBackend final : public GamepadBackend {
public:
    XInputGamepadBackend() {
        static const wchar_t* kDlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const wchar_t* dll : kDlls) {
            m_module = LoadLibraryW(dll);
            if (m_module != nullptr) {
                break;
            }
        }
        if (m_module == nullptr) {
            return;
        }
        // Ordinal 100: XInputGetStateEx (same signature; adds XINPUT_GAMEPAD_GUIDE 0x0400).
        m_getState = reinterpret_cast<XInputGetStateFn>(
            reinterpret_cast<void*>(GetProcAddress(m_module, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(100)))));
        if (m_getState == nullptr) {
            m_getState = reinterpret_cast<XInputGetStateFn>(
                reinterpret_cast<void*>(GetProcAddress(m_module, "XInputGetState")));
        }
        m_setState = reinterpret_cast<XInputSetStateFn>(
            reinterpret_cast<void*>(GetProcAddress(m_module, "XInputSetState")));
    }

    ~XInputGamepadBackend() override {
        if (m_module != nullptr) {
            FreeLibrary(m_module);
        }
    }

    XInputGamepadBackend(const XInputGamepadBackend&) = delete;
    XInputGamepadBackend& operator=(const XInputGamepadBackend&) = delete;

    [[nodiscard]] bool available() const { return m_getState != nullptr; }

    [[nodiscard]] const char* name() const override { return "xinput"; }

    u32 poll(std::vector<GamepadEvent>& out) override {
        if (m_getState == nullptr) {
            return 0;
        }
        const usize before = out.size();
        const ULONGLONG now = GetTickCount64();
        for (u32 slot = 0; slot < kMaxGamepads && slot < XUSER_MAX_COUNT; ++slot) {
            XInputPadSnapshot& prev = m_prev[slot];
            if (!prev.connected && m_lastProbe[slot] != 0 && now - m_lastProbe[slot] < kEmptySlotProbeMs) {
                continue;
            }
            XINPUT_STATE state{};
            const DWORD result = m_getState(slot, &state);
            XInputPadSnapshot next{};
            if (result == ERROR_SUCCESS) {
                next.connected = true;
                next.buttons = state.Gamepad.wButtons;
                next.leftTrigger = state.Gamepad.bLeftTrigger;
                next.rightTrigger = state.Gamepad.bRightTrigger;
                next.thumbLX = state.Gamepad.sThumbLX;
                next.thumbLY = state.Gamepad.sThumbLY;
                next.thumbRX = state.Gamepad.sThumbRX;
                next.thumbRY = state.Gamepad.sThumbRY;
            } else {
                m_lastProbe[slot] = now == 0 ? 1 : now;
            }
            diffXInputSnapshot(static_cast<u8>(slot), prev, next, out);
            prev = next;
        }
        return static_cast<u32>(out.size() - before);
    }

    bool setRumble(u32 pad, f32 lowFrequency, f32 highFrequency) override {
        if (m_setState == nullptr || pad >= kMaxGamepads || !m_prev[pad].connected) {
            return false;
        }
        const auto speed = [](f32 v) {
            const f32 c = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            return static_cast<WORD>(c * 65535.f);
        };
        XINPUT_VIBRATION vibration{};
        vibration.wLeftMotorSpeed = speed(lowFrequency);
        vibration.wRightMotorSpeed = speed(highFrequency);
        return m_setState(pad, &vibration) == ERROR_SUCCESS;
    }

private:
    HMODULE m_module = nullptr;
    XInputGetStateFn m_getState = nullptr;
    XInputSetStateFn m_setState = nullptr;
    XInputPadSnapshot m_prev[kMaxGamepads]{};
    ULONGLONG m_lastProbe[kMaxGamepads]{};
};

} // namespace

std::unique_ptr<GamepadBackend> createPlatformGamepadBackend() {
    auto xinput = std::make_unique<XInputGamepadBackend>();
    if (xinput->available()) {
        return xinput;
    }
    return std::make_unique<NullGamepadBackend>();
}

} // namespace fuse::platform
