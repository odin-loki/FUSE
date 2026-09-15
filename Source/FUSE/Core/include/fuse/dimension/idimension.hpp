#pragma once

#include <fuse/dimension/world_handle.hpp>
#include <fuse/frame/frame_ctx.hpp>

namespace fuse::dimension {

/// Stable dimension API (U4) — tick/render hooks for 2D and 3D worlds.
class IDimension {
public:
    virtual ~IDimension() = default;

    virtual const char* dimensionName() const = 0;
    virtual bool isEnabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;

    virtual void tick(frame::FrameCtx& ctx) = 0;
    virtual void render(frame::FrameCtx& ctx) = 0;

    virtual void loadWorld(WorldHandle world) = 0;
    virtual WorldHandle activeWorld() const = 0;
};

} // namespace fuse::dimension
