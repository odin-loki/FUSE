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
// Ported from dxvk-remix bridge/src/client/{window.h,window.cpp,di_hook.h,di_hook.cpp,remix_state.h}@0867d3c

// FUSE Relight RL-2.2: game window and input handling of the bridge client.
//
// Semantics kept from upstream (window.cpp, di_hook.cpp):
// - The device window is subclassed. Input messages go to the renderer (the host), except WM_INPUT;
//   while the renderer UI is active they are swallowed (the ALT key-up passes), as are
//   WM_MOUSELEAVE, move/size/minimize/maximize system commands and non-client messages other than
//   the close button. Fullscreen devices re-position the window on WM_ACTIVATEAPP and minimize on
//   deactivation unless D3DCREATE_NOWINDOWCHANGES; WM_SIZE re-posts the activation.
// - A game that re-subclasses the window with SetWindowLong(GWLP_WNDPROC) replaces the procedure
//   below ours (GetWindowLong returns the game's procedure), so the bridge stays on top.
// - DirectInput: the device-state and buffered-data reads of keyboards and mice are translated into
//   window messages (input_translate.hpp) and forwarded; while the UI is active the game sees empty
//   state. SetCooperativeLevel records exclusivity (optionally forced non-exclusive).
// - Win32 input: GetCursorPos/SetCursorPos echo a frozen cursor, GetAsyncKeyState/GetKeyState/
//   GetKeyboardState read as released, raw mouse deltas and buttons are zeroed and raw keyboard
//   input repeats the last state, and GetRawInputBuffer drains while the UI is active.
//
// Changes (revamp):
// - No Detours (plan §0.3: dropped). User32 functions and the DirectInput factories are hooked in
//   the game executable's import table; DirectInput device methods by patching the device vtable
//   (shared by every device of that DirectInput implementation). Upstream detoured the functions
//   process-wide; a module that resolves these functions with GetProcAddress is not covered here.
// - DirectInput devices are hooked when the game creates them (through the hooked factories), not
//   by creating a probe mouse device at start-up in every game.
// - The UI state arrives with the host's replies (Bridge_InputState) instead of a separate
//   thread-message channel.
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

namespace fuse::relight::bridge::client {

// Device creation / Reset: subclass the device window and install the input hooks (once).
void windowAttach(HWND hwnd, const D3DPRESENT_PARAMETERS& pp, const D3DDEVICE_CREATION_PARAMETERS& cp);
void windowDetach();
// Called on every Present (keeps the DirectInput window extent current).
void windowPump();
// Host notification: the renderer UI opened / closed.
void inputSetUiActive(bool active);
bool inputUiActive();

// Import-table hook of `module`: replaces the import `dll!func` with `hook`. Returns false if the
// module does not import it. Exposed for the unit test.
bool patchImport(HMODULE module, const char* dll, const char* func, void* hook);
// Installs the Win32 input hooks into `module` (the unit test hooks itself).
void installInputHooks(HMODULE module);
// Counters for tests: messages forwarded to the host and messages swallowed.
struct WindowStats {
    uint32_t forwarded = 0;
    uint32_t swallowed = 0;
    uint32_t directInputMessages = 0;
    uint32_t directInputReads = 0;  // hooked GetDeviceState / GetDeviceData calls
};
WindowStats windowStats();
// Test hook: where forwarded messages go (default: Bridge_WindowMessage to the host).
using MessageSink = void (*)(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void setMessageSinkForTest(MessageSink sink);
// Applies the DirectInput hooks to a device's vtable (called by the hooked factories; exposed for
// the unit test).
void hookDirectInputDevice(void* device);

}  // namespace fuse::relight::bridge::client
