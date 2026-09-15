#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/property_track.hpp>
#include <fuse/cinematics/sprite_track.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testThirtySecondTimelineAdvance() {
    fuse::cinematics::Timeline timeline;
    timeline.playhead().set_duration_ms(30'000);
    timeline.play();

    fuse::cinematics::TimelineMs accumulated = 0;
    constexpr fuse::cinematics::TimelineMs step_ms = 100;
    while (timeline.playhead().is_playing() && accumulated < 35'000) {
        timeline.advance(step_ms);
        accumulated += step_ms;
    }

    expectTrue(timeline.playhead().time_ms() == 30'000, "30s timeline reaches duration");
    expectTrue(timeline.playhead().is_stopped(), "timeline stops at end without loop");
    expectTrue(accumulated >= 30'000, "advance consumed at least 30s of wall time");
}

void testTimelineLoopRewinds() {
    fuse::cinematics::Timeline timeline;
    timeline.playhead().set_duration_ms(1'000);
    timeline.set_loop(true);
    timeline.play();

    timeline.advance(1'000);
    expectTrue(timeline.playhead().is_playing(), "loop keeps timeline playing");
    expectTrue(timeline.playhead().time_ms() == 0, "loop rewinds playhead to start");
}

void testPauseStopsAdvance() {
    fuse::cinematics::Timeline timeline;
    timeline.playhead().set_duration_ms(5'000);
    timeline.play();
    timeline.advance(500);
    timeline.pause();

    const auto paused_at = timeline.playhead().time_ms();
    timeline.advance(1'000);
    expectTrue(timeline.playhead().time_ms() == paused_at, "paused timeline ignores advance");
}

void testTrackSpanAndInterpolation() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Director");
    fuse::cinematics::Track& track = group.add_track("Motion");

    track.add_event(fuse::cinematics::TimelineEvent("intro", 2'000, 3'000));
    track.add_event(fuse::cinematics::TimelineEvent("outro", 10'000, 2'000));
    track.sort_events();

    const fuse::cinematics::TrackSpan span = track.span();
    expectTrue(span.start_ms == 2'000, "span starts at first trigger");
    expectTrue(span.end_ms == 12'000, "span ends at last finish");
    expectTrue(span.length_ms() == 10'000, "span length covers both events");

    timeline.playhead().set_duration_ms(20'000);
    expectNear(track.interpolation_at(0, 20'000), 0.f, 0.001f, "interp at t=0");
    expectNear(track.interpolation_at(1'000, 20'000), 0.5f, 0.001f, "interp halfway to first event");
    expectNear(track.interpolation_at(3'500, 20'000), 0.5f, 0.001f, "interp inside first event");
}

void testReverseInterpolation() {
    fuse::cinematics::Track track("Reverse");
    track.add_event(fuse::cinematics::TimelineEvent("a", 2'000, 2'000));
    track.sort_events();

    expectNear(track.interpolation_at(1'000, 10'000, true), 0.5f, 0.001f, "forward interp");
    expectNear(track.interpolation_at(1'000, 10'000, false), 0.5f, 0.001f, "reverse interp mirrors Verve");
}

void testNextEventIndex() {
    fuse::cinematics::Track track("Audio");
    track.add_event(fuse::cinematics::TimelineEvent("a", 1'000));
    track.add_event(fuse::cinematics::TimelineEvent("b", 5'000));
    track.add_event(fuse::cinematics::TimelineEvent("c", 9'000));
    track.sort_events();

    expectTrue(track.next_event_index(0) == 0, "next event at sequence start");
    expectTrue(track.next_event_index(4'999) == 1, "next event before second trigger");
    expectTrue(track.next_event_index(12'000) == -1, "no next event after last");
}

void testEventTriggerDetection() {
    const fuse::cinematics::TimelineEvent event("cue", 5'000, 1'000);
    expectTrue(event.should_trigger_forward(4'900, 200), "forward crossing detects trigger");
    expectTrue(!event.should_trigger_forward(5'100, 200), "trigger not re-fired after start");
    expectTrue(event.contains_time(5'500), "contains_time inside duration");
    expectTrue(!event.contains_time(6'000), "contains_time after finish");
}

void testInterpolateHelpers() {
    expectNear(fuse::cinematics::lerp(0.f, 10.f, 0.25f), 2.5f, 0.001f, "lerp scalar");
    expectNear(fuse::cinematics::apply_ease(fuse::cinematics::EaseMode::SmoothStep, 0.5f),
               0.5f,
               0.001f,
               "smoothstep midpoint");

    const fuse::cinematics::Vec3 a{0.f, 0.f, 0.f};
    const fuse::cinematics::Vec3 b{10.f, 20.f, 30.f};
    const fuse::cinematics::Vec3 mid = fuse::cinematics::lerp_vec3(a, b, 0.5f);
    expectNear(mid.x, 5.f, 0.001f, "lerp_vec3 x");
    expectNear(mid.y, 10.f, 0.001f, "lerp_vec3 y");
    expectNear(mid.z, 15.f, 0.001f, "lerp_vec3 z");
}

void testCameraTrackSampling() {
    fuse::cinematics::CameraTrack track("MainCamera");
    track.set_target_camera_id("player_cam");

    fuse::cinematics::CameraKeyframe start{};
    start.time_ms = 0;
    start.position = {0.f, 0.f, 5.f};
    start.look_at = {0.f, 0.f, 0.f};
    start.field_of_view = 60.f;

    fuse::cinematics::CameraKeyframe end{};
    end.time_ms = 2'000;
    end.position = {0.f, 0.f, 10.f};
    end.look_at = {0.f, 0.f, 0.f};
    end.field_of_view = 45.f;

    track.add_keyframe(start);
    track.add_keyframe(end);
    track.sort_keyframes();

    expectTrue(track.kind() == fuse::cinematics::TrackKind::Camera, "camera track kind");

    const fuse::cinematics::CameraSample mid = track.sample_at(1'000);
    expectNear(mid.position.z, 7.5f, 0.001f, "camera z midpoint");
    expectNear(mid.field_of_view, 52.5f, 0.001f, "camera fov midpoint");
}

void testSpriteTrackSampling() {
    fuse::cinematics::SpriteTrack track("HeroSprite");
    track.set_target_sprite_id("hero");

    fuse::cinematics::SpriteKeyframe start{};
    start.time_ms = 0;
    start.x = 0.f;
    start.y = 0.f;
    start.alpha = 1.f;

    fuse::cinematics::SpriteKeyframe end{};
    end.time_ms = 1'000;
    end.x = 100.f;
    end.y = 50.f;
    end.alpha = 0.f;

    track.add_keyframe(start);
    track.add_keyframe(end);
    track.sort_keyframes();

    expectTrue(track.kind() == fuse::cinematics::TrackKind::Sprite, "sprite track kind");

    const fuse::cinematics::SpriteSample mid = track.sample_at(500);
    expectNear(mid.x, 50.f, 0.001f, "sprite x midpoint");
    expectNear(mid.y, 25.f, 0.001f, "sprite y midpoint");
    expectNear(mid.alpha, 0.5f, 0.001f, "sprite alpha midpoint");
}

void testPropertyTrackSampling() {
    fuse::cinematics::PropertyTrack track("DoorOpen");
    track.set_property_path("rotation.y");
    track.set_target_object_id("door_01");

    track.add_keyframe({0, 0.f});
    track.add_keyframe({2'000, 90.f});
    track.sort_keyframes();

    expectTrue(track.kind() == fuse::cinematics::TrackKind::Property, "property track kind");
    expectNear(track.sample_at(1'000), 45.f, 0.001f, "property midpoint");
}

void testTimelineContentSpan() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Scene");
    fuse::cinematics::CameraTrack& camera = group.add_camera_track("Cam");
    fuse::cinematics::SpriteTrack& sprite = group.add_sprite_track("Sprite");

    camera.add_event(fuse::cinematics::TimelineEvent("cam_hold", 0, 5'000));
    sprite.add_event(fuse::cinematics::TimelineEvent("sprite_move", 1'000, 4'000));

    const fuse::cinematics::TrackSpan span = timeline.content_span();
    expectTrue(span.start_ms == 0, "content span starts at earliest track");
    expectTrue(span.end_ms == 5'000, "content span ends at latest finish");
    expectTrue(timeline.suggested_duration_ms() == 5'000, "suggested duration from content");
}

} // namespace

int main() {
    fuse::core::initialize();
    testThirtySecondTimelineAdvance();
    testTimelineLoopRewinds();
    testPauseStopsAdvance();
    testTrackSpanAndInterpolation();
    testReverseInterpolation();
    testNextEventIndex();
    testEventTriggerDetection();
    testInterpolateHelpers();
    testCameraTrackSampling();
    testSpriteTrackSampling();
    testPropertyTrackSampling();
    testTimelineContentSpan();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cinematics_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cinematics_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
