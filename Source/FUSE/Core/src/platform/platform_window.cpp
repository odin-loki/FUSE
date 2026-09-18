#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/window.hpp>

#include <utility>

namespace fuse::platform {

namespace {

const char* sanitizeTitle(const char* title) {
    return (title != nullptr && title[0] != '\0') ? title : "FUSE";
}

} // namespace

Window::Window() {
    m_valid = true;
    m_width = 1920u;
    m_height = 1080u;
    m_title = "FUSE";
}

Window::Window(const WindowDesc& desc) {
    m_valid = true;
    m_width = desc.width > 0 ? desc.width : 1920u;
    m_height = desc.height > 0 ? desc.height : 1080u;
    m_fullscreen = desc.fullscreen;
    m_borderless = desc.borderless;
    m_vsync = desc.vsync;
    m_title = sanitizeTitle(desc.title);
}

Window::~Window() {
    m_valid = false;
}

Window::Window(Window&& other) noexcept
    : m_valid(other.m_valid),
      m_width(other.m_width),
      m_height(other.m_height),
      m_fullscreen(other.m_fullscreen),
      m_borderless(other.m_borderless),
      m_vsync(other.m_vsync),
      m_focused(other.m_focused),
      m_closeRequest(other.m_closeRequest),
      m_title(std::move(other.m_title)) {
    other.m_valid = false;
    other.m_closeRequest = WindowCloseRequest::None;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        m_valid = other.m_valid;
        m_width = other.m_width;
        m_height = other.m_height;
        m_fullscreen = other.m_fullscreen;
        m_borderless = other.m_borderless;
        m_vsync = other.m_vsync;
        m_focused = other.m_focused;
        m_closeRequest = other.m_closeRequest;
        m_title = std::move(other.m_title);
        other.m_valid = false;
        other.m_closeRequest = WindowCloseRequest::None;
    }
    return *this;
}

WindowDesc Window::description() const {
    WindowDesc desc;
    desc.title = m_title.c_str();
    desc.width = m_width;
    desc.height = m_height;
    desc.fullscreen = m_fullscreen;
    desc.borderless = m_borderless;
    desc.vsync = m_vsync;
    return desc;
}

NativeWindowHandle Window::nativeHandle() const {
    // Stub — real HWND / X11 Window / ANativeWindow* lands in platform backends.
    return {};
}

void* Window::nativeVulkanSurface() const {
    return nullptr;
}

VulkanSurfaceWire Window::vulkanSurfaceWire() const {
    VulkanSurfaceWire wire;
    wire.nativeSurface = nativeVulkanSurface();
    wire.presentable = wire.nativeSurface != nullptr;
    return wire;
}

void Window::setTitle(const char* title) {
    m_title = sanitizeTitle(title);
}

void Window::resize(u32 width, u32 height, EventPump* pump) {
    const u32 previousWidth = m_width;
    const u32 previousHeight = m_height;

    if (width > 0) {
        m_width = width;
    }
    if (height > 0) {
        m_height = height;
    }

    if (pump != nullptr && (m_width != previousWidth || m_height != previousHeight)) {
        pump->pushWindowResized(*this);
    }
}

void Window::setFullscreen(bool fullscreen) {
    m_fullscreen = fullscreen;
}

void Window::setFocused(bool focused, EventPump* pump) {
    if (m_focused == focused) {
        return;
    }

    m_focused = focused;
    if (pump == nullptr) {
        return;
    }

    if (focused) {
        pump->pushWindowFocusGained(*this);
    } else {
        pump->pushWindowFocusLost(*this);
    }
}

void Window::requestClose(EventPump* pump) {
    const bool alreadyRequested = m_closeRequest == WindowCloseRequest::Requested;
    m_closeRequest = WindowCloseRequest::Requested;
    if (pump != nullptr && !alreadyRequested) {
        pump->pushWindowCloseRequested(*this);
    }
}

void Window::clearCloseRequest() {
    m_closeRequest = WindowCloseRequest::None;
}

EventPump::EventPump() = default;
EventPump::~EventPump() = default;

bool EventPump::pollEvent(PlatformEvent& outEvent) {
    if (m_syntheticHead == m_syntheticTail) {
        outEvent = {};
        return false;
    }

    outEvent = m_syntheticEvents[m_syntheticHead];
    m_syntheticHead = (m_syntheticHead + 1) % kMaxSyntheticEvents;

    if (outEvent.type == PlatformEventType::Quit) {
        m_quitRequested = true;
    }

    return true;
}

bool EventPump::peekEvent(PlatformEvent& outEvent) const {
    if (m_syntheticHead == m_syntheticTail) {
        outEvent = {};
        return false;
    }

    outEvent = m_syntheticEvents[m_syntheticHead];
    return true;
}

bool EventPump::peekEventType(PlatformEventType& outType) const {
    if (m_syntheticHead == m_syntheticTail) {
        outType = PlatformEventType::None;
        return false;
    }

    outType = m_syntheticEvents[m_syntheticHead].type;
    return true;
}

bool EventPump::tryPeekEventOfType(PlatformEventType type, PlatformEvent& outEvent) const {
    if (m_syntheticHead == m_syntheticTail) {
        outEvent = {};
        return false;
    }

    const PlatformEvent& front = m_syntheticEvents[m_syntheticHead];
    if (front.type != type) {
        outEvent = {};
        return false;
    }

    outEvent = front;
    return true;
}

bool EventPump::frontEventTypeIs(PlatformEventType type) const {
    PlatformEventType front = PlatformEventType::None;
    if (!peekEventType(front)) {
        return false;
    }

    return front == type;
}

bool EventPump::hasPendingEvents() const {
    return m_syntheticHead != m_syntheticTail;
}

u32 EventPump::pendingEventCount() const {
    if (m_syntheticHead == m_syntheticTail) {
        return 0;
    }

    if (m_syntheticTail > m_syntheticHead) {
        return m_syntheticTail - m_syntheticHead;
    }

    return kMaxSyntheticEvents - m_syntheticHead + m_syntheticTail;
}

bool EventPump::hasPendingResizeFor(const Window& window) const {
    return pendingResizeExtentFor(window).pending;
}

bool EventPump::hasPendingEventOfType(PlatformEventType type) const {
    return countPendingEventsOfType(type) > 0u;
}

u32 EventPump::countPendingEventsFor(const Window& window) const {
    if (m_syntheticHead == m_syntheticTail) {
        return 0;
    }

    u32 count = 0;
    u32 index = m_syntheticHead;
    while (index != m_syntheticTail) {
        if (m_syntheticEvents[index].window == &window) {
            ++count;
        }

        index = (index + 1u) % kMaxSyntheticEvents;
    }

    return count;
}

u32 EventPump::countPendingEventsOfType(PlatformEventType type) const {
    if (m_syntheticHead == m_syntheticTail) {
        return 0;
    }

    u32 count = 0;
    u32 index = m_syntheticHead;
    while (index != m_syntheticTail) {
        if (m_syntheticEvents[index].type == type) {
            ++count;
        }

        index = (index + 1u) % kMaxSyntheticEvents;
    }

    return count;
}

bool EventPump::wouldCoalesceResize(const PlatformEvent& event) const {
    if (event.type != PlatformEventType::WindowResized || event.window == nullptr ||
        event.width == 0u || event.height == 0u || m_syntheticHead == m_syntheticTail) {
        return false;
    }

    u32 index = (m_syntheticTail + kMaxSyntheticEvents - 1u) % kMaxSyntheticEvents;
    while (true) {
        const PlatformEvent& pending = m_syntheticEvents[index];
        if (pending.type == PlatformEventType::WindowResized && pending.window == event.window) {
            return true;
        }

        if (index == m_syntheticHead) {
            break;
        }

        index = (index + kMaxSyntheticEvents - 1u) % kMaxSyntheticEvents;
    }

    return false;
}

PendingResizeExtent EventPump::pendingResizeExtentFor(const Window& window) const {
    PendingResizeExtent extent;
    if (m_syntheticHead == m_syntheticTail) {
        return extent;
    }

    u32 index = m_syntheticHead;
    while (index != m_syntheticTail) {
        const PlatformEvent& pending = m_syntheticEvents[index];
        if (pending.type == PlatformEventType::WindowResized && pending.window == &window &&
            pending.width > 0u && pending.height > 0u) {
            extent.width = pending.width;
            extent.height = pending.height;
            extent.pending = true;
        }

        index = (index + 1u) % kMaxSyntheticEvents;
    }

    return extent;
}

const ResizeCoalesceRecord& EventPump::lastCoalescedResize() const {
    return m_lastCoalescedResize;
}

EventPumpStats EventPump::stats() const {
    EventPumpStats snapshot;
    snapshot.pendingEventCount = pendingEventCount();
    snapshot.droppedEventCount = m_droppedEventCount;
    snapshot.coalescedResizeCount = m_coalescedResizeCount;
    snapshot.quitRequested = m_quitRequested;
    snapshot.hasPendingQuitEvent = hasPendingEventOfType(PlatformEventType::Quit);
    snapshot.hasPendingResizeEvent = hasPendingEventOfType(PlatformEventType::WindowResized);
    snapshot.lastCoalescedResizeValid = m_lastCoalescedResize.valid;
    peekEventType(snapshot.frontEventType);
    return snapshot;
}

u32 EventPump::droppedEventCount() const {
    return m_droppedEventCount;
}

u32 EventPump::coalescedResizeCount() const {
    return m_coalescedResizeCount;
}

void EventPump::processOsEvents() {
    // Desktop + mobile no-op — OS backends enqueue into the synthetic queue later.
}

bool EventPump::pumpOnce() {
    if (!hasPendingEvents() && !m_quitRequested) {
        processOsEvents();
        return true;
    }

    processOsEvents();

    if (!hasPendingEvents()) {
        return !m_quitRequested;
    }

    PlatformEvent event;
    while (pollEvent(event)) {
        if (event.type == PlatformEventType::Quit) {
            return false;
        }
    }

    return !m_quitRequested;
}

u32 EventPump::drainEvents(std::vector<PlatformEvent>& out) {
    const u32 pending = pendingEventCount();
    if (pending == 0) {
        return 0;
    }

    out.reserve(out.size() + pending);

    u32 drained = 0;
    PlatformEvent event;
    while (pollEvent(event)) {
        out.push_back(event);
        ++drained;
    }
    return drained;
}

bool EventPump::tryCoalescePendingResize_(const PlatformEvent& event) {
    if (event.type != PlatformEventType::WindowResized || event.window == nullptr ||
        event.width == 0u || event.height == 0u || m_syntheticHead == m_syntheticTail) {
        return false;
    }

    u32 index = (m_syntheticTail + kMaxSyntheticEvents - 1u) % kMaxSyntheticEvents;
    while (true) {
        PlatformEvent& pending = m_syntheticEvents[index];
        if (pending.type == PlatformEventType::WindowResized && pending.window == event.window) {
            pending.width = event.width;
            pending.height = event.height;
            ++m_coalescedResizeCount;
            m_lastCoalescedResize.window = event.window;
            m_lastCoalescedResize.width = event.width;
            m_lastCoalescedResize.height = event.height;
            m_lastCoalescedResize.valid = true;
            return true;
        }

        if (index == m_syntheticHead) {
            break;
        }

        index = (index + kMaxSyntheticEvents - 1u) % kMaxSyntheticEvents;
    }

    return false;
}

void EventPump::enqueueSyntheticEvent_(const PlatformEvent& event) {
    const u32 nextTail = (m_syntheticTail + 1u) % kMaxSyntheticEvents;
    if (nextTail == m_syntheticHead) {
        ++m_droppedEventCount;
        return;
    }

    m_syntheticEvents[m_syntheticTail] = event;
    m_syntheticTail = nextTail;
}

void EventPump::pushSyntheticEvent(const PlatformEvent& event) {
    if (tryCoalescePendingResize_(event)) {
        return;
    }

    enqueueSyntheticEvent_(event);
}

void EventPump::pushWindowResized(Window& window) {
    PlatformEvent event;
    event.type = PlatformEventType::WindowResized;
    event.window = &window;
    event.width = window.width();
    event.height = window.height();
    pushSyntheticEvent(event);
}

void EventPump::pushWindowFocusGained(Window& window) {
    PlatformEvent event;
    event.type = PlatformEventType::WindowFocusGained;
    event.window = &window;
    pushSyntheticEvent(event);
}

void EventPump::pushWindowFocusLost(Window& window) {
    PlatformEvent event;
    event.type = PlatformEventType::WindowFocusLost;
    event.window = &window;
    pushSyntheticEvent(event);
}

void EventPump::pushWindowCloseRequested(Window& window) {
    PlatformEvent event;
    event.type = PlatformEventType::WindowCloseRequested;
    event.window = &window;
    pushSyntheticEvent(event);
}

void EventPump::requestQuit() {
    m_quitRequested = true;
    PlatformEvent quitEvent;
    quitEvent.type = PlatformEventType::Quit;
    pushSyntheticEvent(quitEvent);
}

bool EventPump::quitRequested() const {
    return m_quitRequested;
}

void EventPump::resetQuit() {
    m_quitRequested = false;
    clearSyntheticEvents();
}

void EventPump::clearSyntheticEvents() {
    m_syntheticHead = 0;
    m_syntheticTail = 0;
}

void EventPump::resetEventStats() {
    m_droppedEventCount = 0;
    m_coalescedResizeCount = 0;
    m_lastCoalescedResize = {};
}

} // namespace fuse::platform
