#include <fuse/cinematics/cue_preview.hpp>

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/audio_track.hpp>
#include <fuse/cinematics/event_track.hpp>

namespace fuse::cinematics {

namespace {

CuePayload payload_for_event(const Track& track, const TimelineEvent& event) {
    CuePayload payload;
    if (track.kind() == TrackKind::Event) {
        const auto& eventTrack = static_cast<const EventTrack&>(track);
        payload.kind = CuePayloadKind::ScriptHook;
        payload.hook_id = eventTrack.script_hook_id();
        return payload;
    }
    if (track.kind() == TrackKind::Audio) {
        const auto& audioTrack = static_cast<const AudioTrack&>(track);
        payload.kind = CuePayloadKind::AudioClip;
        payload.asset_id = audioTrack.sound_asset_id();
        return payload;
    }
    payload.kind = CuePayloadKind::Custom;
    payload.custom_key = event.label();
    return payload;
}

} // namespace

std::vector<CuePreviewEntry> preview_cues_at(const Timeline& timeline, TimelineMs time_ms) {
    std::vector<CuePreviewEntry> previews;
    if (time_ms <= 0) {
        return previews;
    }

    for (const TrackGroup& group : timeline.groups()) {
        for (const std::unique_ptr<Track>& trackPtr : group.tracks()) {
            if (trackPtr == nullptr || !trackPtr->enabled()) {
                continue;
            }
            const Track& track = *trackPtr;

            if (track.kind() == TrackKind::Actor) {
                const auto& actorTrack = static_cast<const ActorTrack&>(track);
                for (const ActorEvent& event : actorTrack.actor_events()) {
                    if (event.time_ms > time_ms) {
                        continue;
                    }
                    CuePreviewEntry entry;
                    entry.label = event.kind == ActorEventKind::Mount ? "actor_mount" : "actor_unmount";
                    entry.track_label = track.label();
                    entry.group_label = group.label();
                    entry.track_kind = TrackKind::Actor;
                    entry.trigger_ms = event.time_ms;
                    entry.payload.kind = CuePayloadKind::Custom;
                    entry.payload.custom_key = event.mount_point;
                    previews.push_back(std::move(entry));
                }
                continue;
            }

            for (const TimelineEvent& event : track.events()) {
                if (event.trigger_ms() > time_ms) {
                    continue;
                }
                if (!event.should_trigger_forward(0, time_ms)) {
                    continue;
                }

                CuePreviewEntry entry;
                entry.label = event.label();
                entry.track_label = track.label();
                entry.group_label = group.label();
                entry.track_kind = track.kind();
                entry.trigger_ms = event.trigger_ms();
                entry.payload = payload_for_event(track, event);
                previews.push_back(std::move(entry));
            }
        }
    }

    return previews;
}

} // namespace fuse::cinematics
