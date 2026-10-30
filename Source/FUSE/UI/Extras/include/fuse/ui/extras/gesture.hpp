// Gesture recognition (Qt QGesture parity: tap, double tap, tap-and-hold, pan, pinch incl. rotation, swipe) from raw
// touch / pen / mouse points. Backends (Android, iOS, Wayland, Win32 touch) feed points; widgets consume gestures.
#pragma once

#include "fuse/ui/types.hpp"

#include <cstdint>
#include <vector>

namespace fuse::ui {

enum class TouchPhase : std::uint8_t { Down, Move, Up, Cancel };
struct TouchPoint {
    std::uint32_t id = 0;
    Vec2 pos;
    TouchPhase phase = TouchPhase::Down;
};

enum class GestureType : std::uint8_t { Tap, DoubleTap, LongPress, Pan, Pinch, Swipe };
enum class GestureState : std::uint8_t { Started, Updated, Finished, Canceled };
enum class SwipeDirection : std::uint8_t { None, Left, Right, Up, Down };

struct Gesture {
    GestureType type = GestureType::Tap;
    GestureState state = GestureState::Finished;
    Vec2 pos;            ///< tap point / pan point / pinch centre
    Vec2 delta;          ///< pan: movement since the last update
    Vec2 offset;         ///< pan: total movement since start
    Vec2 velocity;       ///< px/s (pan end, swipe)
    f32 scale = 1.f;     ///< pinch: total scale since start
    f32 rotation = 0.f;  ///< pinch: total rotation since start (radians, clockwise in y-down space)
    SwipeDirection direction = SwipeDirection::None;
    int fingers = 1;
};

struct GestureConfig {
    f32 tapSlop = 10.f;          ///< px a tap may move
    double tapMaxSeconds = 0.35;
    double doubleTapSeconds = 0.3;
    f32 doubleTapSlop = 24.f;
    double longPressSeconds = 0.5;
    f32 swipeMinVelocity = 600.f; ///< px/s at release
    f32 swipeMinDistance = 60.f;
};

class GestureRecognizer {
public:
    explicit GestureRecognizer(GestureConfig cfg = {}) : m_cfg(cfg) {}
    /// Feeds one point at `time` seconds; returns gestures recognised by it.
    std::vector<Gesture> feed(const TouchPoint& p, double time);
    /// Advances time without input (long press fires from here).
    std::vector<Gesture> tick(double time);
    void reset();
    const GestureConfig& config() const { return m_cfg; }

private:
    struct Pt { std::uint32_t id; Vec2 start, pos, prev; double t0; };
    Pt* find(std::uint32_t id);
    Vec2 centroid() const;
    GestureConfig m_cfg;
    std::vector<Pt> m_pts;
    enum class Mode : std::uint8_t { Idle, Pending, Pan, Pinch, Held } m_mode = Mode::Idle;
    double m_downTime = 0, m_lastTapTime = -1e9, m_lastMoveTime = 0;
    Vec2 m_lastTapPos, m_panStart, m_panLast, m_velocity;
    f32 m_pinchDist0 = 0, m_pinchAngle0 = 0;
    int m_maxFingers = 0;
};

} // namespace fuse::ui
