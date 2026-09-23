#include "viewport_placeholder_widget.hpp"
#include "viewport_qt_vulkan_surface.hpp"
#include "viewport_vulkan_window.hpp"

#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/editor/command_queue.hpp>

#include <QAction>
#include <QColor>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QGuiApplication>
#include <QShowEvent>
#include <QWheelEvent>
#include <QWindow>
#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace fuse::editor::qt {

ViewportPlaceholderWidget::ViewportPlaceholderWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    setObjectName(QStringLiteral("fuseViewport"));
    setMinimumSize(320, 180);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    // Right-click is resolved on release (click = context menu, drag = fly camera).
    setContextMenuPolicy(Qt::PreventContextMenu);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor(32, 36, 44));
    setPalette(palette);

    m_frameTimer.setTimerType(Qt::PreciseTimer);
    m_frameTimer.setInterval(kFrameIntervalMs);
    connect(&m_frameTimer, &QTimer::timeout, this, &ViewportPlaceholderWidget::onFrameTimer);
    m_presentPollTimer.setInterval(15);
    connect(&m_presentPollTimer, &QTimer::timeout, this, &ViewportPlaceholderWidget::pollEmbeddedVulkanViewport);
}

ViewportPlaceholderWidget::~ViewportPlaceholderWidget() {
    if (m_activeMenu) {
        m_activeMenu->close();
    }
    // MainWindow releases the surface in order (swapchain -> surface -> instance) before the host
    // goes; this only covers a widget used without it.
    releaseVulkanViewport();
}

// ---- embedded Vulkan viewport ---------------------------------------------------------------------

namespace {

/// Instance extensions Qt's platform plugin needs to create a VkSurfaceKHR for a QWindow.
std::vector<std::string> platformSurfaceExtensions() {
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("xcb")) {
        return {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    }
    if (platform.startsWith(QLatin1String("wayland"))) {
        return {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    }
    if (platform == QLatin1String("windows")) {
        return {"VK_KHR_surface", "VK_KHR_win32_surface"};
    }
    return {}; // offscreen / minimal / cocoa (MoltenVK not wired): software placeholder
}

} // namespace

bool ViewportPlaceholderWidget::enableEmbeddedVulkanViewport(bool enableValidation) {
#if QT_CONFIG(vulkan) && defined(FUSE_VULKAN_BACKEND)
    if (m_windowPresentRequested) {
        return true;
    }
    if (qEnvironmentVariableIntValue("FUSE_EDITOR_HEADLESS_VIEWPORT") != 0) {
        return false;
    }
    const std::vector<std::string> extensions = platformSurfaceExtensions();
    if (extensions.empty()) {
        return false;
    }
    WindowPresentRequest request;
    request.instanceExtensions = extensions;
    request.enableValidation = enableValidation;
    m_host.runtimeViewport().requestWindowSystemPresent(request);
    m_windowPresentRequested = true;
    m_presentPollTimer.start();
    return true;
#else
    (void)enableValidation;
    return false;
#endif
}

void ViewportPlaceholderWidget::pollEmbeddedVulkanViewport() {
    if (!m_windowPresentRequested) {
        m_presentPollTimer.stop();
        return;
    }
    const RuntimeViewportHook& hook = m_host.runtimeViewport();
    switch (hook.windowPresentState()) {
    case WindowPresentState::Off:
    case WindowPresentState::InstancePending:
        return;
    case WindowPresentState::Failed:
        fallBackToPlaceholder_();
        return;
    case WindowPresentState::InstanceReady:
        if (m_vkWindow == nullptr && isVisible() && width() > 0 && height() > 0) {
            if (!attachVulkanWindow_(hook.windowPresentInstance())) {
                fallBackToPlaceholder_();
            }
        }
        return;
    case WindowPresentState::SurfaceWired:
        return; // keep polling: a later wiring failure falls back to the placeholder
    }
}

bool ViewportPlaceholderWidget::attachVulkanWindow_(void* vkInstance) {
#if QT_CONFIG(vulkan) && defined(FUSE_VULKAN_BACKEND)
    if (vkInstance == nullptr) {
        return false;
    }
    // Qt adopts the game thread's instance (it never destroys an adopted VkInstance), so the
    // surface it creates is owned by the instance the swapchain lives on.
    m_qtVulkanInstance = std::make_unique<QVulkanInstance>();
    m_qtVulkanInstance->setVkInstance(static_cast<VkInstance>(vkInstance));
    if (!m_qtVulkanInstance->create()) {
        m_qtVulkanInstance.reset();
        return false;
    }

    m_vkWindow = new ViewportVulkanWindow(this);
    m_vkWindow->setVulkanInstance(m_qtVulkanInstance.get());
    m_vkContainer = QWidget::createWindowContainer(m_vkWindow, this, Qt::Widget);
    m_vkContainer->setObjectName(QStringLiteral("fuseViewportVulkanContainer"));
    // Keyboard focus stays on this widget (the container is only a native host); events that
    // reach the child window are forwarded here by ViewportVulkanWindow.
    m_vkContainer->setFocusPolicy(Qt::NoFocus);
    m_vkContainer->setGeometry(rect());
    m_vkContainer->show();
    if (m_vkWindow->handle() == nullptr) {
        m_vkWindow->create();
    }
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(m_vkWindow);
    if (surface == VK_NULL_HANDLE) {
        releaseVulkanViewport();
        return false;
    }

    syncPanelSize();
    postViewportResize();
    EditorCommand cmd;
    cmd.kind = CommandKind::SetProperty;
    cmd.propertyName = "viewport.vk_surface_adopted";
    cmd.propertyValue = std::to_string(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(surface))) +
                        ' ' +
                        std::to_string(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(vkInstance))) +
                        ' ' + std::to_string(m_panel.width()) + ' ' + std::to_string(m_panel.height());
    m_host.postFromUi(std::move(cmd));
    m_vkSurfacePosted = true;
    return true;
#else
    (void)vkInstance;
    return false;
#endif
}

void ViewportPlaceholderWidget::fallBackToPlaceholder_() {
    m_presentPollTimer.stop();
    m_windowPresentRequested = false;
    if (m_vkContainer != nullptr) {
        m_vkContainer->hide();
    }
    update();
}

void ViewportPlaceholderWidget::releaseVulkanViewport() {
    m_presentPollTimer.stop();
    m_vkSurfacePosted = false;
    if (m_vkContainer != nullptr) {
        // Deleting the container deletes the embedded QWindow; destroying its platform window
        // destroys Qt's VkSurfaceKHR through the (still alive) adopted instance.
        delete m_vkContainer;
        m_vkContainer = nullptr;
        m_vkWindow = nullptr;
    } else if (m_vkWindow != nullptr) {
        delete m_vkWindow;
        m_vkWindow = nullptr;
    }
#if QT_CONFIG(vulkan)
    if (m_qtVulkanInstance != nullptr) {
        m_qtVulkanInstance->destroy();
        m_qtVulkanInstance.reset();
    }
#endif
}

void ViewportPlaceholderWidget::setProjectLabel(const QString& projectName) {
    m_projectName = projectName;
    m_panel.setProjectLabel(projectName.toStdString());
    update();
}

// ---- frame pump ------------------------------------------------------------------------------------

void ViewportPlaceholderWidget::setFramePumpEnabled(bool enabled) {
    if (enabled && !m_frameTimer.isActive()) {
        m_frameClock.start();
        m_frameTimer.start();
    } else if (!enabled) {
        m_frameTimer.stop();
    }
}

void ViewportPlaceholderWidget::onFrameTimer() {
    const qint64 ns = m_frameClock.nsecsElapsed();
    m_frameClock.restart();
    advanceFrame(static_cast<float>(static_cast<double>(ns) * 1e-9));
}

void ViewportPlaceholderWidget::advanceFrame(float dt) {
    dt = std::clamp(dt, 0.f, kMaxFrameDt);
    ViewportInput input{};
    input.lookHeld = m_lookHeld;
    input.mouseDeltaX = m_pendingMouseDx;
    input.mouseDeltaY = m_pendingMouseDy;
    input.moveForward = m_keys.forward;
    input.moveBack = m_keys.back;
    input.moveLeft = m_keys.left;
    input.moveRight = m_keys.right;
    input.moveUp = m_keys.up;
    input.moveDown = m_keys.down;
    input.fast = m_keys.fast;
    m_pendingMouseDx = 0.f;
    m_pendingMouseDy = 0.f;
    m_panel.submitInput(input);
    m_panel.tick(dt);
    ++m_frameCount;
    emit frameAdvanced(dt);
    if (m_lookHeld || m_keys.forward || m_keys.back || m_keys.left || m_keys.right || m_keys.up || m_keys.down) {
        update();
    }
}

// ---- input -----------------------------------------------------------------------------------------

bool ViewportPlaceholderWidget::setKey(int key, bool down) {
    switch (key) {
    case Qt::Key_W: m_keys.forward = down; break;
    case Qt::Key_S: m_keys.back = down; break;
    case Qt::Key_A: m_keys.left = down; break;
    case Qt::Key_D: m_keys.right = down; break;
    case Qt::Key_E: m_keys.up = down; break;
    case Qt::Key_Q: m_keys.down = down; break;
    case Qt::Key_Shift: m_keys.fast = down; break;
    default: return false;
    }
    if (down && m_lookHeld && key != Qt::Key_Shift) {
        m_flyKeyUsedDuringLook = true;
    }
    return true;
}

void ViewportPlaceholderWidget::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        // Held keys are level-triggered; X11 auto-repeat arrives as release+press pairs.
        event->accept();
        return;
    }
    if (setKey(event->key(), true)) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ViewportPlaceholderWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    if (setKey(event->key(), false)) {
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void ViewportPlaceholderWidget::focusOutEvent(QFocusEvent* event) {
    // Releases may be delivered elsewhere once focus leaves: never leave the camera drifting.
    m_keys = HeldKeys{};
    m_lookHeld = false;
    QWidget::focusOutEvent(event);
}

void ViewportPlaceholderWidget::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const QPoint pos = event->position().toPoint();
    if (event->button() == Qt::RightButton) {
        m_lookHeld = true;
        m_rmbDragged = false;
        m_flyKeyUsedDuringLook = false;
        m_rmbPressPos = pos;
        m_lastMousePos = pos;
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        syncPanelSize();
        const qreal dpr = devicePixelRatioF();
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_sceneView.pickAndSelect(m_host.editorScene(), m_panel, static_cast<f32>(event->position().x() * dpr),
                                      static_cast<f32>(event->position().y() * dpr), m_host.editorState());
        }
        emit selectionChanged();
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ViewportPlaceholderWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_lookHeld) {
        const QPoint pos = event->position().toPoint();
        m_pendingMouseDx += static_cast<float>(pos.x() - m_lastMousePos.x());
        m_pendingMouseDy += static_cast<float>(pos.y() - m_lastMousePos.y());
        m_lastMousePos = pos;
        if ((pos - m_rmbPressPos).manhattanLength() >= kContextMenuClickSlop) {
            m_rmbDragged = true;
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ViewportPlaceholderWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && m_lookHeld) {
        m_lookHeld = false;
        const bool click = !m_rmbDragged && !m_flyKeyUsedDuringLook;
        event->accept();
        if (click) {
            openContextMenuAt(event->position().toPoint());
        }
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ViewportPlaceholderWidget::wheelEvent(QWheelEvent* event) {
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.f;
    if (steps == 0.f) {
        QWidget::wheelEvent(event);
        return;
    }
    ViewportCamera& cam = m_panel.camera();
    if (m_lookHeld) {
        // While flying: wheel scales the fly speed (20 % per notch).
        cam.moveSpeed = std::clamp(cam.moveSpeed * std::pow(1.2f, steps), 0.5f, 1000.f);
    } else {
        // Otherwise dolly along the view direction.
        const ecs::vec3 fwd = m_panel.forward();
        const float distance = steps * cam.moveSpeed * 0.25f;
        cam.positionX += fwd.x * distance;
        cam.positionY += fwd.y * distance;
        cam.positionZ += fwd.z * distance;
    }
    event->accept();
    update();
}

// ---- context menu ----------------------------------------------------------------------------------

ContextMenuHostGeometry ViewportPlaceholderWidget::hostGeometry() const {
    ContextMenuHostGeometry g{};
    const QWidget* top = window();
    const QPoint origin = mapTo(top, QPoint(0, 0));
    g.viewportOriginX = static_cast<f32>(origin.x());
    g.viewportOriginY = static_cast<f32>(origin.y());
    g.devicePixelRatio = static_cast<f32>(devicePixelRatioF());
    g.windowWidth = static_cast<f32>(top->width());
    g.windowHeight = static_cast<f32>(top->height());
    return g;
}

QMenu* ViewportPlaceholderWidget::openContextMenuAt(const QPoint& localPos) {
    if (m_activeMenu) {
        m_activeMenu->close();
    }
    syncPanelSize();
    const qreal dpr = devicePixelRatioF();
    m_contextMenu.setHostGeometry(hostGeometry());
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_contextMenu.openInViewport(m_host.editorScene(), m_sceneView, m_panel,
                                     static_cast<f32>(localPos.x() * dpr), static_cast<f32>(localPos.y() * dpr),
                                     m_host.editorState());
    }
    emit selectionChanged();

    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("fuseEntityContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFixedWidth(static_cast<int>(std::lround(m_contextMenu.menuWidth())));
    for (const ContextMenuItem& item : m_contextMenu.items()) {
        if (item.separatorBefore) {
            menu->addSeparator();
        }
        QAction* action = menu->addAction(QString::fromUtf8(item.label));
        action->setEnabled(item.enabled);
        const ContextMenuAction id = item.action;
        connect(action, &QAction::triggered, this, [this, id]() {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_sceneMutex);
                changed = m_contextMenu.activate(id, m_host.editorScene(), m_host.editorState(), m_host.undoStack());
            }
            if (changed) {
                emit sceneEdited();
                emit selectionChanged();
                update();
            }
        });
    }
    connect(menu, &QMenu::aboutToHide, this, [this]() { m_contextMenu.close(); });

    // The model sized the menu from its item metrics; the stylesheet matches them, but the real
    // QMenu size is authoritative (fonts / styles vary): re-place with it so flip / clamp decisions
    // use what is actually shown.
    menu->ensurePolished();
    const QSize hint = menu->sizeHint();
    if (hint.width() != std::lround(m_contextMenu.placement().width) ||
        hint.height() != std::lround(m_contextMenu.placement().height)) {
        m_contextMenu.setMenuSize(static_cast<f32>(hint.width()), static_cast<f32>(hint.height()));
    }

    m_activeMenu = menu;
    menu->popup(placementGlobalRect().topLeft());
    return menu;
}

QRect ViewportPlaceholderWidget::placementGlobalRect() const {
    const ContextMenuPlacement& p = m_contextMenu.placement();
    const QPoint topLeft = window()->mapToGlobal(
        QPoint(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y))));
    return {topLeft, QSize(static_cast<int>(std::lround(p.width)), static_cast<int>(std::lround(p.height)))};
}

// ---- size / surface hand-off -----------------------------------------------------------------------

void ViewportPlaceholderWidget::syncPanelSize() {
    const qreal dpr = devicePixelRatioF();
    m_panel.setDimensions(static_cast<u32>(std::lround(width() * dpr)), static_cast<u32>(std::lround(height() * dpr)));
}

void ViewportPlaceholderWidget::postViewportResize() {
    const QSize size = this->size();
    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }
    syncPanelSize();

    EditorCommand widthCmd;
    widthCmd.kind = CommandKind::SetProperty;
    widthCmd.propertyName = "viewport.width";
    widthCmd.propertyValue = std::to_string(m_panel.width());
    m_host.postFromUi(std::move(widthCmd));

    EditorCommand heightCmd;
    heightCmd.kind = CommandKind::SetProperty;
    heightCmd.propertyName = "viewport.height";
    heightCmd.propertyValue = std::to_string(m_panel.height());
    m_host.postFromUi(std::move(heightCmd));
}

void ViewportPlaceholderWidget::postVulkanSurfaceHandoffStub() {
    const QSize size = this->size();
    if (size.width() <= 0 || size.height() <= 0) {
        return;
    }

    postViewportResize();

    ViewportQtVulkanSurface qtSurface;
    void* surfaceHandle = reinterpret_cast<void*>(winId());
    bool qtStubSurface = true;

#if defined(FUSE_EDITOR_HAS_QT_VULKAN)
    QWindow* surfaceWindow = window() ? window()->windowHandle() : nullptr;
    if (qtSurface.initialize(surfaceWindow)) {
        surfaceHandle = qtSurface.nativeSurface();
        qtStubSurface = false;
    }
#endif

    EditorCommand stubCmd;
    stubCmd.kind = CommandKind::SetProperty;
    stubCmd.propertyName = "viewport.vk_surface_qt_stub";
    stubCmd.propertyValue = qtStubSurface ? "1" : "0";
    m_host.postFromUi(std::move(stubCmd));

    EditorCommand surfaceCmd;
    surfaceCmd.kind = CommandKind::SetProperty;
    surfaceCmd.propertyName = "viewport.vk_surface_handle";
    surfaceCmd.propertyValue = std::to_string(static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(surfaceHandle)));
    m_host.postFromUi(std::move(surfaceCmd));
}

void ViewportPlaceholderWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_windowPresentRequested) {
        // The presenting path hands over a real surface once the instance is adopted.
        postViewportResize();
        pollEmbeddedVulkanViewport();
        return;
    }
    postVulkanSurfaceHandoffStub();
}

void ViewportPlaceholderWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_vkContainer != nullptr) {
        m_vkContainer->setGeometry(rect());
    }
    postViewportResize();
}

// ---- paint -----------------------------------------------------------------------------------------

void ViewportPlaceholderWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));

    const RuntimeViewportHook& hook = m_host.runtimeViewport();
    const QString project = m_projectName.isEmpty() ? tr("(none)") : m_projectName;
    const ViewportCamera& cam = m_panel.camera();

    painter.setPen(QColor(180, 190, 210));
    painter.drawText(
        rect().adjusted(16, 16, -16, -80),
        Qt::AlignCenter,
        tr("Runtime viewport\n\nProject: %1\nSize: %2 × %3\nCamera: (%4, %5, %6) yaw %7° pitch %8°\n"
           "Runtime ticks: %9\nEmbedded: %10")
            .arg(project)
            .arg(m_panel.width())
            .arg(m_panel.height())
            .arg(cam.positionX, 0, 'f', 2)
            .arg(cam.positionY, 0, 'f', 2)
            .arg(cam.positionZ, 0, 'f', 2)
            .arg(cam.yaw, 0, 'f', 1)
            .arg(cam.pitch, 0, 'f', 1)
            .arg(hook.runtimeTickCount())
            .arg(hook.isEmbedded() ? tr("yes") : tr("no")));

    const fuse::cinematics::SeqScrubPreview& seqPreview = m_host.cinematicsSeqScrubPreview();
    const QRect overlayRect = rect().adjusted(16, rect().height() - 72, -16, -16);
    painter.fillRect(overlayRect, QColor(24, 28, 36, 220));
    painter.setPen(QColor(120, 180, 255));
    painter.drawRect(overlayRect);
    if (seqPreview.valid) {
        painter.setPen(QColor(210, 220, 235));
        painter.drawText(
            overlayRect.adjusted(8, 8, -8, -8),
            Qt::AlignLeft | Qt::AlignVCenter,
            tr("Seq overlay @ %1 ms — mount %2 yaw %3° sprite (%4, %5)")
                .arg(static_cast<qulonglong>(seqPreview.time_ms))
                .arg(QString::fromStdString(seqPreview.mount_point))
                .arg(seqPreview.mount_yaw_deg, 0, 'f', 1)
                .arg(seqPreview.sprite_x, 0, 'f', 1)
                .arg(seqPreview.sprite_y, 0, 'f', 1));
    } else {
        painter.setPen(QColor(140, 150, 165));
        painter.drawText(overlayRect.adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignVCenter,
                         tr("Seq overlay stub — scrub .seq via CinematicsSeqImport"));
    }
}

} // namespace fuse::editor::qt
