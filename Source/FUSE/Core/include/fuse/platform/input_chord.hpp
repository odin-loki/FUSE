#pragma once

#include <fuse/platform/input.hpp>
#include <fuse/types.hpp>

#include <initializer_list>
#include <span>
#include <vector>

namespace fuse::platform {

/// Ordered key chord. `feed` advances when the next key arrives within `maxGapMs` of the previous
/// accepted key (a gap equal to `maxGapMs` still advances). The first key starts the sequence.
/// An empty sequence never matches.
///
/// Reset: a wrong key drops progress and is discarded — it does not open a new sequence, even when
/// it equals the first key. A gap greater than `maxGapMs` drops progress, then that same key starts
/// a new sequence when it equals the first key. `matched()` is true only on the `feed` that
/// completes the sequence; the next `feed` clears it and starts fresh.
///
/// `timeMs` is a millisecond timestamp. The gap is the unsigned difference from the previous
/// accepted key, so a wrapping clock looks like a short gap.
class ChordMatcher {
public:
    /// Copies `keys`. `maxGapMs` is the maximum gap between consecutive accepted keys.
    ChordMatcher(std::span<const Key> keys, u32 maxGapMs)
        : m_keys(keys.begin(), keys.end()), m_maxGapMs(maxGapMs) {}

    ChordMatcher(std::initializer_list<Key> keys, u32 maxGapMs)
        : ChordMatcher(std::span<const Key>(keys.begin(), keys.size()), maxGapMs) {}

    /// Advance the sequence. See the class comment for the gap and reset rules.
    void feed(Key key, u32 timeMs);

    /// True only after the `feed` that completed the sequence, until the next `feed`.
    [[nodiscard]] bool matched() const { return m_matched; }

private:
    /// If `key` is the first key, begin there. A one-key sequence completes immediately.
    void acceptFirst(Key key, u32 timeMs);

    std::vector<Key> m_keys;
    u32 m_maxGapMs = 0;
    /// Keys accepted in the current attempt. 0 means waiting for the first key.
    usize m_accepted = 0;
    u32 m_lastTimeMs = 0;
    bool m_matched = false;
};

inline void ChordMatcher::acceptFirst(Key key, u32 timeMs) {
    if (m_keys.empty() || key != m_keys[0]) {
        return;
    }
    m_lastTimeMs = timeMs;
    if (m_keys.size() == 1) {
        m_matched = true;
        m_accepted = 0;
        return;
    }
    m_accepted = 1;
}

inline void ChordMatcher::feed(Key key, u32 timeMs) {
    if (m_keys.empty()) {
        m_matched = false;
        return;
    }

    // The previous feed completed the chord. This one starts over, then is matched normally.
    if (m_matched) {
        m_matched = false;
        m_accepted = 0;
    }

    if (m_accepted == 0) {
        acceptFirst(key, timeMs);
        return;
    }

    const u32 gap = timeMs - m_lastTimeMs;
    if (gap > m_maxGapMs) {
        m_accepted = 0;
        acceptFirst(key, timeMs);
        return;
    }

    if (key != m_keys[m_accepted]) {
        m_accepted = 0;
        return;
    }

    m_lastTimeMs = timeMs;
    ++m_accepted;
    if (m_accepted == m_keys.size()) {
        m_matched = true;
        m_accepted = 0;
    }
}

} // namespace fuse::platform
