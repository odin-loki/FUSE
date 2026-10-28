#pragma once

#include <fuse/platform/input.hpp>
#include <fuse/types.hpp>

#include <optional>
#include <vector>

namespace fuse::platform {

/// One recorded key edge. `down` is true for a press and false for a release.
struct InputEvent {
    u32 timeMs = 0;
    Key key = Key::A;
    bool down = false;
};

/// Deterministic key replay log. Accepted events stay in the order they were recorded.
///
/// `record` appends when `event.timeMs` is greater than or equal to the previous event's
/// time. A strictly earlier timestamp is rejected and returns false.
/// `seek` moves the cursor to the first stored event with `timeMs >= time`.
/// `next` returns the event at the cursor and advances, or `std::nullopt` at the end.
/// `reset` seeks to time 0. `size` is the number of stored events.
class InputReplay {
public:
    bool record(const InputEvent& event);
    void seek(u32 timeMs);
    std::optional<InputEvent> next();
    void reset();
    u32 size() const;

private:
    std::vector<InputEvent> m_events;
    u32 m_cursor = 0;
};

inline bool InputReplay::record(const InputEvent& event) {
    if (!m_events.empty() && event.timeMs < m_events.back().timeMs) {
        return false;
    }
    m_events.push_back(event);
    return true;
}

inline void InputReplay::seek(u32 timeMs) {
    u32 index = 0;
    const u32 count = static_cast<u32>(m_events.size());
    while (index < count && m_events[index].timeMs < timeMs) {
        ++index;
    }
    m_cursor = index;
}

inline std::optional<InputEvent> InputReplay::next() {
    if (static_cast<usize>(m_cursor) >= m_events.size()) {
        return std::nullopt;
    }
    return m_events[m_cursor++];
}

inline void InputReplay::reset() {
    seek(0);
}

inline u32 InputReplay::size() const {
    return static_cast<u32>(m_events.size());
}

} // namespace fuse::platform
