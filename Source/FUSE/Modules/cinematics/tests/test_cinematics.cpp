#include <fuse/cinematics/audio_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
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

    queue.enqueue({"a", "TrackA", "GroupA", fuse::cinematics::TrackKind::Event, 100});
    queue.enqueue({"b", "TrackB", "GroupA", fuse::cinematics::TrackKind::Generic, 200});

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
    testCameraTrackMultiKeyframe();
    testSpriteTrackSampling();
    testPropertyTrackSampling();
    testTimelineContentSpan();
    testPlayheadScrub();
    testTimelineScrubEnqueuesCues();
    testAdvanceEnqueuesCues();
    testCueQueueDrain();
    testAudioTrackVolume();
    testEventTrackKind();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cinematics_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cinematics_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
