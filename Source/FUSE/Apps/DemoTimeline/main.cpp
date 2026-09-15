#include "demo_check.hpp"

#include <fuse/cinematics/timeline.hpp>
#include <fuse/cinematics/track.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_timeline: fuse_cinematics timeline stub (Verve-inspired)");

    std::string projectPath = "Samples/unification/demo_timeline";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.cinematics, "project enables fuse_cinematics");

    fuse::cinematics::Timeline timeline;
    fuse::cinematics::Track cameraTrack;
    cameraTrack.name = "camera_intro";
    cameraTrack.kind = fuse::cinematics::TrackKind::Camera;
    cameraTrack.startTime = 0.f;
    cameraTrack.endTime = 2.f;
    timeline.addTrack(cameraTrack);
    timeline.play();

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.5f;
    for (int frame = 0; frame < 4; ++frame) {
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        timeline.tick(ctx);
    }

    fuse::demo::check(timeline.trackCount() == 1u, "one track registered");
    fuse::demo::check(timeline.duration() == 2.f, "timeline duration from track end");
    fuse::demo::check(timeline.playhead() > 1.f, "playhead advanced");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_timeline");
}
