#pragma once

// FUSE provider ABI  ->  NVIDIA Streamline 2.14.1 types (vendored MIT headers, Engine/lib/streamline).
// Pure data mapping with no runtime calls, so the gates compile it and check it on any OS; the
// Streamline provider (fuse_nvplugin_streamline.cpp) uses it to build slSetTagForFrame /
// slSetConstants / slEvaluateFeature arguments.

#include <fuse/renderer/nvidia/fuse_nv_plugin_abi.h>

#include <cstdint>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>

namespace fuse::renderer::nvidia::streamline {

/// FUSE_NV_FEATURE_* -> sl::kFeature*; false for an unknown feature.
bool to_sl_feature(FuseNvFeature feature, sl::Feature& out) noexcept;

/// Buffer kind -> sl::BufferType for `feature`. Colour in/out map to the scaling buffers for
/// SR/RR and to the "uplift" buffers for DLSS 5 neural rendering (NR); the reactive mask maps to
/// DLSS's bias-current-colour hint. False when the kind has no meaning for the feature.
bool to_sl_buffer_type(FuseNvBufferKind kind, FuseNvFeature feature, sl::BufferType& out) noexcept;

sl::ResourceLifecycle to_sl_lifecycle(FuseNvLifecycle lifecycle) noexcept;

/// A tex2d sl::Resource from the native handles (Vulkan: image, memory, view, layout, size, format).
sl::Resource to_sl_resource(const FuseNvResource& r) noexcept;

/// sl::Extent for a tag (all zero = whole resource).
sl::Extent to_sl_extent(const FuseNvResourceTag& tag) noexcept;

/// Every sl::Constants member is written; flags become explicit sl::Boolean eTrue/eFalse (never
/// eInvalid, which Streamline rejects as "missing constant").
void to_sl_constants(const FuseNvConstants& in, sl::Constants& out) noexcept;

sl::DLSSMode to_sl_dlss_mode(FuseNvQuality quality) noexcept;

/// sl::Result -> FUSE provider status.
FuseNvStatus from_sl_result(sl::Result result) noexcept;

} // namespace fuse::renderer::nvidia::streamline
