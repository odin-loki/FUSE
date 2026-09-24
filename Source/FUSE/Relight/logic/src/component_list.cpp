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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_component_list.h@0867d3c and rtx_component_list.cpp@0867d3c
#include <fuse/relight/logic/component_list.hpp>

#include "component_list_internal.hpp"

#include <mutex>

namespace fuse::relight::logic {

void registerAllComponents() {
    static std::once_flag once;
    std::call_once(once, [] {
        components::registerConstComponents();
        components::registerMathComponents();
        components::registerCompareComponents();
        components::registerStateComponents();
        components::registerSenseComponents();
        components::registerOptionComponents();
    });
}

} // namespace fuse::relight::logic
