/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_options.h@0867d3c (fusedWorldViewMode, the
// rtx.viewModel options processCameraData reads; names, types, defaults and descriptions as upstream).
//
// RL-1.5 owns these rtx.* options. Borrowed (declared elsewhere, read by name with the Remix default):
//   rtx.uniqueObjectDistance   (default 300; owner: scene/instances, RL-1.7)
// The other rtx.viewModel.* options (rangeMeters, scale, enableVirtualInstances, perspectiveCorrection)
// drive view-model rendering and belong to the render packages.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/scene/camera/camera_manager.hpp>

namespace fuse::relight::scene {

struct CameraOptions {
    FUSE_RELIGHT_OPTION("rtx", FusedWorldViewMode, fusedWorldViewMode, FusedWorldViewMode::None, "Set if game uses a fused World-View transform matrix.");
    FUSE_RELIGHT_OPTION("rtx.viewModel", bool, enable, false, "If true, try to resolve view models (e.g. first-person weapons). World geometry doesn't have shadows / reflections / etc from the view models.");
    FUSE_RELIGHT_OPTION("rtx.viewModel", float, maxZThreshold, 0.0f, "If a draw call's viewport has max depth less than or equal to this threshold, then assume that it's a view model.");

    /// rtx.uniqueObjectDistance (borrowed; 300 when no package declares it).
    static float uniqueObjectDistance();
};

} // namespace fuse::relight::scene
