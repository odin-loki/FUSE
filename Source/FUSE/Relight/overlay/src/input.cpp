// FUSE Relight RL-6.1: the overlay's input rules and queue (see input.hpp).
#include <fuse/relight/overlay/input.hpp>

namespace fuse::relight::overlay {

namespace {
std::int32_t lowWord(std::int64_t l) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(l & 0xffff)); }
std::int32_t highWord(std::int64_t l) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>((l >> 16) & 0xffff));
}
bool isToggleKey(std::uint64_t wParam) { return (wParam & 0xffffu) == win32::kVkX; }
} // namespace

Translation translateMessage(std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam, bool visible,
                             const CoordMap& map) {
    Translation t;
    const bool alt = (lParam & win32::kAltContextBit) != 0;
    // The hotkey: Alt+X, in both states.
    if (msg == win32::kWmSysKeyDown && alt && isToggleKey(wParam)) {
        t.consume = true;
        if ((lParam & win32::kRepeatBit) == 0) {
            t.event.type = EventType::Toggle;
        }
        return t;
    }
    if ((msg == win32::kWmSysKeyUp && isToggleKey(wParam)) ||
        (msg == win32::kWmSysChar && alt && ((wParam & 0xffffu) == 'x' || (wParam & 0xffffu) == 'X'))) {
        t.consume = true;
        return t;
    }
    if (!visible) {
        return t; // forwarded unchanged
    }
    const bool keyboard = msg >= win32::kWmKeyDown && msg <= win32::kWmKeyLast;
    const bool mouse = msg >= win32::kWmMouseMove && msg <= win32::kWmMouseLast;
    if (!keyboard && !mouse && msg != win32::kWmInput) {
        if (msg == win32::kWmKillFocus) {
            t.event.type = EventType::MouseUp; // no button stays pressed across a focus loss
            t.event.x = t.event.y = -1;
        }
        return t;
    }
    t.consume = true;
    switch (msg) {
    case win32::kWmMouseMove:
    case win32::kWmLButtonDown:
    case win32::kWmLButtonDblClk:
    case win32::kWmLButtonUp:
        t.event.type = msg == win32::kWmMouseMove ? EventType::MouseMove
                       : msg == win32::kWmLButtonUp ? EventType::MouseUp
                                                     : EventType::MouseDown;
        t.event.x = map.x(lowWord(lParam));
        t.event.y = map.y(highWord(lParam));
        break;
    case win32::kWmMouseWheel: {
        // WHEEL_DELTA = 120 per notch, in the high word of wParam.
        const std::int32_t delta = static_cast<std::int16_t>(static_cast<std::uint16_t>((wParam >> 16) & 0xffff));
        t.event.type = EventType::Wheel;
        t.event.value = delta >= 0 ? (delta + 119) / 120 : -((-delta + 119) / 120);
        break;
    }
    case win32::kWmKeyDown:
    case win32::kWmSysKeyDown:
        t.event.type = EventType::KeyDown;
        t.event.value = static_cast<std::int32_t>(wParam & 0xffffu);
        break;
    case win32::kWmChar:
        t.event.type = EventType::Char;
        t.event.value = static_cast<std::int32_t>(wParam & 0xffffu);
        break;
    default:
        break;
    }
    return t;
}

bool InputHookCore::handle(std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam) {
    const Translation t = translateMessage(msg, wParam, lParam, visible(), coordMap());
    if (t.event.type != EventType::None) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (t.event.type == EventType::Toggle) {
            m_toggles.fetch_add(1, std::memory_order_relaxed);
        }
        if (m_count < kQueueCapacity) {
            m_queue[(m_head + m_count) % kQueueCapacity] = t.event;
            ++m_count;
        } else {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    (t.consume ? m_consumed : m_forwarded).fetch_add(1, std::memory_order_relaxed);
    return t.consume;
}

void InputHookCore::setCoordMap(const CoordMap& map) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_map = map;
}

CoordMap InputHookCore::coordMap() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_map;
}

std::size_t InputHookCore::drain(InputEvent* out, std::size_t capacity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t n = 0;
    while (m_count > 0 && n < capacity) {
        out[n++] = m_queue[m_head];
        m_head = (m_head + 1) % kQueueCapacity;
        --m_count;
    }
    return n;
}

} // namespace fuse::relight::overlay
