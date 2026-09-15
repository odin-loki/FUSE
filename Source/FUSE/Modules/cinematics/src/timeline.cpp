#include <fuse/cinematics/timeline.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::cinematics {

Timeline::Timeline() = default;

TrackGroup& Timeline::add_group(const std::string& label) {
    groups_.emplace_back(label);
    return groups_.back();
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

    playhead_.set_time_ms(current + effective_delta);
    playhead_.clamp_time();

    if ((playhead_.is_playing_forward() && playhead_.time_ms() >= duration)
        || (!playhead_.is_playing_forward() && playhead_.time_ms() <= 0)) {
        handle_sequence_end();
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
        for (Track& track : group.tracks()) {
            track.sort_events();
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
