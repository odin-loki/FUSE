#include "demo_check.hpp"

#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_timeline: fuse_cinematics timeline (Verve-inspired)");

    std::string projectPath = "Samples/unification/demo_timeline";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.cinematics, "project enables fuse_cinematics");

    fuse::cinematics::Timeline timeline;
    timeline.playhead().set_duration_ms(2'000);
    fuse::cinematics::TrackGroup& group = timeline.add_group("Director");
    fuse::cinematics::CameraTrack& cameraTrack = group.add_camera_track("camera_intro");
    cameraTrack.add_event(fuse::cinematics::TimelineEvent("camera_intro", 0, 2'000));
    timeline.play();

    for (int frame = 0; frame < 4; ++frame) {
        timeline.advance(500);
    }

    fuse::demo::check(timeline.groups().size() == 1u, "one group registered");
    fuse::demo::check(group.tracks().size() == 1u, "one track registered");
    fuse::demo::check(cameraTrack.span().end_ms == 2'000, "timeline duration from track span");
    fuse::demo::check(timeline.playhead().time_ms() > 1'000, "playhead advanced");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_timeline");
}
