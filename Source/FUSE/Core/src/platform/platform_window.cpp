#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>

#include <utility>

#if defined(FUSE_PLATFORM_WINDOW_GLFW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fuse::platform {

namespace {

constexpr u32 kMaxPumpWindows = 32;
Window* g_pumpWindows[kMaxPumpWindows]{};
u32 g_pumpWindowCount = 0;
bool g_requireCaptureForInput = false;

void registerPumpWindow(Window* window) {
    if (window == nullptr || window->nativeHandle().value == nullptr) {
        return;
    }

    for (u32 i = 0; i < g_pumpWindowCount; ++i) {
        if (g_pumpWindows[i] == window) {
            return;
        }
    }

    if (g_pumpWindowCount >= kMaxPumpWindows) {
        return;
    }

    g_pumpWindows[g_pumpWindowCount++] = window;
}

void unregisterPumpWindow(Window* window) {
    if (window == nullptr || g_pumpWindowCount == 0u) {
        return;
    }

    for (u32 i = 0; i < g_pumpWindowCount; ++i) {
        if (g_pumpWindows[i] != window) {
            continue;
        }

        g_pumpWindows[i] = g_pumpWindows[g_pumpWindowCount - 1u];
        g_pumpWindows[g_pumpWindowCount - 1u] = nullptr;
        --g_pumpWindowCount;
        return;
    }
}

const char* sanitizeTitle(const char* title) {
    return (title != nullptr && title[0] != '\0') ? title : "FUSE";
}

bool isWindowScopedEventType(PlatformEventType type) {
    return type == PlatformEventType::WindowCloseRequested ||
           type == PlatformEventType::WindowResized ||
           type == PlatformEventType::WindowFocusGained ||
           type == PlatformEventType::WindowFocusLost;
}

#if defined(_WIN32)
constexpr wchar_t kOwnedWindowClassName[] = L"FUSE_PlatformWindow";

bool registerOwnedWindowClassOnce() {
    static bool ready = false;
    if (ready) {
        return true;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kOwnedWindowClassName;

    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    ready = true;
    return true;
}

HWND createHiddenOverlappedWindow(u32 width, u32 height, const char* title) {
    if (!registerOwnedWindowClassOnce()) {
        return nullptr;
    }

    wchar_t wideTitle[256];
    const int converted = MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle, 256);
    const wchar_t* titleW = (converted > 0) ? wideTitle : L"FUSE";

    return CreateWindowExW(0, kOwnedWindowClassName, titleW, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, static_cast<int>(width), static_cast<int>(height), nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}
#endif

void destroyNativeWindow(void*& nativeWindow) {
    if (nativeWindow == nullptr) {
        return;
    }

#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    glfwDestroyWindow(static_cast<GLFWwindow*>(nativeWindow));
#elif defined(_WIN32)
    DestroyWindow(reinterpret_cast<HWND>(nativeWindow));
#endif
    nativeWindow = nullptr;
}

void createNativeWindowIfAvailable(u32 width, u32 height, const char* title, bool createNative,
                                   void*& nativeWindow) {
    nativeWindow = nullptr;
    if (!createNative) {
        return;
    }

#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    if (!windowWsiAvailable()) {
        return;
    }

    GLFWwindow* window =
        glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title, nullptr, nullptr);
    if (window == nullptr) {
        return;
    }

    nativeWindow = window;
#elif defined(_WIN32)
    nativeWindow = createHiddenOverlappedWindow(width, height, title);
#else
    (void)width;
    (void)height;
    (void)title;
#endif
}

void releaseOwnedNativeWindow(void*& nativeWindow, bool& ownsNativeWindow) {
    if (ownsNativeWindow) {
        destroyNativeWindow(nativeWindow);
    } else {
        nativeWindow = nullptr;
    }
    ownsNativeWindow = false;
}

#if defined(_WIN32)
i32 osMessageAxisX(LPARAM lParam) {
    return static_cast<i32>(static_cast<short>(LOWORD(lParam)));
}

i32 osMessageAxisY(LPARAM lParam) {
    return static_cast<i32>(static_cast<short>(HIWORD(lParam)));
}

#if !defined(FUSE_PLATFORM_WINDOW_GLFW)
HWND ownedHwnd(void* nativeWindow, bool ownsNativeWindow) {
    if (!ownsNativeWindow || nativeWindow == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HWND>(nativeWindow);
}

void registerRawMouseInput(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void unregisterRawMouseInput() {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void applyOwnedHwndInputCapture(HWND hwnd, bool captured) {
    if (hwnd == nullptr) {
        return;
    }

    if (captured) {
        RECT client{};
        if (GetClientRect(hwnd, &client) != FALSE) {
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<LPPOINT>(&client), 2);
            ClipCursor(&client);
        }
        ShowCursor(FALSE);
        registerRawMouseInput(hwnd);
        return;
    }

    ClipCursor(nullptr);
    ShowCursor(TRUE);
    unregisterRawMouseInput();
}

void releaseOwnedHwndInputCapture(void* nativeWindow, bool ownsNativeWindow,
                                  InputCaptureMode capture) {
    if (capture != InputCaptureMode::Captured) {
        return;
    }
    applyOwnedHwndInputCapture(ownedHwnd(nativeWindow, ownsNativeWindow), false);
}

void enqueueRawMouseDeltaFromWmInput(EventPump& pump, Window* window, LPARAM lParam) {
    const HRAWINPUT rawInput = reinterpret_cast<HRAWINPUT>(lParam);
    UINT size = 0;
    if (GetRawInputData(rawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1) ||
        size == 0u || size > sizeof(RAWINPUT)) {
        return;
    }

    RAWINPUT raw{};
    UINT copied = size;
    if (GetRawInputData(rawInput, RID_INPUT, &raw, &copied, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1) ||
        raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }

    if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        return;
    }

    PlatformEvent event{};
    event.type = PlatformEventType::RawMouseDelta;
    event.window = window;
    event.mouseX = raw.data.mouse.lLastX;
    event.mouseY = raw.data.mouse.lLastY;
    pump.pushSyntheticEvent(event);
}
#endif

bool shouldDropOsKeyOrMouse(const EventPump& pump, const Window* window) {
    if (!pump.requireCaptureForInput()) {
        return false;
    }

    return window == nullptr || !window->isInputCaptured();
}

void enqueueMappedOsMessage(EventPump& pump, Window* window, const MSG& msg) {
    const bool dropKeyMouse = shouldDropOsKeyOrMouse(pump, window);

    switch (msg.message) {
    case WM_KEYDOWN: {
        if (dropKeyMouse) {
            break;
        }
        PlatformEvent event{};
        event.type = PlatformEventType::KeyDown;
        event.window = window;
        event.keyCode = static_cast<u32>(msg.wParam);
        pump.pushSyntheticEvent(event);
        break;
    }
    case WM_KEYUP: {
        if (dropKeyMouse) {
            break;
        }
        PlatformEvent event{};
        event.type = PlatformEventType::KeyUp;
        event.window = window;
        event.keyCode = static_cast<u32>(msg.wParam);
        pump.pushSyntheticEvent(event);
        break;
    }
    case WM_MOUSEMOVE: {
        if (dropKeyMouse) {
            break;
        }
        PlatformEvent event{};
        event.type = PlatformEventType::MouseMove;
        event.window = window;
        event.mouseX = osMessageAxisX(msg.lParam);
        event.mouseY = osMessageAxisY(msg.lParam);
        pump.pushSyntheticEvent(event);
        break;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        if (dropKeyMouse) {
            break;
        }

        u8 button = 0;
        bool down = false;
        switch (msg.message) {
        case WM_LBUTTONDOWN:
            button = 1u;
            down = true;
            break;
        case WM_LBUTTONUP:
            button = 1u;
            break;
        case WM_RBUTTONDOWN:
            button = 2u;
            down = true;
            break;
        case WM_RBUTTONUP:
            button = 2u;
            break;
        case WM_MBUTTONDOWN:
            button = 3u;
            down = true;
            break;
        case WM_MBUTTONUP:
            button = 3u;
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const u16 xButton = HIWORD(msg.wParam);
            if (xButton == 1u) {
                button = 4u;
            } else if (xButton == 2u) {
                button = 5u;
            }
            down = msg.message == WM_XBUTTONDOWN;
            break;
        }
        default:
            break;
        }

        if (button == 0u) {
            break;
        }

        PlatformEvent event{};
        event.type = down ? PlatformEventType::MouseButtonDown : PlatformEventType::MouseButtonUp;
        event.window = window;
        event.mouseX = osMessageAxisX(msg.lParam);
        event.mouseY = osMessageAxisY(msg.lParam);
        event.mouseButton = button;
        pump.pushSyntheticEvent(event);
        break;
    }
    case WM_MOUSEWHEEL: {
        if (dropKeyMouse) {
            break;
        }
        PlatformEvent event{};
        event.type = PlatformEventType::MouseMove;
        event.window = window;
        event.mouseX = osMessageAxisX(msg.lParam);
        event.mouseY = static_cast<i32>(GET_WHEEL_DELTA_WPARAM(msg.wParam));
        pump.pushSyntheticEvent(event);
        break;
    }
    case WM_INPUT: {
        if (dropKeyMouse) {
            break;
        }
#if !defined(FUSE_PLATFORM_WINDOW_GLFW)
        enqueueRawMouseDeltaFromWmInput(pump, window, msg.lParam);
#endif
        break;
    }
    case WM_SETFOCUS:
        if (window != nullptr) {
            window->setFocused(true, &pump);
        }
        break;
    case WM_KILLFOCUS:
        if (window != nullptr) {
            window->setFocused(false, &pump);
        }
        break;
    case WM_CLOSE:
        if (window != nullptr) {
            window->requestClose(&pump);
        }
        break;
    case WM_SIZE:
        if (window != nullptr && msg.wParam != SIZE_MINIMIZED) {
            const u32 width = static_cast<u32>(LOWORD(msg.lParam));
            const u32 height = static_cast<u32>(HIWORD(msg.lParam));
            if (width > 0u && height > 0u) {
                window->resize(width, height, &pump);
            }
        }
        break;
    case WM_QUIT:
        pump.requestQuit();
        break;
    default:
        break;
    }
}

bool dispatchMappedOsMessage(const MSG& msg) {
    return msg.message != WM_CLOSE && msg.message != WM_QUIT;
}
#endif

} // namespace

Window::Window() {
    m_valid = true;
    m_width = 1920u;
    m_height = 1080u;
    m_title = "FUSE";
    m_createNative = false;
    createNativeWindowIfAvailable(m_width, m_height, m_title.c_str(), false, m_nativeWindow);
    m_ownsNativeWindow = m_nativeWindow != nullptr;
#if defined(_WIN32) && !defined(FUSE_PLATFORM_WINDOW_GLFW)
    m_pumpAsHwnd = m_ownsNativeWindow;
#endif
    registerPumpWindow(this);
}

Window::Window(const WindowDesc& desc) {
    m_valid = true;
    m_width = desc.width > 0 ? desc.width : 1920u;
    m_height = desc.height > 0 ? desc.height : 1080u;
    m_fullscreen = desc.fullscreen;
    m_borderless = desc.borderless;
    m_vsync = desc.vsync;
    m_createNative = desc.createNative;
    m_title = sanitizeTitle(desc.title);
    createNativeWindowIfAvailable(m_width, m_height, m_title.c_str(), desc.createNative, m_nativeWindow);
    m_ownsNativeWindow = m_nativeWindow != nullptr;
#if defined(_WIN32) && !defined(FUSE_PLATFORM_WINDOW_GLFW)
    m_pumpAsHwnd = m_ownsNativeWindow;
#endif
    registerPumpWindow(this);
}

Window::~Window() {
    unregisterPumpWindow(this);
#if defined(_WIN32) && !defined(FUSE_PLATFORM_WINDOW_GLFW)
    releaseOwnedHwndInputCapture(m_nativeWindow, m_ownsNativeWindow, m_inputCapture);
#endif
    releaseOwnedNativeWindow(m_nativeWindow, m_ownsNativeWindow);
    m_pumpAsHwnd = false;
    m_valid = false;
}

Window::Window(Window&& other) noexcept
    : m_valid(other.m_valid),
      m_width(other.m_width),
      m_height(other.m_height),
      m_fullscreen(other.m_fullscreen),
      m_borderless(other.m_borderless),
      m_vsync(other.m_vsync),
      m_createNative(other.m_createNative),
      m_focused(other.m_focused),
      m_inputCapture(other.m_inputCapture),
      m_closeRequest(other.m_closeRequest),
      m_title(std::move(other.m_title)),
      m_nativeWindow(other.m_nativeWindow),
      m_ownsNativeWindow(other.m_ownsNativeWindow),
      m_pumpAsHwnd(other.m_pumpAsHwnd) {
    unregisterPumpWindow(&other);
    other.m_valid = false;
    other.m_inputCapture = InputCaptureMode::Released;
    other.m_closeRequest = WindowCloseRequest::None;
    other.m_nativeWindow = nullptr;
    other.m_ownsNativeWindow = false;
    other.m_pumpAsHwnd = false;
    registerPumpWindow(this);
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        unregisterPumpWindow(this);
#if defined(_WIN32) && !defined(FUSE_PLATFORM_WINDOW_GLFW)
        releaseOwnedHwndInputCapture(m_nativeWindow, m_ownsNativeWindow, m_inputCapture);
#endif
        releaseOwnedNativeWindow(m_nativeWindow, m_ownsNativeWindow);

        m_valid = other.m_valid;
        m_width = other.m_width;
        m_height = other.m_height;
        m_fullscreen = other.m_fullscreen;
        m_borderless = other.m_borderless;
        m_vsync = other.m_vsync;
        m_createNative = other.m_createNative;
        m_focused = other.m_focused;
        m_inputCapture = other.m_inputCapture;
        m_closeRequest = other.m_closeRequest;
        m_title = std::move(other.m_title);
        m_nativeWindow = other.m_nativeWindow;
        m_ownsNativeWindow = other.m_ownsNativeWindow;
        m_pumpAsHwnd = other.m_pumpAsHwnd;

        unregisterPumpWindow(&other);
        other.m_valid = false;
        other.m_inputCapture = InputCaptureMode::Released;
        other.m_closeRequest = WindowCloseRequest::None;
        other.m_nativeWindow = nullptr;
        other.m_ownsNativeWindow = false;
        other.m_pumpAsHwnd = false;
        registerPumpWindow(this);
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
    desc.createNative = m_createNative;
    return desc;
}

NativeWindowHandle Window::nativeHandle() const {
    NativeWindowHandle handle;
    handle.value = m_nativeWindow;
    return handle;
}

void Window::setNativeHandleForPump(void* hwnd) {
    unregisterPumpWindow(this);
#if defined(_WIN32) && !defined(FUSE_PLATFORM_WINDOW_GLFW)
    releaseOwnedHwndInputCapture(m_nativeWindow, m_ownsNativeWindow, m_inputCapture);
#endif
    releaseOwnedNativeWindow(m_nativeWindow, m_ownsNativeWindow);
    m_nativeWindow = hwnd;
    m_pumpAsHwnd = hwnd != nullptr;
    registerPumpWindow(this);
}

void Window::setInputCapture(InputCaptureMode mode) {
    if (m_inputCapture == mode) {
        return;
    }

#if defined(_WIN32) && !defined(FUSE_PLATFORM_WINDOW_GLFW)
    const HWND hwnd = ownedHwnd(m_nativeWindow, m_ownsNativeWindow);
    if (hwnd != nullptr) {
        applyOwnedHwndInputCapture(hwnd, mode == InputCaptureMode::Captured);
    }
#endif

    m_inputCapture = mode;
}

void* Window::nativeVulkanSurface() const {
    return nullptr;
}

VulkanSurfaceWire Window::vulkanSurfaceWire() const {
    VulkanSurfaceWire wire;
    wire.nativeSurface = nativeVulkanSurface();
    wire.presentable = m_nativeWindow != nullptr;
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

void EventPump::setRequireCaptureForInput(bool require) {
    g_requireCaptureForInput = require;
}

bool EventPump::requireCaptureForInput() const {
    return g_requireCaptureForInput;
}

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

bool EventPump::tryPeekEventFor(const Window& window, PlatformEvent& outEvent) const {
    if (m_syntheticHead == m_syntheticTail) {
        outEvent = {};
        return false;
    }

    const PlatformEvent& front = m_syntheticEvents[m_syntheticHead];
    if (front.window != &window) {
        outEvent = {};
        return false;
    }

    outEvent = front;
    return true;
}

bool EventPump::tryPeekEventOfTypeFor(const Window& window, PlatformEventType type,
                                      PlatformEvent& outEvent) const {
    if (m_syntheticHead == m_syntheticTail) {
        outEvent = {};
        return false;
    }

    const PlatformEvent& front = m_syntheticEvents[m_syntheticHead];
    if (front.type != type || front.window != &window) {
        outEvent = {};
        return false;
    }

    outEvent = front;
    return true;
}

bool EventPump::tryPollEventOfType(PlatformEventType type, PlatformEvent& outEvent) {
    if (!tryPeekEventOfType(type, outEvent)) {
        return false;
    }

    return pollEvent(outEvent);
}

bool EventPump::tryPollEventOfTypeFor(const Window& window, PlatformEventType type,
                                      PlatformEvent& outEvent) {
    if (!tryPeekEventOfTypeFor(window, type, outEvent)) {
        return false;
    }

    return pollEvent(outEvent);
}

bool EventPump::frontEventTypeIs(PlatformEventType type) const {
    PlatformEventType front = PlatformEventType::None;
    if (!peekEventType(front)) {
        return false;
    }

    return front == type;
}

bool EventPump::frontEventIsFor(const Window& window, PlatformEventType type) const {
    if (m_syntheticHead == m_syntheticTail) {
        return false;
    }

    const PlatformEvent& front = m_syntheticEvents[m_syntheticHead];
    return front.type == type && front.window == &window;
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

bool EventPump::hasPendingEventOfTypeFor(const Window& window, PlatformEventType type) const {
    return countPendingEventsOfTypeFor(window, type) > 0u;
}

bool EventPump::hasPendingEventsFor(const Window& window) const {
    return countPendingEventsFor(window) > 0u;
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

u32 EventPump::countPendingEventsOfTypeFor(const Window& window, PlatformEventType type) const {
    if (m_syntheticHead == m_syntheticTail) {
        return 0;
    }

    u32 count = 0;
    u32 index = m_syntheticHead;
    while (index != m_syntheticTail) {
        const PlatformEvent& pending = m_syntheticEvents[index];
        if (pending.type == type && pending.window == &window) {
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

bool EventPump::wouldCoalesceResizeFor(const Window& window) const {
    if (window.width() == 0u || window.height() == 0u || m_syntheticHead == m_syntheticTail) {
        return false;
    }

    u32 index = (m_syntheticTail + kMaxSyntheticEvents - 1u) % kMaxSyntheticEvents;
    while (true) {
        const PlatformEvent& pending = m_syntheticEvents[index];
        if (pending.type == PlatformEventType::WindowResized && pending.window == &window) {
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

bool EventPump::hasCoalescedResizeFor(const Window& window) const {
    return m_lastCoalescedResize.valid && m_lastCoalescedResize.window == &window;
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
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    if (windowWsiAvailable()) {
        glfwPollEvents();
    }
#endif

#if defined(_WIN32)
    if (g_pumpWindowCount == 0u) {
        return;
    }

    bool pumpedHwnd = false;
    for (u32 i = 0; i < g_pumpWindowCount; ++i) {
        Window* window = g_pumpWindows[i];
        if (window == nullptr || !window->m_pumpAsHwnd || window->m_nativeWindow == nullptr) {
            continue;
        }

        const HWND hwnd = reinterpret_cast<HWND>(window->m_nativeWindow);
        pumpedHwnd = true;

        MSG msg;
        while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE) != FALSE) {
            if (dispatchMappedOsMessage(msg)) {
                TranslateMessage(&msg);
            }
            enqueueMappedOsMessage(*this, window, msg);
            if (dispatchMappedOsMessage(msg)) {
                DispatchMessageW(&msg);
            }
        }
    }

    if (!pumpedHwnd) {
        return;
    }

    MSG threadMsg;
    while (PeekMessageW(&threadMsg, reinterpret_cast<HWND>(-1), 0, 0, PM_REMOVE) != FALSE) {
        if (threadMsg.message == WM_QUIT) {
            requestQuit();
        }
    }
#else
    (void)this;
#endif
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
    if (event.type == PlatformEventType::None) {
        return;
    }

    if (isWindowScopedEventType(event.type) && event.window == nullptr) {
        return;
    }

    if (tryCoalescePendingResize_(event)) {
        return;
    }

    enqueueSyntheticEvent_(event);
}

void EventPump::pushWindowResized(Window& window) {
    PlatformEvent event{};
    event.type = PlatformEventType::WindowResized;
    event.window = &window;
    event.width = window.width();
    event.height = window.height();
    event.keyCode = 0;
    event.mouseX = 0;
    event.mouseY = 0;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushWindowFocusGained(Window& window) {
    PlatformEvent event{};
    event.type = PlatformEventType::WindowFocusGained;
    event.window = &window;
    event.keyCode = 0;
    event.mouseX = 0;
    event.mouseY = 0;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushWindowFocusLost(Window& window) {
    PlatformEvent event{};
    event.type = PlatformEventType::WindowFocusLost;
    event.window = &window;
    event.keyCode = 0;
    event.mouseX = 0;
    event.mouseY = 0;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushWindowCloseRequested(Window& window) {
    PlatformEvent event{};
    event.type = PlatformEventType::WindowCloseRequested;
    event.window = &window;
    event.keyCode = 0;
    event.mouseX = 0;
    event.mouseY = 0;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushKeyDown(u32 keyCode) {
    PlatformEvent event{};
    event.type = PlatformEventType::KeyDown;
    event.keyCode = keyCode;
    event.mouseX = 0;
    event.mouseY = 0;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushKeyUp(u32 keyCode) {
    PlatformEvent event{};
    event.type = PlatformEventType::KeyUp;
    event.keyCode = keyCode;
    event.mouseX = 0;
    event.mouseY = 0;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushMouseMove(i32 x, i32 y) {
    PlatformEvent event{};
    event.type = PlatformEventType::MouseMove;
    event.keyCode = 0;
    event.mouseX = x;
    event.mouseY = y;
    event.mouseButton = 0;
    pushSyntheticEvent(event);
}

void EventPump::pushMouseButton(u8 button, bool down, i32 x, i32 y) {
    PlatformEvent event{};
    event.type = down ? PlatformEventType::MouseButtonDown : PlatformEventType::MouseButtonUp;
    event.keyCode = 0;
    event.mouseX = x;
    event.mouseY = y;
    event.mouseButton = button;
    pushSyntheticEvent(event);
}

void EventPump::requestQuit() {
    m_quitRequested = true;
    PlatformEvent quitEvent{};
    quitEvent.type = PlatformEventType::Quit;
    quitEvent.keyCode = 0;
    quitEvent.mouseX = 0;
    quitEvent.mouseY = 0;
    quitEvent.mouseButton = 0;
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
