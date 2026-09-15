#include <fuse/cinematics/timeline.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::cinematics {

Timeline::Timeline() = default;

TrackGroup& Timeline::add_group(const std::string& label) {
    groups_.emplace_back(label);
    return groups_.back();
}

TrackSpan Timeline::content_span() const {
    TrackSpan span;
    bool found = false;
    for (const TrackGroup& group : groups_) {
        const TrackSpan group_span = group.span();
        if (group_span.empty()) {
            continue;
        }

        if (!found) {
            span = group_span;
            found = true;
        } else {
            span.start_ms = std::min(span.start_ms, group_span.start_ms);
            span.end_ms = std::max(span.end_ms, group_span.end_ms);
        }
    }
    return span;
}

TimelineMs Timeline::suggested_duration_ms() const {
    const TrackSpan span = content_span();
    if (!span.empty()) {
        return span.end_ms;
    }
    return playhead_.duration_ms();
}

void Timeline::reset(TimelineMs time_ms) {
    playhead_.set_time_ms(time_ms);
    playhead_.clamp_time();
    playhead_.set_state(PlaybackState::Stopped);
    post_event(ControllerEvent::Reset);
}

void Timeline::play(TimelineMs time_ms) {
    if (time_ms >= 0) {
        playhead_.set_time_ms(time_ms);
        playhead_.clamp_time();
    }
    if (playhead_.time_scale() == 0.f) {
        playhead_.set_time_scale(1.f);
    }
    playhead_.set_state(PlaybackState::Playing);
    post_event(ControllerEvent::Play);
}

void Timeline::pause() {
    playhead_.set_state(PlaybackState::Paused);
    post_event(ControllerEvent::Pause);
}

void Timeline::stop(bool reset_playhead) {
    playhead_.set_state(PlaybackState::Stopped);
    if (reset_playhead) {
        playhead_.set_time_ms(0);
    }
    post_event(ControllerEvent::Stop);
}

void Timeline::advance(TimelineMs delta_ms) {
    if (!playhead_.is_playing()) {
        return;
    }

    if (playhead_.time_scale() == 0.f) {
        pause();
        return;
    }

    TimelineMs scaled_delta = delta_ms;
    if (playhead_.time_scale() < 0.f) {
        scaled_delta = -scaled_delta;
    } else if (playhead_.time_scale() != 1.f) {
        scaled_delta = static_cast<TimelineMs>(
            std::lround(static_cast<float>(scaled_delta) * std::fabs(playhead_.time_scale())));
    }

    if (scaled_delta == 0) {
        return;
    }

    const TimelineMs current = playhead_.time_ms();
    const TimelineMs duration = playhead_.duration_ms();
    TimelineMs effective_delta = scaled_delta;

    if (playhead_.is_playing_forward()) {
        if (current + effective_delta > duration) {
            effective_delta = duration - current;
        }
    } else {
        if (current + effective_delta < 0) {
            effective_delta = -current;
        }
    }

    if (effective_delta == 0) {
        handle_sequence_end();
        return;
    }

    if (update_callback_) {
        update_callback_(current, effective_delta);
    }

    if (playhead_.is_playing_forward() && effective_delta > 0) {
        collect_cues_forward(current, current + effective_delta);
    }

    playhead_.set_time_ms(current + effective_delta);
    playhead_.clamp_time();

    if ((playhead_.is_playing_forward() && playhead_.time_ms() >= duration)
        || (!playhead_.is_playing_forward() && playhead_.time_ms() <= 0)) {
        handle_sequence_end();
    }
}

void Timeline::scrub_to(TimelineMs time_ms, bool enqueue_cues) {
    const TimelineMs previous = playhead_.time_ms();
    playhead_.scrub_to(time_ms);

    if (enqueue_cues && playhead_.time_ms() > previous) {
        collect_cues_forward(previous, playhead_.time_ms());
    }
}

void Timeline::collect_cues_forward(TimelineMs from_ms, TimelineMs to_ms) {
    if (to_ms <= from_ms) {
        return;
    }

    for (const TrackGroup& group : groups_) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (!track || !track->enabled()) {
                continue;
            }

            for (const TimelineEvent& event : track->events()) {
                if (!event.enabled()) {
                    continue;
                }

                const TimelineMs trigger = event.trigger_ms();
                if (trigger > from_ms && trigger <= to_ms) {
                    cue_queue_.enqueue(CueEntry{event.label(),
                                                track->label(),
                                                group.label(),
                                                track->kind(),
                                                trigger});
                }
            }
        }
    }
}

void Timeline::on_update(TimelineUpdateCallback callback) {
    update_callback_ = std::move(callback);
}

void Timeline::on_controller_event(TimelineEventCallback callback) {
    event_callback_ = std::move(callback);
}

void Timeline::sort_tracks() {
    for (TrackGroup& group : groups_) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (track) {
                track->sort_events();
            }
        }
    }
}

void Timeline::post_event(ControllerEvent event) {
    if (event_callback_) {
        event_callback_(event);
    }
}

void Timeline::handle_sequence_end() {
    if (loop_) {
        post_event(ControllerEvent::Loop);
        playhead_.set_time_ms(playhead_.is_playing_forward() ? 0 : playhead_.duration_ms());
        return;
    }

    playhead_.set_state(PlaybackState::Stopped);
    post_event(ControllerEvent::Stop);
}

} // namespace fuse::cinematics
