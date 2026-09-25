/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/util/util_keybind.h@0867d3c (key names) and the VirtualKeys parsing of
// src/util/config/config.cpp@0867d3c
//
// FUSE Relight RL-3.5: key-combination strings ("CTRL, A", "SHIFT, SPACE") -> Windows virtual-key codes, for the
// KeyboardInput component. Names are upstream's (case-insensitive; single letters and digits are their ASCII
// codes). The overlay (RL-6.1) owns the option-type keybinds; this is only the parser the component needs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::logic {

/// Comma-separated key names -> VK codes in order. False (and `out` cleared) when any name is unknown or the
/// string has no key.
bool parseVirtualKeys(std::string_view text, std::vector<std::uint32_t>& out);
/// The canonical name of a VK code ("" when unknown).
std::string virtualKeyName(std::uint32_t vk);

} // namespace fuse::relight::logic
