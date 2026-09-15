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

} // namespace

int main() {
    fuse::core::initialize();
    testThirtySecondTimelineAdvance();
    testTimelineLoopRewinds();
    testPauseStopsAdvance();
    testTrackSpanAndInterpolation();
    testNextEventIndex();
    testEventTriggerDetection();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cinematics_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cinematics_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
