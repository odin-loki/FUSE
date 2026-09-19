#include <fuse/cinematics/audio_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/cue_payload.hpp>
#include <fuse/cinematics/cue_queue.hpp>
#include <fuse/cinematics/event_track.hpp>
#include <fuse/cinematics/interpolate.hpp>
#include <fuse/cinematics/look_at.hpp>
#include <fuse/cinematics/property_track.hpp>
#include <fuse/cinematics/sprite_track.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

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

void testFovLerpHelpers() {
    expectNear(fuse::cinematics::lerp_fov(60.f, 30.f, 0.5f), 45.f, 0.001f, "lerp_fov midpoint");
    expectNear(fuse::cinematics::clamp_fov(0.f), fuse::cinematics::kMinFovDeg, 0.001f, "clamp_fov min");
    expectNear(fuse::cinematics::clamp_fov(200.f), fuse::cinematics::kMaxFovDeg, 0.001f, "clamp_fov max");
}

void testCameraTrackSampling() {
    fuse::cinematics::CameraTrack track("MainCamera");
    track.set_target_camera_id("player_cam");

    fuse::cinematics::CameraKeyframe start{};
    start.time_ms = 0;
    start.position = {0.f, 0.f, 5.f};
    start.look_at = {0.f, 0.f, 0.f};
    start.field_of_view = 60.f;
    start.roll_deg = 0.f;

    fuse::cinematics::CameraKeyframe end{};
    end.time_ms = 2'000;
    end.position = {0.f, 0.f, 10.f};
    end.look_at = {10.f, 0.f, 0.f};
    end.field_of_view = 45.f;
    end.roll_deg = 90.f;

    track.add_keyframe(start);
    track.add_keyframe(end);
    track.sort_keyframes();

    expectTrue(track.kind() == fuse::cinematics::TrackKind::Camera, "camera track kind");

    const fuse::cinematics::TrackSpan span = track.keyframe_span();
    expectTrue(span.start_ms == 0, "camera keyframe span start");
    expectTrue(span.end_ms == 2'000, "camera keyframe span end");

    const fuse::cinematics::CameraSample mid = track.sample_at(1'000);
    expectNear(mid.position.z, 7.5f, 0.001f, "camera z midpoint");
    expectNear(mid.look_at.x, 5.f, 0.001f, "camera look-at x midpoint");
    expectNear(mid.field_of_view, 52.5f, 0.001f, "camera fov midpoint");
    expectNear(mid.roll_deg, 45.f, 0.001f, "camera roll midpoint");
}

void testCameraLookAtTargetEntity() {
    fuse::cinematics::CameraTrack track("FollowHero");
    fuse::cinematics::LookAtResolver resolver;
    resolver.set_resolve_fn([](const std::string& target_id) -> fuse::cinematics::Vec3 {
        if (target_id == "hero") {
            return {100.f, 0.f, 0.f};
        }
        return {};
    });

    fuse::cinematics::CameraKeyframe start{};
    start.time_ms = 0;
    start.position = {0.f, 2.f, 5.f};
    start.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    start.look_at_target_id = "hero";

    fuse::cinematics::CameraKeyframe end{};
    end.time_ms = 1'000;
    end.position = {0.f, 2.f, 10.f};
    end.look_at_mode = fuse::cinematics::CameraLookAtMode::FixedPoint;
    end.look_at = {0.f, 0.f, 0.f};

    track.add_keyframe(start);
    track.add_keyframe(end);
    track.sort_keyframes();

    const fuse::cinematics::CameraSample startSample = track.sample_at(0, fuse::cinematics::EaseMode::Linear, &resolver);
    expectNear(startSample.look_at.x, 100.f, 0.001f, "entity look-at resolves at start");

    const fuse::cinematics::CameraSample mid = track.sample_at(500, fuse::cinematics::EaseMode::Linear, &resolver);
    expectNear(mid.look_at.x, 50.f, 0.001f, "entity-to-fixed look-at midpoint");
}

void testCameraTrackEmpty() {
    fuse::cinematics::CameraTrack track("EmptyCam");
    expectTrue(track.empty(), "fresh camera track has no keyframes");

    const fuse::cinematics::TrackSpan span = track.keyframe_span();
    expectTrue(span.start_ms == 0 && span.end_ms == 0, "empty track span is zero");

    const fuse::cinematics::CameraSample sample = track.sample_at(500);
    expectNear(sample.position.x, 0.f, 0.001f, "empty track position default");
    expectNear(sample.look_at.x, 0.f, 0.001f, "empty track look-at default");
    expectNear(sample.field_of_view, 60.f, 0.001f, "empty track fov default");
    expectNear(sample.roll_deg, 0.f, 0.001f, "empty track roll default");
    expectNear(sample.look_distance(), 0.f, 0.001f, "empty track look distance");
}

void testCameraTrackFovExtremes() {
    fuse::cinematics::CameraTrack track("WideNarrow");
    fuse::cinematics::CameraKeyframe wide{};
    wide.time_ms = 0;
    wide.field_of_view = 0.f;

    fuse::cinematics::CameraKeyframe narrow{};
    narrow.time_ms = 1'000;
    narrow.field_of_view = 200.f;

    track.add_keyframe(wide);
    track.add_keyframe(narrow);
    track.sort_keyframes();

    const fuse::cinematics::CameraSample start = track.sample_at(0);
    expectNear(start.field_of_view, fuse::cinematics::kMinFovDeg, 0.001f, "fov clamps at min keyframe");

    const fuse::cinematics::CameraSample end = track.sample_at(1'000);
    expectNear(end.field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f, "fov clamps at max keyframe");

    const fuse::cinematics::CameraSample mid = track.sample_at(500);
    const float expected_mid =
        fuse::cinematics::lerp_fov(fuse::cinematics::kMinFovDeg, fuse::cinematics::kMaxFovDeg, 0.5f);
    expectNear(mid.field_of_view, expected_mid, 0.001f, "fov midpoint clamps after lerp on normalized keyframes");
    expectNear(expected_mid, 90.f, 0.001f, "lerp_fov midpoint of clamped extremes");
}

void testCameraLookDirection() {
    fuse::cinematics::CameraTrack track("Aim");
    fuse::cinematics::CameraKeyframe start{};
    start.time_ms = 0;
    start.position = {0.f, 0.f, 0.f};
    start.look_at = {0.f, 0.f, -10.f};

    fuse::cinematics::CameraKeyframe end{};
    end.time_ms = 1'000;
    end.position = {10.f, 0.f, 0.f};
    end.look_at = {10.f, 0.f, 10.f};

    track.add_keyframe(start);
    track.add_keyframe(end);
    track.sort_keyframes();

    const fuse::cinematics::CameraSample sample = track.sample_at(0);
    const fuse::cinematics::Vec3 forward = sample.look_direction();
    expectNear(forward.z, -1.f, 0.001f, "look direction points down -Z at start");
    expectNear(sample.look_distance(), 10.f, 0.001f, "look distance at start");

    const fuse::cinematics::Vec3 helper = fuse::cinematics::camera_look_direction(sample.position, sample.look_at);
    expectNear(helper.z, forward.z, 0.001f, "helper matches sample look direction");
}

void testLookAtResolverFallback() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    keyframe.look_at_target_id = "missing";
    keyframe.look_at = {1.f, 2.f, 3.f};

    fuse::cinematics::LookAtResolver resolver;
    const fuse::cinematics::Vec3 resolved = fuse::cinematics::resolve_look_at_world(keyframe, resolver);
    expectNear(resolved.x, 1.f, 0.001f, "unresolved entity falls back to fixed look-at");
    expectNear(resolved.y, 2.f, 0.001f, "unresolved entity falls back to fixed look-at y");
    expectNear(resolved.z, 3.f, 0.001f, "unresolved entity falls back to fixed look-at z");
}

void testLookAtTryResolveNotFound() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    keyframe.look_at_target_id = "missing";
    keyframe.look_at = {4.f, 5.f, 6.f};

    fuse::cinematics::LookAtResolver resolver;
    resolver.set_try_resolve_fn([](const std::string& target_id, fuse::cinematics::Vec3& out) -> bool {
        if (target_id == "hero") {
            out = {0.f, 0.f, 0.f};
            return true;
        }
        return false;
    });

    const fuse::cinematics::Vec3 missing = fuse::cinematics::resolve_look_at_world(keyframe, resolver);
    expectNear(missing.x, 4.f, 0.001f, "try_resolve miss falls back to fixed look-at");
    expectNear(missing.y, 5.f, 0.001f, "try_resolve miss falls back to fixed look-at y");
    expectNear(missing.z, 6.f, 0.001f, "try_resolve miss falls back to fixed look-at z");

    keyframe.look_at_target_id = "hero";
    const fuse::cinematics::Vec3 origin = fuse::cinematics::resolve_look_at_world(keyframe, resolver);
    expectNear(origin.x, 0.f, 0.001f, "try_resolve accepts world origin target");
    expectNear(origin.y, 0.f, 0.001f, "try_resolve accepts world origin target y");
    expectNear(origin.z, 0.f, 0.001f, "try_resolve accepts world origin target z");
}

void testCameraLookAtCoincidentEdge() {
    fuse::cinematics::CameraTrack track("CoincidentAim");
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.time_ms = 0;
    keyframe.position = {1.f, 2.f, 3.f};
    keyframe.look_at = {1.f, 2.f, 3.f};
    track.add_keyframe(keyframe);

    const fuse::cinematics::CameraSample sample = track.sample_at(0);
    const fuse::cinematics::Vec3 forward = sample.look_direction();
    expectNear(forward.z, -1.f, 0.001f, "coincident look-at uses default forward");
    expectNear(sample.look_distance(), 0.f, 0.001f, "coincident look-at has zero distance");
}

void testCameraKeyframeSampleHelpers() {
    std::vector<fuse::cinematics::CameraKeyframe> keyframes;
    keyframes.push_back({0, {0.f, 0.f, 0.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {0.f, 0.f, -5.f}, {}, 80.f, 0.f});
    keyframes.push_back({2'000, {0.f, 0.f, 10.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {0.f, 0.f, 10.f}, {}, 40.f, 20.f});

    expectNear(fuse::cinematics::sample_camera_position(keyframes, 1'000).z, 5.f, 0.001f, "sample_camera_position midpoint");
    expectNear(fuse::cinematics::sample_camera_field_of_view(keyframes, 1'000), 60.f, 0.001f, "sample_camera_field_of_view midpoint");
    expectNear(fuse::cinematics::sample_camera_roll(keyframes, 1'000), 10.f, 0.001f, "sample_camera_roll midpoint");
    expectNear(fuse::cinematics::sample_camera_look_at(keyframes, 1'000).z, 2.5f, 0.001f, "sample_camera_look_at midpoint");

    const fuse::cinematics::CameraKeyframeBracket before = fuse::cinematics::find_camera_keyframe_bracket(keyframes, -100);
    expectTrue(before.prev_index == -1, "before first keyframe has no prev");
    expectTrue(before.next_index == 0, "before first keyframe points at first index");

    const fuse::cinematics::CameraKeyframeBracket mid = fuse::cinematics::find_camera_keyframe_bracket(keyframes, 1'000);
    expectTrue(mid.prev_index == 0, "mid bracket prev index");
    expectTrue(mid.next_index == 1, "mid bracket next index");
    expectNear(mid.segment_t, 0.5f, 0.001f, "mid bracket eased segment t");

    const fuse::cinematics::CameraKeyframeBracket after = fuse::cinematics::find_camera_keyframe_bracket(keyframes, 5'000);
    expectTrue(after.prev_index == 1, "after last keyframe holds on last index");
    expectTrue(after.next_index == -1, "after last keyframe has no next");
}

void testCameraFovHoldExtrapolation() {
    fuse::cinematics::CameraTrack track("FovHold");
    fuse::cinematics::CameraKeyframe start{};
    start.time_ms = 1'000;
    start.field_of_view = 50.f;
    fuse::cinematics::CameraKeyframe end{};
    end.time_ms = 3'000;
    end.field_of_view = 70.f;
    track.add_keyframe(start);
    track.add_keyframe(end);
    track.sort_keyframes();

    const fuse::cinematics::CameraSample before = track.sample_at(0);
    expectNear(before.field_of_view, 50.f, 0.001f, "fov holds first keyframe before span");

    const fuse::cinematics::CameraSample after = track.sample_at(10'000);
    expectNear(after.field_of_view, 70.f, 0.001f, "fov holds last keyframe after span");

    const fuse::cinematics::CameraSample eased = track.sample_at(2'000, fuse::cinematics::EaseMode::SmoothStep);
    expectNear(eased.field_of_view, 60.f, 0.001f, "fov smoothstep midpoint");
}

void testCameraSampleHelpersEmpty() {
    const std::vector<fuse::cinematics::CameraKeyframe> empty;

    expectTrue(fuse::cinematics::camera_keyframes_empty(empty), "empty keyframe vector guard");
    expectNear(fuse::cinematics::sample_camera_position(empty, 500).x, 0.f, 0.001f, "empty position rail");
    expectNear(fuse::cinematics::sample_camera_field_of_view(empty, 500),
               fuse::cinematics::kDefaultCameraFovDeg,
               0.001f,
               "empty fov rail uses default");
    expectNear(fuse::cinematics::sample_camera_roll(empty, 500), 0.f, 0.001f, "empty roll rail");
    expectNear(fuse::cinematics::sample_camera_look_at(empty, 500).x, 0.f, 0.001f, "empty look-at rail");

    const fuse::cinematics::CameraKeyframeBracket bracket =
        fuse::cinematics::find_camera_keyframe_bracket(empty, 1'000);
    expectTrue(bracket.prev_index == -1, "empty bracket has no prev");
    expectTrue(bracket.next_index == -1, "empty bracket has no next");
    expectNear(bracket.segment_t, 0.f, 0.001f, "empty bracket segment t");
}

void testDefaultCameraSample() {
    const fuse::cinematics::CameraSample defaults = fuse::cinematics::default_camera_sample();
    expectNear(defaults.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f, "default sample fov");
    expectNear(defaults.roll_deg, 0.f, 0.001f, "default sample roll");

    fuse::cinematics::CameraTrack track("Defaults");
    const fuse::cinematics::CameraSample sampled = track.sample_at(1'000);
    expectNear(sampled.field_of_view, defaults.field_of_view, 0.001f, "empty track matches default sample fov");
    expectNear(sampled.roll_deg, defaults.roll_deg, 0.001f, "empty track matches default sample roll");
}

void testCameraKeyframeEntityLookAtStub() {
    fuse::cinematics::CameraKeyframe fixed{};
    fixed.look_at_mode = fuse::cinematics::CameraLookAtMode::FixedPoint;
    fixed.look_at_target_id = "ignored";
    expectTrue(!fuse::cinematics::camera_keyframe_uses_entity_look_at(fixed),
               "fixed point mode does not use entity look-at");

    fuse::cinematics::CameraKeyframe entity{};
    entity.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    entity.look_at_target_id = "hero";
    expectTrue(fuse::cinematics::camera_keyframe_uses_entity_look_at(entity),
               "entity mode with id uses entity look-at");

    entity.look_at_target_id.clear();
    expectTrue(!fuse::cinematics::camera_keyframe_uses_entity_look_at(entity),
               "entity mode without id is not entity-bound");
}

void testCameraTrackClearKeyframes() {
    fuse::cinematics::CameraTrack track("Clearable");
    track.add_keyframe({0, {0.f, 0.f, 5.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 70.f, 10.f});
    expectTrue(!track.empty(), "track has keyframes before clear");

    track.clear_keyframes();
    expectTrue(track.empty(), "clear_keyframes empties track");
    expectTrue(track.keyframe_span().start_ms == 0 && track.keyframe_span().end_ms == 0,
               "cleared track span is zero");

    const fuse::cinematics::CameraSample sample = track.sample_at(250);
    expectNear(sample.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f, "cleared track fov default");
    expectNear(sample.roll_deg, 0.f, 0.001f, "cleared track roll default");
}

void testCameraDuplicateKeyframeTime() {
    fuse::cinematics::CameraTrack track("CoincidentTimes");
    fuse::cinematics::CameraKeyframe first{};
    first.time_ms = 1'000;
    first.field_of_view = 40.f;
    first.roll_deg = 5.f;

    fuse::cinematics::CameraKeyframe duplicate{};
    duplicate.time_ms = 1'000;
    duplicate.field_of_view = 80.f;
    duplicate.roll_deg = 15.f;

    track.add_keyframe(first);
    track.add_keyframe(duplicate);
    track.sort_keyframes();

    const fuse::cinematics::CameraSample at_time = track.sample_at(1'000);
    expectNear(at_time.field_of_view, 80.f, 0.001f, "duplicate time picks later fov keyframe");
    expectNear(at_time.roll_deg, 15.f, 0.001f, "duplicate time picks later roll keyframe");
}

void testCameraTrackMultiKeyframe() {
    fuse::cinematics::CameraTrack track("Dolly");
    track.add_keyframe({0, {0.f, 0.f, 0.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 70.f, 0.f});
    track.add_keyframe({1'000, {0.f, 0.f, 5.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 60.f, 15.f});
    track.add_keyframe({3'000, {0.f, 0.f, 15.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 40.f, -10.f});
    track.sort_keyframes();

    const fuse::cinematics::CameraSample hold = track.sample_at(2'000);
    expectNear(hold.position.z, 10.f, 0.001f, "three-keyframe position midpoint");
    expectNear(hold.field_of_view, 50.f, 0.001f, "three-keyframe fov midpoint");
    expectNear(hold.roll_deg, 2.5f, 0.001f, "three-keyframe roll midpoint");
}

void testNormalizeCameraKeyframe() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.field_of_view = -5.f;
    fuse::cinematics::normalize_camera_keyframe(keyframe);
    expectNear(keyframe.field_of_view, fuse::cinematics::kMinFovDeg, 0.001f, "normalize clamps below min fov");

    keyframe.field_of_view = 250.f;
    fuse::cinematics::normalize_camera_keyframe(keyframe);
    expectNear(keyframe.field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f, "normalize clamps above max fov");

    keyframe.field_of_view = 75.f;
    fuse::cinematics::normalize_camera_keyframe(keyframe);
    expectNear(keyframe.field_of_view, 75.f, 0.001f, "normalize preserves in-range fov");
}

void testCameraTrackAddKeyframeFovGuard() {
    fuse::cinematics::CameraTrack track("Guarded");
    fuse::cinematics::CameraKeyframe wide{};
    wide.time_ms = 0;
    wide.field_of_view = 0.f;
    track.add_keyframe(wide);

    expectNear(track.keyframes().front().field_of_view, fuse::cinematics::kMinFovDeg, 0.001f,
               "add_keyframe normalizes fov on insert");

    const fuse::cinematics::CameraSample sample = track.sample_at(0);
    expectNear(sample.field_of_view, fuse::cinematics::kMinFovDeg, 0.001f, "sampled fov matches normalized keyframe");
}

void testCameraTrackNeedsLookAtResolver() {
    fuse::cinematics::CameraTrack fixed("FixedOnly");
    fixed.add_keyframe({0, {0.f, 0.f, 5.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {0.f, 0.f, 0.f}, {}, 60.f, 0.f});
    expectTrue(!fixed.needs_look_at_resolver(), "fixed-point track does not need resolver");

    std::vector<fuse::cinematics::CameraKeyframe> fixedKeyframes = fixed.keyframes();
    expectTrue(!fuse::cinematics::camera_track_needs_look_at_resolver(fixedKeyframes),
               "fixed-point vector does not need resolver");

    fuse::cinematics::CameraTrack entity("Follow");
    fuse::cinematics::CameraKeyframe follow{};
    follow.time_ms = 0;
    follow.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    follow.look_at_target_id = "hero";
    entity.add_keyframe(follow);
    expectTrue(entity.needs_look_at_resolver(), "entity look-at track needs resolver");
    expectTrue(fuse::cinematics::camera_track_needs_look_at_resolver(entity.keyframes()),
               "entity look-at vector needs resolver");
}

void testSampleCameraKeyframe() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.time_ms = 500;
    keyframe.position = {1.f, 2.f, 3.f};
    keyframe.look_at = {4.f, 5.f, 6.f};
    keyframe.field_of_view = 200.f;
    keyframe.roll_deg = 30.f;

    const fuse::cinematics::CameraSample sample = fuse::cinematics::sample_camera_keyframe(keyframe);
    expectNear(sample.position.x, 1.f, 0.001f, "single keyframe position");
    expectNear(sample.look_at.z, 6.f, 0.001f, "single keyframe look-at");
    expectNear(sample.field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f, "single keyframe fov clamped");
    expectNear(sample.roll_deg, 30.f, 0.001f, "single keyframe roll");
}

void testEntityLookAtWithoutResolver() {
    fuse::cinematics::CameraTrack track("NoResolver");
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.time_ms = 0;
    keyframe.position = {0.f, 2.f, 5.f};
    keyframe.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    keyframe.look_at_target_id = "hero";
    keyframe.look_at = {3.f, 4.f, 5.f};
    track.add_keyframe(keyframe);

    const fuse::cinematics::CameraSample sample = track.sample_at(0);
    expectNear(sample.look_at.x, 3.f, 0.001f, "entity look-at without resolver falls back to fixed point");
    expectNear(sample.look_at.y, 4.f, 0.001f, "entity look-at without resolver falls back y");
    expectNear(sample.look_at.z, 5.f, 0.001f, "entity look-at without resolver falls back z");
}

void testDefaultCameraLookAtForPosition() {
    const fuse::cinematics::Vec3 position{10.f, 20.f, 30.f};
    const fuse::cinematics::Vec3 aim = fuse::cinematics::default_camera_look_at_for_position(position, 5.f);
    expectNear(aim.x, 10.f, 0.001f, "default look-at preserves x");
    expectNear(aim.y, 20.f, 0.001f, "default look-at preserves y");
    expectNear(aim.z, 25.f, 0.001f, "default look-at offsets along -Z");
}

void testCameraBracketSmoothStep() {
    std::vector<fuse::cinematics::CameraKeyframe> keyframes;
    keyframes.push_back({0, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 60.f, 0.f});
    keyframes.push_back({2'000, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 40.f, 0.f});

    const fuse::cinematics::CameraKeyframeBracket bracket =
        fuse::cinematics::find_camera_keyframe_bracket(keyframes, 1'000, fuse::cinematics::EaseMode::SmoothStep);
    expectTrue(bracket.prev_index == 0, "smoothstep bracket prev index");
    expectTrue(bracket.next_index == 1, "smoothstep bracket next index");
    expectNear(bracket.segment_t, 0.5f, 0.001f, "smoothstep bracket eased segment t at midpoint");
}

void testCameraKeyframeFovUnset() {
    expectTrue(fuse::cinematics::camera_keyframe_fov_unset(0.f), "zero fov is unset");
    expectTrue(fuse::cinematics::camera_keyframe_fov_unset(-10.f), "negative fov is unset");
    expectTrue(!fuse::cinematics::camera_keyframe_fov_unset(45.f), "positive fov is set");

    expectNear(fuse::cinematics::effective_camera_fov(0.f),
               fuse::cinematics::kDefaultCameraFovDeg,
               0.001f,
               "effective fov uses default when unset");
    expectNear(fuse::cinematics::effective_camera_fov(200.f),
               fuse::cinematics::kMaxFovDeg,
               0.001f,
               "effective fov clamps explicit values");
    expectNear(fuse::cinematics::effective_camera_fov(75.f), 75.f, 0.001f, "effective fov preserves in-range value");
}

void testApplyCameraKeyframeDefaults() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.position = {5.f, 10.f, 15.f};
    keyframe.field_of_view = 0.f;

    fuse::cinematics::apply_camera_keyframe_defaults(keyframe);
    expectNear(keyframe.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f, "defaults fill unset fov");
    expectNear(keyframe.look_at.z, 5.f, 0.001f, "defaults fill unset look-at along -Z");
    expectNear(keyframe.look_at.x, 5.f, 0.001f, "defaults preserve look-at x");
    expectNear(keyframe.look_at.y, 10.f, 0.001f, "defaults preserve look-at y");

    keyframe.field_of_view = 250.f;
    keyframe.look_at = {1.f, 2.f, 3.f};
    fuse::cinematics::apply_camera_keyframe_defaults(keyframe);
    expectNear(keyframe.field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f, "defaults clamp explicit fov");
    expectNear(keyframe.look_at.z, 3.f, 0.001f, "defaults preserve explicit look-at");
}

void testMakeDefaultCameraKeyframe() {
    const fuse::cinematics::CameraKeyframe keyframe = fuse::cinematics::make_default_camera_keyframe(500);
    expectTrue(keyframe.time_ms == 500, "default keyframe time");
    expectNear(keyframe.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f, "default keyframe fov");
    expectNear(keyframe.look_at.z, -fuse::cinematics::kDefaultCameraLookAtDistance, 0.001f,
               "default keyframe look-at offset");
}

void testCameraTrackCoversTime() {
    const std::vector<fuse::cinematics::CameraKeyframe> empty;
    expectTrue(fuse::cinematics::camera_keyframe_count(empty) == 0, "empty keyframe count");
    expectTrue(!fuse::cinematics::camera_track_covers_time(empty, 500), "empty track covers no time");

    fuse::cinematics::CameraTrack track("Span");
    track.add_keyframe({1'000, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 60.f, 0.f});
    track.add_keyframe({3'000, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 45.f, 0.f});
    track.sort_keyframes();

    expectTrue(track.keyframe_count() == 2, "track keyframe count");
    expectTrue(!track.covers_time(500), "time before span is uncovered");
    expectTrue(track.covers_time(2'000), "time inside span is covered");
    expectTrue(track.covers_time(3'000), "time at span end is covered");
    expectTrue(!track.covers_time(4'000), "time after span is uncovered");
}

void testCameraKeyframeLookAtUnset() {
    fuse::cinematics::CameraKeyframe unset{};
    unset.look_at_mode = fuse::cinematics::CameraLookAtMode::FixedPoint;
    expectTrue(fuse::cinematics::camera_keyframe_look_at_unset(unset), "zero fixed look-at is unset");

    unset.look_at = {0.f, 0.f, 1.f};
    expectTrue(!fuse::cinematics::camera_keyframe_look_at_unset(unset), "non-zero fixed look-at is set");

    unset.look_at = {};
    unset.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    expectTrue(!fuse::cinematics::camera_keyframe_look_at_unset(unset), "entity mode is never unset stub");
}

void testResolveLookAtWorldOrDefault() {
    fuse::cinematics::CameraKeyframe fixed{};
    fixed.position = {0.f, 0.f, 20.f};
    fixed.look_at_mode = fuse::cinematics::CameraLookAtMode::FixedPoint;

    fuse::cinematics::LookAtResolver resolver;
    const fuse::cinematics::Vec3 defaulted =
        fuse::cinematics::resolve_look_at_world_or_default(fixed, resolver, 5.f);
    expectNear(defaulted.z, 15.f, 0.001f, "unset fixed look-at uses position default");

    fixed.look_at = {1.f, 2.f, 3.f};
    const fuse::cinematics::Vec3 explicitAim =
        fuse::cinematics::resolve_look_at_world_or_default(fixed, resolver);
    expectNear(explicitAim.x, 1.f, 0.001f, "explicit fixed look-at preserved");
}

void testLookAtResolverResolveOr() {
    fuse::cinematics::LookAtResolver resolver;
    resolver.set_try_resolve_fn([](const std::string& target_id, fuse::cinematics::Vec3& out) -> bool {
        if (target_id == "hero") {
            out = {10.f, 0.f, 0.f};
            return true;
        }
        return false;
    });

    const fuse::cinematics::Vec3 fallback{1.f, 2.f, 3.f};
    const fuse::cinematics::Vec3 missing = resolver.resolve_or("missing", fallback);
    expectNear(missing.x, 1.f, 0.001f, "resolve_or returns fallback on miss");
    expectNear(missing.y, 2.f, 0.001f, "resolve_or fallback y");
    expectNear(missing.z, 3.f, 0.001f, "resolve_or fallback z");

    const fuse::cinematics::Vec3 hero = resolver.resolve_or("hero", fallback);
    expectNear(hero.x, 10.f, 0.001f, "resolve_or returns resolved target");
}

void testDefaultCameraFovDeg() {
    expectNear(fuse::cinematics::default_camera_fov_deg(),
               fuse::cinematics::kDefaultCameraFovDeg,
               0.001f,
               "default_camera_fov_deg matches constexpr");
    expectNear(fuse::cinematics::default_camera_fov_deg(), 60.f, 0.001f, "default_camera_fov_deg is 60");
}

void testDefaultCameraSampleAt() {
    const fuse::cinematics::Vec3 position{5.f, 10.f, 20.f};
    const fuse::cinematics::CameraSample sample = fuse::cinematics::default_camera_sample_at(position, 8.f);

    expectNear(sample.position.x, 5.f, 0.001f, "default sample at preserves x");
    expectNear(sample.position.y, 10.f, 0.001f, "default sample at preserves y");
    expectNear(sample.position.z, 20.f, 0.001f, "default sample at preserves z");
    expectNear(sample.look_at.z, 12.f, 0.001f, "default sample at offsets look-at along -Z");
    expectNear(sample.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f, "default sample at fov");
    expectNear(sample.roll_deg, 0.f, 0.001f, "default sample at roll");
}

void testNormalizeCameraSample() {
    fuse::cinematics::CameraSample sample{};
    sample.position = {1.f, 2.f, 3.f};
    sample.look_at = {4.f, 5.f, 6.f};
    sample.field_of_view = 250.f;
    sample.roll_deg = 45.f;

    fuse::cinematics::normalize_camera_sample(sample);
    expectNear(sample.field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f, "normalize_camera_sample clamps fov");
    expectNear(sample.roll_deg, 45.f, 0.001f, "normalize_camera_sample preserves roll");
    expectNear(sample.position.x, 1.f, 0.001f, "normalize_camera_sample preserves position");
}

void testCameraTrackIsEmpty() {
    fuse::cinematics::CameraTrack track("AliasGuard");
    expectTrue(fuse::cinematics::camera_track_is_empty(track), "empty track alias");

    track.add_keyframe({0, {0.f, 0.f, 5.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 60.f, 0.f});
    expectTrue(!fuse::cinematics::camera_track_is_empty(track), "non-empty track alias");
}

void testCameraKeyframeEntityTargetMissing() {
    fuse::cinematics::CameraKeyframe fixed{};
    fixed.look_at_mode = fuse::cinematics::CameraLookAtMode::FixedPoint;
    expectTrue(!fuse::cinematics::camera_keyframe_entity_target_missing(fixed),
               "fixed point is not missing entity target");

    fuse::cinematics::CameraKeyframe entity{};
    entity.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    entity.look_at_target_id = "hero";
    expectTrue(!fuse::cinematics::camera_keyframe_entity_target_missing(entity),
               "entity with id is not missing target");

    entity.look_at_target_id.clear();
    expectTrue(fuse::cinematics::camera_keyframe_entity_target_missing(entity),
               "entity without id is missing target");
}

void testEntityLookAtMissingTargetIdFallback() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    keyframe.look_at = {7.f, 8.f, 9.f};

    fuse::cinematics::LookAtResolver resolver;
    resolver.set_resolve_fn([](const std::string&) -> fuse::cinematics::Vec3 { return {99.f, 0.f, 0.f}; });

    const fuse::cinematics::Vec3 resolved = fuse::cinematics::resolve_look_at_world(keyframe, resolver);
    expectNear(resolved.x, 7.f, 0.001f, "missing entity id falls back to fixed look-at");
    expectNear(resolved.y, 8.f, 0.001f, "missing entity id falls back to fixed look-at y");
    expectNear(resolved.z, 9.f, 0.001f, "missing entity id falls back to fixed look-at z");
}

void testLookAtResolverHasTarget() {
    fuse::cinematics::LookAtResolver unconfigured;
    expectTrue(!fuse::cinematics::look_at_resolver_has_target(unconfigured, "hero"),
               "unconfigured resolver has no targets");
    expectTrue(!unconfigured.has_target("hero"), "has_target mirrors unconfigured guard");

    fuse::cinematics::LookAtResolver resolver;
    resolver.set_try_resolve_fn([](const std::string& target_id, fuse::cinematics::Vec3& out) -> bool {
        if (target_id == "hero") {
            out = {1.f, 2.f, 3.f};
            return true;
        }
        return false;
    });

    expectTrue(fuse::cinematics::look_at_resolver_has_target(resolver, "hero"), "resolver finds known target");
    expectTrue(resolver.has_target("hero"), "has_target finds known target");
    expectTrue(!fuse::cinematics::look_at_resolver_has_target(resolver, "missing"),
               "resolver rejects unknown target");
    expectTrue(!resolver.has_target(""), "empty target id is rejected");
}

void testCameraFovRangeGuards() {
    expectTrue(fuse::cinematics::camera_fov_in_valid_range(60.f), "default fov is in range");
    expectTrue(fuse::cinematics::camera_fov_in_valid_range(fuse::cinematics::kMinFovDeg), "min fov is in range");
    expectTrue(fuse::cinematics::camera_fov_in_valid_range(fuse::cinematics::kMaxFovDeg), "max fov is in range");
    expectTrue(!fuse::cinematics::camera_fov_in_valid_range(0.f), "zero fov is out of range");
    expectTrue(fuse::cinematics::camera_fov_needs_clamp(250.f), "high fov needs clamp");
    expectTrue(!fuse::cinematics::camera_fov_needs_clamp(75.f), "in-range fov does not need clamp");
    expectTrue(fuse::cinematics::camera_fov_uses_default(fuse::cinematics::kDefaultCameraFovDeg),
               "default fov constant matches guard");
    expectTrue(!fuse::cinematics::camera_fov_uses_default(45.f), "non-default fov rejected by guard");
}

void testEffectiveCameraFovNonFinite() {
    expectNear(fuse::cinematics::effective_camera_fov(std::numeric_limits<float>::quiet_NaN()),
               fuse::cinematics::kDefaultCameraFovDeg,
               0.001f,
               "non-finite fov falls back to default");
    expectNear(fuse::cinematics::effective_camera_fov(std::numeric_limits<float>::infinity()),
               fuse::cinematics::kDefaultCameraFovDeg,
               0.001f,
               "infinite fov falls back to default");
}

void testNormalizeCameraKeyframesBatch() {
    std::vector<fuse::cinematics::CameraKeyframe> keyframes;
    keyframes.push_back({0, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 0.f, 0.f});
    keyframes.push_back({1'000, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {}, {}, 200.f, 0.f});

    fuse::cinematics::normalize_camera_keyframes(keyframes);
    expectNear(keyframes[0].field_of_view, fuse::cinematics::kMinFovDeg, 0.001f, "batch normalize clamps min");
    expectNear(keyframes[1].field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f, "batch normalize clamps max");

    fuse::cinematics::CameraTrack track("BatchNormalize");
    track.keyframes() = keyframes;
    track.normalize_keyframes();
    expectNear(track.keyframes()[1].field_of_view, fuse::cinematics::kMaxFovDeg, 0.001f,
               "track normalize_keyframes clamps stored keyframes");
}

void testCameraSampleIsDefault() {
    const fuse::cinematics::CameraSample defaults = fuse::cinematics::default_camera_sample();
    expectTrue(fuse::cinematics::camera_sample_is_default(defaults), "default sample passes is_default guard");

    fuse::cinematics::CameraTrack track("DefaultGuard");
    const fuse::cinematics::CameraSample sampled = track.sample_at(1'000);
    expectTrue(fuse::cinematics::camera_sample_is_default(sampled), "empty track sample is default");

    fuse::cinematics::CameraSample nonDefault{};
    nonDefault.position.z = 1.f;
    expectTrue(!fuse::cinematics::camera_sample_is_default(nonDefault), "non-default position fails guard");
}

void testResetCameraKeyframeToDefaults() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.time_ms = 2'000;
    keyframe.position = {1.f, 2.f, 3.f};
    keyframe.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    keyframe.look_at_target_id = "hero";
    keyframe.look_at = {4.f, 5.f, 6.f};
    keyframe.field_of_view = 30.f;
    keyframe.roll_deg = 12.f;

    fuse::cinematics::reset_camera_keyframe_to_defaults(keyframe);
    expectTrue(keyframe.time_ms == 0, "reset clears time");
    expectNear(keyframe.position.x, 0.f, 0.001f, "reset clears position");
    expectTrue(keyframe.look_at_mode == fuse::cinematics::CameraLookAtMode::FixedPoint, "reset fixed look-at mode");
    expectTrue(keyframe.look_at_target_id.empty(), "reset clears entity id");
    expectNear(keyframe.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f, "reset restores default fov");
    expectNear(keyframe.roll_deg, 0.f, 0.001f, "reset clears roll");
}

void testSampleCameraPose() {
    std::vector<fuse::cinematics::CameraKeyframe> keyframes;
    keyframes.push_back({0, {0.f, 0.f, 0.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {0.f, 0.f, -10.f}, {}, 60.f, 0.f});
    keyframes.push_back({2'000, {0.f, 0.f, 10.f}, fuse::cinematics::CameraLookAtMode::FixedPoint, {0.f, 0.f, 10.f}, {}, 40.f, 20.f});

    const fuse::cinematics::CameraSample mid = fuse::cinematics::sample_camera_pose(keyframes, 1'000);
    expectNear(mid.position.z, 5.f, 0.001f, "sample_camera_pose position midpoint");
    expectNear(mid.field_of_view, 50.f, 0.001f, "sample_camera_pose fov midpoint");
    expectNear(mid.roll_deg, 10.f, 0.001f, "sample_camera_pose roll midpoint");

    const fuse::cinematics::CameraSample defaults = fuse::cinematics::sample_camera_pose({}, 500);
    expectNear(defaults.field_of_view, fuse::cinematics::kDefaultCameraFovDeg, 0.001f,
               "sample_camera_pose empty vector returns default fov");
}

void testResolveLookAtOrFallback() {
    fuse::cinematics::CameraKeyframe keyframe{};
    keyframe.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    keyframe.look_at_target_id = "missing";
    keyframe.look_at = {9.f, 9.f, 9.f};

    fuse::cinematics::LookAtResolver resolver;
    resolver.set_try_resolve_fn([](const std::string& target_id, fuse::cinematics::Vec3& out) -> bool {
        if (target_id == "hero") {
            out = {100.f, 0.f, 0.f};
            return true;
        }
        return false;
    });

    const fuse::cinematics::Vec3 fallback{3.f, 4.f, 5.f};
    const fuse::cinematics::Vec3 unresolved =
        fuse::cinematics::resolve_look_at_or_fallback(keyframe, resolver, fallback);
    expectNear(unresolved.x, 3.f, 0.001f, "resolve_look_at_or_fallback uses fallback when unresolved");

    keyframe.look_at_target_id = "hero";
    const fuse::cinematics::Vec3 resolved =
        fuse::cinematics::resolve_look_at_or_fallback(keyframe, resolver, fallback);
    expectNear(resolved.x, 100.f, 0.001f, "resolve_look_at_or_fallback resolves entity target");
}

void testCollectCameraLookAtTargetIds() {
    std::vector<fuse::cinematics::CameraKeyframe> keyframes;
    keyframes.push_back({0, {}, fuse::cinematics::CameraLookAtMode::TargetEntity, {}, "hero", 60.f, 0.f});
    keyframes.push_back({1'000, {}, fuse::cinematics::CameraLookAtMode::TargetEntity, {}, "hero", 60.f, 0.f});
    keyframes.push_back({2'000, {}, fuse::cinematics::CameraLookAtMode::TargetEntity, {}, "boss", 60.f, 0.f});
    keyframes.push_back({3'000, {}, fuse::cinematics::CameraLookAtMode::FixedPoint, {0.f, 0.f, 0.f}, "ignored", 60.f, 0.f});

    const std::vector<std::string> ids = fuse::cinematics::collect_camera_look_at_target_ids(keyframes);
    expectTrue(ids.size() == 2, "collect deduplicates entity look-at ids");
    expectTrue(ids[0] == "hero", "collect preserves first entity id");
    expectTrue(ids[1] == "boss", "collect preserves second entity id");
    expectTrue(fuse::cinematics::camera_keyframe_entity_look_at_count(keyframes) == 3,
               "entity look-at count includes duplicate ids");
}

void testLookAtResolverAvailability() {
    expectTrue(!fuse::cinematics::look_at_resolver_available(nullptr), "null resolver unavailable");

    fuse::cinematics::LookAtResolver empty;
    expectTrue(!fuse::cinematics::look_at_resolver_available(&empty), "empty resolver unavailable");

    fuse::cinematics::LookAtResolver resolver;
    resolver.set_resolve_fn([](const std::string&) -> fuse::cinematics::Vec3 { return {}; });
    expectTrue(fuse::cinematics::look_at_resolver_available(&resolver), "resolver with callback available");
    expectTrue(fuse::cinematics::look_at_resolver_can_resolve_target(resolver, "any"),
               "legacy resolve_fn always resolves target");
}

void testLookAtResolverCanResolveTarget() {
    fuse::cinematics::LookAtResolver resolver;
    resolver.set_try_resolve_fn([](const std::string& target_id, fuse::cinematics::Vec3& out) -> bool {
        if (target_id == "hero") {
            out = {1.f, 2.f, 3.f};
            return true;
        }
        return false;
    });

    expectTrue(fuse::cinematics::look_at_resolver_can_resolve_target(resolver, "hero"),
               "try_resolve finds known target");
    expectTrue(!fuse::cinematics::look_at_resolver_can_resolve_target(resolver, "missing"),
               "try_resolve reports missing target");
}

void testFallbackCameraLookAt() {
    const fuse::cinematics::Vec3 position{10.f, 20.f, 30.f};

    fuse::cinematics::CameraKeyframe coincident{};
    coincident.position = position;
    coincident.look_at = position;
    const fuse::cinematics::Vec3 offset =
        fuse::cinematics::fallback_camera_look_at(coincident, position, 5.f);
    expectNear(offset.z, 25.f, 0.001f, "coincident look-at falls back along -Z");

    fuse::cinematics::CameraKeyframe distinct{};
    distinct.look_at = {11.f, 20.f, 30.f};
    const fuse::cinematics::Vec3 fixed =
        fuse::cinematics::fallback_camera_look_at(distinct, position, 5.f);
    expectNear(fixed.x, 11.f, 0.001f, "distinct look-at preserved");
}

void testResolveLookAtWorldWithFallback() {
    const fuse::cinematics::Vec3 position{0.f, 0.f, 10.f};
    fuse::cinematics::LookAtResolver resolver;
    resolver.set_try_resolve_fn([](const std::string& target_id, fuse::cinematics::Vec3& out) -> bool {
        if (target_id == "hero") {
            out = {0.f, 0.f, 20.f};
            return true;
        }
        return false;
    });

    fuse::cinematics::CameraKeyframe entity{};
    entity.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    entity.look_at_target_id = "hero";
    const fuse::cinematics::Vec3 resolved =
        fuse::cinematics::resolve_look_at_world_with_fallback(entity, resolver, position, 8.f);
    expectNear(resolved.z, 20.f, 0.001f, "entity resolve skips fallback when distinct");

    fuse::cinematics::CameraKeyframe unresolved{};
    unresolved.look_at_mode = fuse::cinematics::CameraLookAtMode::TargetEntity;
    unresolved.look_at_target_id = "missing";
    unresolved.look_at = position;
    const fuse::cinematics::Vec3 fallback =
        fuse::cinematics::resolve_look_at_world_with_fallback(unresolved, resolver, position, 8.f);
    expectNear(fallback.z, 2.f, 0.001f, "unresolved coincident look-at uses position fallback");
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

void testPlayheadScrub() {
    fuse::cinematics::Playhead playhead;
    playhead.set_duration_ms(10'000);
    playhead.set_state(fuse::cinematics::PlaybackState::Paused);

    playhead.scrub_to(2'500);
    expectTrue(playhead.time_ms() == 2'500, "scrub sets playhead time");
    expectTrue(playhead.is_paused(), "scrub preserves paused state");

    playhead.scrub_to(99'000);
    expectTrue(playhead.time_ms() == 10'000, "scrub clamps to duration");
}

void testTimelineScrubEnqueuesCues() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Director");
    fuse::cinematics::EventTrack& events = group.add_event_track("ScriptCues");
    events.set_script_hook_id("on_cutscene");
    events.add_event(fuse::cinematics::TimelineEvent("door_open", 1'000));
    events.add_event(fuse::cinematics::TimelineEvent("dialog_start", 3'000));
    events.sort_events();

    timeline.playhead().set_duration_ms(10'000);
    timeline.scrub_to(0, false);
    timeline.scrub_to(2'500);

    expectTrue(timeline.cue_queue().pending_count() == 1, "forward scrub enqueues crossed cue");
    expectTrue(timeline.cue_queue().pending()[0].label == "door_open", "scrub cue label");

    timeline.cue_queue().clear();
    timeline.scrub_to(5'000);
    expectTrue(timeline.cue_queue().pending_count() == 1, "second scrub enqueues next cue");
    expectTrue(timeline.cue_queue().pending()[0].label == "dialog_start", "second scrub cue label");

    timeline.cue_queue().clear();
    timeline.scrub_to(1'500);
    expectTrue(timeline.cue_queue().empty(), "backward scrub does not enqueue cues");
}

void testAdvanceEnqueuesCues() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Audio");
    fuse::cinematics::AudioTrack& audio = group.add_audio_track("Stinger");
    audio.set_sound_asset_id("sfx_intro");
    audio.add_event(fuse::cinematics::TimelineEvent("play_stinger", 500));
    audio.sort_events();

    timeline.playhead().set_duration_ms(5'000);
    timeline.play();
    timeline.advance(600);

    expectTrue(timeline.cue_queue().pending_count() == 1, "advance enqueues crossed cue");
    expectTrue(timeline.cue_queue().pending()[0].track_kind == fuse::cinematics::TrackKind::Audio,
               "advance cue carries track kind");
}

void testCueQueueDrain() {
    fuse::cinematics::CueQueue queue;
    fuse::u32 hook_count = 0;
    queue.set_dispatch_hook([&hook_count](const fuse::cinematics::CueEntry&) { ++hook_count; });

    fuse::cinematics::CueEntry cue_a;
    cue_a.cue_key = "key_a";
    cue_a.label = "a";
    cue_a.track_label = "TrackA";
    cue_a.group_label = "GroupA";
    cue_a.track_kind = fuse::cinematics::TrackKind::Event;
    cue_a.trigger_ms = 100;
    queue.enqueue(cue_a);

    fuse::cinematics::CueEntry cue_b;
    cue_b.cue_key = "key_b";
    cue_b.label = "b";
    cue_b.track_label = "TrackB";
    cue_b.group_label = "GroupA";
    cue_b.track_kind = fuse::cinematics::TrackKind::Generic;
    cue_b.trigger_ms = 200;
    queue.enqueue(cue_b);

    expectTrue(queue.pending_count() == 2, "queue holds pending cues");
    expectTrue(queue.total_enqueued() == 2, "queue counts enqueued cues");
    expectTrue(hook_count == 2, "dispatch hook fires per enqueue");

    const std::vector<fuse::cinematics::CueEntry> drained = queue.drain();
    expectTrue(drained.size() == 2, "drain moves all cues out");
    expectTrue(queue.empty(), "drain clears pending");
    expectTrue(drained[1].label == "b", "drain preserves order");
}

void testAudioTrackVolume() {
    fuse::cinematics::AudioTrack track("Ambience");
    track.set_sound_asset_id("wind_loop");
    track.add_keyframe({0, 0.f});
    track.add_keyframe({2'000, 1.f});
    track.sort_keyframes();

    expectTrue(track.kind() == fuse::cinematics::TrackKind::Audio, "audio track kind");
    expectNear(track.volume_at(1'000), 0.5f, 0.001f, "audio volume midpoint");
}

void testEventTrackKind() {
    fuse::cinematics::EventTrack track("DirectorCue");
    track.set_script_hook_id("cutscene_hook");
    track.add_event(fuse::cinematics::TimelineEvent("fade_in", 0, 500));

    expectTrue(track.kind() == fuse::cinematics::TrackKind::Event, "event track kind");
    expectTrue(track.script_hook_id() == "cutscene_hook", "event track hook id");
}

void testScrubFiresCuesInOrder() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Director");
    fuse::cinematics::EventTrack& events = group.add_event_track("ScriptCues");
    events.add_event(fuse::cinematics::TimelineEvent("alpha", 1'000));
    events.add_event(fuse::cinematics::TimelineEvent("beta", 2'000));
    events.add_event(fuse::cinematics::TimelineEvent("gamma", 3'000));
    events.sort_events();

    timeline.playhead().set_duration_ms(10'000);
    timeline.scrub_to(0, false);
    timeline.scrub_to(3'500);

    const auto& pending = timeline.cue_queue().pending();
    expectTrue(pending.size() == 3, "single forward scrub enqueues all crossed cues");
    expectTrue(pending[0].label == "alpha", "first crossed cue order");
    expectTrue(pending[1].label == "beta", "second crossed cue order");
    expectTrue(pending[2].label == "gamma", "third crossed cue order");
}

void testCueConsumeOnceSemantics() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Director");
    fuse::cinematics::EventTrack& events = group.add_event_track("ScriptCues");
    events.add_event(fuse::cinematics::TimelineEvent("door_open", 1'000));
    events.sort_events();

    timeline.playhead().set_duration_ms(10'000);
    timeline.scrub_to(0, false);
    timeline.scrub_to(2'000);
    expectTrue(timeline.cue_queue().pending_count() == 1, "first forward scrub fires cue");

    timeline.cue_queue().clear();
    timeline.scrub_to(0, false);
    timeline.scrub_to(2'000);
    expectTrue(timeline.cue_queue().empty(), "re-scrub does not re-fire consumed cue");
}

void testLoopResetClearsConsumedCues() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Audio");
    fuse::cinematics::AudioTrack& audio = group.add_audio_track("Stinger");
    audio.set_sound_asset_id("sfx_loop");
    audio.add_event(fuse::cinematics::TimelineEvent("play_stinger", 500));
    audio.sort_events();

    timeline.playhead().set_duration_ms(1'000);
    timeline.set_loop(true);
    timeline.play();
    timeline.advance(600);
    expectTrue(timeline.cue_queue().pending_count() == 1, "first loop iteration fires cue");

    timeline.cue_queue().drain();
    timeline.advance(500);
    expectTrue(timeline.playhead().time_ms() == 0, "loop rewinds playhead to start");

    timeline.advance(600);
    expectTrue(timeline.cue_queue().pending_count() == 1, "loop reset re-arms consumed cue");
}

void testEmptyTimelineProducesNoCues() {
    fuse::cinematics::Timeline timeline;
    timeline.playhead().set_duration_ms(5'000);

    timeline.scrub_to(0, false);
    timeline.scrub_to(4'000);
    expectTrue(timeline.cue_queue().empty(), "empty timeline scrub enqueues no cues");

    timeline.play();
    timeline.advance(1'000);
    expectTrue(timeline.cue_queue().empty(), "empty timeline advance enqueues no cues");
}

void testCuePayloadStubs() {
    fuse::cinematics::Timeline timeline;
    fuse::cinematics::TrackGroup& group = timeline.add_group("Scene");

    fuse::cinematics::EventTrack& events = group.add_event_track("Director");
    events.set_script_hook_id("on_cutscene");
    events.add_event(fuse::cinematics::TimelineEvent("fade_in", 100));
    events.sort_events();

    fuse::cinematics::AudioTrack& audio = group.add_audio_track("Ambience");
    audio.set_sound_asset_id("wind_loop");
    audio.add_event(fuse::cinematics::TimelineEvent("play_wind", 200));
    audio.sort_events();

    timeline.playhead().set_duration_ms(5'000);
    timeline.scrub_to(0, false);
    timeline.scrub_to(500);

    const auto& pending = timeline.cue_queue().pending();
    expectTrue(pending.size() == 2, "payload stub test collects both cues");

    expectTrue(pending[0].payload.kind == fuse::cinematics::CuePayloadKind::ScriptHook,
               "event track payload kind");
    expectTrue(pending[0].payload.hook_id == "on_cutscene", "event track hook id payload");

    expectTrue(pending[1].payload.kind == fuse::cinematics::CuePayloadKind::AudioClip,
               "audio track payload kind");
    expectTrue(pending[1].payload.asset_id == "wind_loop", "audio track asset id payload");
    expectTrue(!pending[0].cue_key.empty(), "cue key assigned for consume-once ledger");
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
    testFovLerpHelpers();
    testCameraTrackSampling();
    testCameraLookAtTargetEntity();
    testCameraTrackEmpty();
    testCameraTrackFovExtremes();
    testCameraLookDirection();
    testLookAtResolverFallback();
    testLookAtTryResolveNotFound();
    testCameraLookAtCoincidentEdge();
    testCameraKeyframeSampleHelpers();
    testCameraFovHoldExtrapolation();
    testCameraSampleHelpersEmpty();
    testDefaultCameraSample();
    testCameraKeyframeEntityLookAtStub();
    testCameraTrackClearKeyframes();
    testCameraDuplicateKeyframeTime();
    testCameraTrackMultiKeyframe();
    testNormalizeCameraKeyframe();
    testCameraTrackAddKeyframeFovGuard();
    testCameraTrackNeedsLookAtResolver();
    testSampleCameraKeyframe();
    testEntityLookAtWithoutResolver();
    testDefaultCameraLookAtForPosition();
    testCameraBracketSmoothStep();
    testCameraKeyframeFovUnset();
    testApplyCameraKeyframeDefaults();
    testMakeDefaultCameraKeyframe();
    testCameraTrackCoversTime();
    testCameraKeyframeLookAtUnset();
    testResolveLookAtWorldOrDefault();
    testLookAtResolverResolveOr();
    testDefaultCameraFovDeg();
    testDefaultCameraSampleAt();
    testNormalizeCameraSample();
    testCameraTrackIsEmpty();
    testCameraKeyframeEntityTargetMissing();
    testEntityLookAtMissingTargetIdFallback();
    testLookAtResolverHasTarget();
    testCameraFovRangeGuards();
    testEffectiveCameraFovNonFinite();
    testNormalizeCameraKeyframesBatch();
    testCameraSampleIsDefault();
    testResetCameraKeyframeToDefaults();
    testSampleCameraPose();
    testResolveLookAtOrFallback();
    testCollectCameraLookAtTargetIds();
    testLookAtResolverAvailability();
    testLookAtResolverCanResolveTarget();
    testFallbackCameraLookAt();
    testResolveLookAtWorldWithFallback();
    testSpriteTrackSampling();
    testPropertyTrackSampling();
    testTimelineContentSpan();
    testPlayheadScrub();
    testTimelineScrubEnqueuesCues();
    testAdvanceEnqueuesCues();
    testCueQueueDrain();
    testAudioTrackVolume();
    testEventTrackKind();
    testScrubFiresCuesInOrder();
    testCueConsumeOnceSemantics();
    testLoopResetClearsConsumedCues();
    testEmptyTimelineProducesNoCues();
    testCuePayloadStubs();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cinematics_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cinematics_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
