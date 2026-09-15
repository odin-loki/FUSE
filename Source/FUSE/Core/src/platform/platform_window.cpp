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
        return false;
    }

    outEvent = m_syntheticEvents[m_syntheticHead];
    m_syntheticHead = (m_syntheticHead + 1) % kMaxSyntheticEvents;

    if (outEvent.type == PlatformEventType::Quit) {
        m_quitRequested = true;
    }

    return true;
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

void EventPump::processOsEvents() {
    // Desktop + mobile no-op — OS backends enqueue into the synthetic queue later.
}

bool EventPump::pumpOnce() {
    processOsEvents();

    PlatformEvent event;
    while (pollEvent(event)) {
        if (event.type == PlatformEventType::Quit) {
            return false;
        }
    }

    return !m_quitRequested;
}

u32 EventPump::drainEvents(std::vector<PlatformEvent>& out) {
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
        m_syntheticHead == m_syntheticTail) {
        return false;
    }

    u32 index = (m_syntheticTail + kMaxSyntheticEvents - 1u) % kMaxSyntheticEvents;
    while (true) {
        PlatformEvent& pending = m_syntheticEvents[index];
        if (pending.type == PlatformEventType::WindowResized && pending.window == event.window) {
            pending.width = event.width;
            pending.height = event.height;
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

} // namespace fuse::platform
