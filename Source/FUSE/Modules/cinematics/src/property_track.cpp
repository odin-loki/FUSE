#include <fuse/cinematics/property_track.hpp>

#include <algorithm>

namespace fuse::cinematics {

PropertyTrack::PropertyTrack(const std::string& label) : Track(label) {}

void PropertyTrack::add_keyframe(const PropertyKeyframe& keyframe) {
    keyframes_.push_back(keyframe);
}

void PropertyTrack::sort_keyframes() {
    std::stable_sort(keyframes_.begin(), keyframes_.end(),
                     [](const PropertyKeyframe& a, const PropertyKeyframe& b) {
                         return a.time_ms < b.time_ms;
                     });
}

const char* key_interp_name(KeyInterp interp) {
    switch (interp) {
    case KeyInterp::Constant:
        return "constant";
    case KeyInterp::Cubic:
        return "cubic";
    case KeyInterp::Linear:
        break;
    }
    return "linear";
}

const char* tangent_mode_name(TangentMode mode) {
    switch (mode) {
    case TangentMode::Flat:
        return "flat";
    case TangentMode::User:
        return "user";
    case TangentMode::Auto:
        break;
    }
    return "auto";
}

bool parse_key_interp(const std::string& name, KeyInterp& out) {
    if (name == "linear") {
        out = KeyInterp::Linear;
    } else if (name == "constant") {
        out = KeyInterp::Constant;
    } else if (name == "cubic") {
        out = KeyInterp::Cubic;
    } else {
        return false;
    }
    return true;
}

bool parse_tangent_mode(const std::string& name, TangentMode& out) {
    if (name == "auto") {
        out = TangentMode::Auto;
    } else if (name == "flat") {
        out = TangentMode::Flat;
    } else if (name == "user") {
        out = TangentMode::User;
    } else {
        return false;
    }
    return true;
}

void PropertyTrack::effective_tangents(size_t index, float& in_slope, float& out_slope) const {
    in_slope = 0.f;
    out_slope = 0.f;
    if (index >= keyframes_.size()) {
        return;
    }
    const PropertyKeyframe& k = keyframes_[index];
    if (k.tangent_mode == TangentMode::User) {
        in_slope = k.in_tangent;
        out_slope = k.out_tangent;
        return;
    }
    if (k.tangent_mode == TangentMode::Flat || keyframes_.size() < 2) {
        return;
    }
    const PropertyKeyframe* prev = index > 0 ? &keyframes_[index - 1] : nullptr;
    const PropertyKeyframe* next = index + 1 < keyframes_.size() ? &keyframes_[index + 1] : nullptr;
    float slope = 0.f;
    if (prev != nullptr && next != nullptr) {
        // Local extremum (or plateau): flat, so the curve never overshoots its keys.
        if ((k.value - prev->value) * (next->value - k.value) <= 0.f) {
            return;
        }
        const float dt = static_cast<float>(next->time_ms - prev->time_ms) / 1000.f;
        slope = dt > 0.f ? (next->value - prev->value) / dt : 0.f;
    } else {
        const PropertyKeyframe* a = prev != nullptr ? prev : &k;
        const PropertyKeyframe* b = next != nullptr ? next : &k;
        const float dt = static_cast<float>(b->time_ms - a->time_ms) / 1000.f;
        slope = dt > 0.f ? (b->value - a->value) / dt : 0.f;
    }
    in_slope = slope;
    out_slope = slope;
}

void PropertyTrack::update_tangents() {
    for (size_t i = 0; i < keyframes_.size(); ++i) {
        if (keyframes_[i].tangent_mode != TangentMode::User) {
            effective_tangents(i, keyframes_[i].in_tangent, keyframes_[i].out_tangent);
        }
    }
}

float PropertyTrack::sample_at(TimelineMs time_ms, EaseMode ease) const {
    const PropertyKeyframe* previous = nullptr;
    for (size_t i = 0; i < keyframes_.size(); ++i) {
        const PropertyKeyframe& keyframe = keyframes_[i];
        if (time_ms < keyframe.time_ms) {
            if (!previous) {
                return keyframe.value;
            }

            const TimelineMs span = keyframe.time_ms - previous->time_ms;
            if (span <= 0) {
                return keyframe.value;
            }
            const float u = static_cast<float>(time_ms - previous->time_ms) / static_cast<float>(span);
            switch (previous->interp) {
            case KeyInterp::Constant:
                return previous->value;
            case KeyInterp::Cubic: {
                float inPrev = 0.f;
                float outPrev = 0.f;
                float inNext = 0.f;
                float outNext = 0.f;
                effective_tangents(i - 1, inPrev, outPrev);
                effective_tangents(i, inNext, outNext);
                const float dt = static_cast<float>(span) / 1000.f;
                const float u2 = u * u;
                const float u3 = u2 * u;
                const float h00 = 2.f * u3 - 3.f * u2 + 1.f;
                const float h10 = u3 - 2.f * u2 + u;
                const float h01 = -2.f * u3 + 3.f * u2;
                const float h11 = u3 - u2;
                return h00 * previous->value + h10 * dt * outPrev + h01 * keyframe.value + h11 * dt * inNext;
            }
            case KeyInterp::Linear:
                break;
            }
            return lerp(previous->value, keyframe.value, apply_ease(ease, u));
        }

        previous = &keyframe;
    }

    return previous ? previous->value : 0.f;
}

} // namespace fuse::cinematics
