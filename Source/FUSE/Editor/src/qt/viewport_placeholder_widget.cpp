#include "viewport_placeholder_widget.hpp"
#include "viewport_qt_vulkan_surface.hpp"
#include "viewport_vulkan_window.hpp"

#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/editor/command_queue.hpp>

#include <QAction>
#include <QBitmap>
#include <QDataStream>
#include <QRegion>
#include <QColor>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
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
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fuse::editor::qt {

class ViewportOverlayWidget final : public QWidget {
public:
    explicit ViewportOverlayWidget(ViewportPlaceholderWidget* owner) : QWidget(owner), m_owner(owner) {
        setObjectName(QStringLiteral("fuseViewportOverlay"));
        setAttribute(Qt::WA_NativeWindow, true);
        setAttribute(Qt::WA_DontCreateNativeAncestors, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
    }
    QImage image;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.drawImage(rect(), image);
    }
    void forward(QMouseEvent* e) {
        QMouseEvent copy(e->type(), e->position(), e->scenePosition(), e->globalPosition(), e->button(),
                         e->buttons(), e->modifiers(), e->pointingDevice());
        QCoreApplication::sendEvent(m_owner, &copy);
        e->setAccepted(copy.isAccepted());
    }
    void mousePressEvent(QMouseEvent* e) override { forward(e); }
    void mouseMoveEvent(QMouseEvent* e) override { forward(e); }
    void mouseReleaseEvent(QMouseEvent* e) override { forward(e); }
    void wheelEvent(QWheelEvent* e) override { QCoreApplication::sendEvent(m_owner, e); }

private:
    ViewportPlaceholderWidget* m_owner;
};

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

    // Native overlay sibling exists before the Vulkan container so creating it later never
    // re-parents/re-creates the native viewport hierarchy under a live swapchain.
    if (m_overlay == nullptr) {
        m_overlay = new ViewportOverlayWidget(this);
        m_overlay->hide();
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
    setAutoFillBackground(false);
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
    setAutoFillBackground(true);
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
    const bool vkPresenting = m_vkSurfacePosted && m_vkContainer != nullptr && m_vkContainer->isVisible();
    if (!vkPresenting &&
        (m_lookHeld || m_keys.forward || m_keys.back || m_keys.left || m_keys.right || m_keys.up || m_keys.down)) {
        update();
    }
    if (vkPresenting || m_overlayForced) {
        syncOverlay();
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
        if (m_sculptMode) {
            sculptPress(event->position());
            event->accept();
            return;
        }
        if (const int hnd = colliderHandleAt_(event->position()); hnd >= 0 && m_colliderHandleEntity.valid()) {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            if (const ecs::Collider* c = m_host.editorScene().registry().get<ecs::Collider>(m_colliderHandleEntity)) {
                m_colliderBefore = *c;
                m_colliderParamsStart = c->params;
                m_colliderDragStart = event->position();
                m_colliderDrag = hnd;
                event->accept();
                return;
            }
        }
        if (gizmoPress(event->position())) {
            event->accept();
            return;
        }
        syncPanelSize();
        const qreal dpr = devicePixelRatioF();
        const bool additive = (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) != 0;
        ecs::EntityID hit = sceneIconAt_(event->position());
        if (!hit.valid()) {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            const ViewportPickResult r = m_sceneView.pick(m_host.editorScene(), m_panel, static_cast<f32>(event->position().x() * dpr),
                                                          static_cast<f32>(event->position().y() * dpr));
            hit = r.hit ? r.entity : ecs::EntityID::null();
        }
        applyClickSelection_(hit, additive);
        emit selectionChanged();
        refreshGizmo();
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
    if (m_colliderDrag >= 0) {
        colliderDragMove_(event->position());
        event->accept();
        return;
    }
    if (m_gizmo.dragging()) {
        gizmoMove(event->position());
        event->accept();
        return;
    }
    if (m_sculptMode) {
        m_hoverPos = event->position();
        m_hoverValid = true;
        if (m_sculptDragging) {
            sculptMove(event->position());
        }
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ViewportPlaceholderWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_sculptDragging) {
        sculptRelease();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_colliderDrag >= 0) {
        colliderDragRelease_();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_gizmo.dragging()) {
        gizmoRelease();
        event->accept();
        return;
    }
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

namespace {
/// One undo step for a collider handle drag (Collider has no dedicated command; this keeps the API unchanged).
class ColliderEditCommand final : public UndoCommand {
public:
    ColliderEditCommand(ecs::Registry& r, ecs::EntityID e, const ecs::Collider& before, const ecs::Collider& after)
        : m_registry(r), m_entity(e), m_before(before), m_after(after) {}
    void execute() override { apply(m_after); }
    void undo() override { apply(m_before); }
    std::string description() const override { return "Resize collider"; }

private:
    void apply(const ecs::Collider& c) {
        if (ecs::Collider* cur = m_registry.get<ecs::Collider>(m_entity)) *cur = c;
    }
    ecs::Registry& m_registry;
    ecs::EntityID m_entity;
    ecs::Collider m_before, m_after;
};
} // namespace

int ViewportPlaceholderWidget::colliderHandleAt_(const QPointF& pos) const {
    int best = -1;
    qreal bestD = 8.0 * 8.0;
    for (usize i = 0; i < m_colliderHandles.size(); ++i) {
        const QPointF d = m_colliderHandles[i].pos - pos;
        const qreal dd = d.x() * d.x() + d.y() * d.y();
        if (dd <= bestD) {
            bestD = dd;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void ViewportPlaceholderWidget::colliderDragMove_(const QPointF& pos) {
    if (m_colliderDrag < 0) return;
    const ColliderHandle& h = m_colliderHandles[static_cast<usize>(m_colliderDrag)];
    const qreal len2 = h.axisPx.x() * h.axisPx.x() + h.axisPx.y() * h.axisPx.y();
    if (len2 < 1e-6) return;
    const QPointF d = pos - m_colliderDragStart;
    const f32 units = static_cast<f32>((d.x() * h.axisPx.x() + d.y() * h.axisPx.y()) / len2);
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Collider* c = m_host.editorScene().registry().get<ecs::Collider>(m_colliderHandleEntity);
        if (c == nullptr) return;
        f32* p[3] = {&c->params.x, &c->params.y, &c->params.z};
        const f32 start[3] = {m_colliderParamsStart.x, m_colliderParamsStart.y, m_colliderParamsStart.z};
        *p[h.param] = std::max(0.01f, start[h.param] + h.sign * units);
    }
    syncOverlay(true);
    update();
}

void ViewportPlaceholderWidget::colliderDragRelease_() {
    if (m_colliderDrag < 0) return;
    m_colliderDrag = -1;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const ecs::Collider* c = registry.get<ecs::Collider>(m_colliderHandleEntity);
        if (c != nullptr && std::memcmp(&c->params, &m_colliderBefore.params, sizeof(c->params)) != 0) {
            m_host.undoStack().execute(std::make_unique<ColliderEditCommand>(registry, m_colliderHandleEntity, m_colliderBefore, *c));
        }
    }
    emit selectionChanged(); // refresh the Inspector's Collider fields
    syncOverlay(true);
    update();
}

bool ViewportPlaceholderWidget::frameSelection() {
    std::vector<ecs::vec3> points;
    f32 extent = 0.5f;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        ecs::TransformSystem::update(registry);
        std::vector<ecs::EntityID> sel = m_host.editorState().selectedEntities;
        if (sel.empty() && m_host.editorState().primarySelection.valid()) sel.push_back(m_host.editorState().primarySelection);
        for (ecs::EntityID id : sel) {
            const ecs::Transform* t = registry.alive(id) ? registry.get<ecs::Transform>(id) : nullptr;
            if (t == nullptr) continue;
            const auto& m = t->local_to_world.data;
            points.push_back({m[12], m[13], m[14], 1.f});
            const f32 sx = std::hypot(m[0], m[1], m[2]), sy = std::hypot(m[4], m[5], m[6]), sz = std::hypot(m[8], m[9], m[10]);
            extent = std::max(extent, std::max({sx, sy, sz}));
        }
    }
    if (points.empty()) return false;
    ecs::vec3 c{0.f, 0.f, 0.f, 1.f};
    for (const ecs::vec3& p : points) {
        c.x += p.x;
        c.y += p.y;
        c.z += p.z;
    }
    const f32 inv = 1.f / static_cast<f32>(points.size());
    c.x *= inv;
    c.y *= inv;
    c.z *= inv;
    f32 radius = extent;
    for (const ecs::vec3& p : points) radius = std::max(radius, std::hypot(p.x - c.x, p.y - c.y, p.z - c.z) + extent);
    ViewportCamera& cam = m_panel.camera();
    const f32 halfFov = std::max(cam.fovDeg, 10.f) * 0.5f * 3.14159265f / 180.f;
    const f32 distance = std::max(radius / std::sin(halfFov) * 1.1f, cam.nearPlane * 4.f);
    const ecs::vec3 fwd = m_panel.forward();
    cam.positionX = c.x - fwd.x * distance;
    cam.positionY = c.y - fwd.y * distance;
    cam.positionZ = c.z - fwd.z * distance;
    update();
    return true;
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
    // The embedded QWindowContainer owns on-screen pixels; painting the placeholder here only
    // confuses screen grabs (fuse_editor_qt_live_present) and can fight the native child under xvfb.
    if (m_vkSurfacePosted && m_vkContainer != nullptr && m_vkContainer->isVisible()) {
        return;
    }
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
    paintInteractionOverlay(painter);

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

namespace fuse::editor::qt {

// ---- GUI-2 transform gizmo ---------------------------------------------------------------------

void ViewportPlaceholderWidget::setTransformTool(TransformTool tool) {
    if (m_gizmo.dragging()) {
        gizmoRelease();
    }
    m_gizmo.settings().tool = tool;
    refreshGizmo();
    update();
    emit transformToolChanged();
}

void ViewportPlaceholderWidget::setTransformSpace(TransformSpace space) {
    m_gizmo.settings().space = space;
    refreshGizmo();
    update();
    emit transformToolChanged();
}

void ViewportPlaceholderWidget::setTransformSnap(bool snap) {
    m_gizmo.settings().snap = snap;
    emit transformToolChanged();
}

bool ViewportPlaceholderWidget::refreshGizmo() {
    syncPanelSize();
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    const ecs::EntityID entity = m_host.editorState().primarySelection;
    ecs::Registry& registry = m_host.editorScene().registry();
    if (entity == ecs::EntityID::null() || !registry.alive(entity) || registry.get<ecs::Transform>(entity) == nullptr) {
        m_gizmo.clearLayout();
        m_gizmoEntity = ecs::EntityID::null();
        return false;
    }
    m_gizmoEntity = entity;
    const ecs::Transform& transform = *registry.get<ecs::Transform>(entity);
    const ecs::Transform* parent =
        (transform.parent != ecs::EntityID::null() && registry.alive(transform.parent))
            ? registry.get<ecs::Transform>(transform.parent)
            : nullptr;
    if (parent != nullptr) {
        // Parent matrices may be a tick stale after an edit; refresh the hierarchy first.
        ecs::TransformSystem::update(registry);
        m_gizmo.setParentFrame(parent->local_to_world, parent->world_to_local);
    } else {
        m_gizmo.clearParentFrame();
    }
    return m_gizmo.layout(m_panel, transform);
}

bool ViewportPlaceholderWidget::gizmoPress(const QPointF& pos) {
    if (m_gizmo.settings().tool == TransformTool::Select || !refreshGizmo()) {
        return false;
    }
    const qreal dpr = devicePixelRatioF();
    const f32 px = static_cast<f32>(pos.x() * dpr), py = static_cast<f32>(pos.y() * dpr);
    const int axis = m_gizmo.hitTest(px, py);
    if (axis < 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    ecs::Registry& registry = m_host.editorScene().registry();
    const ecs::Transform* transform = registry.get<ecs::Transform>(m_gizmoEntity);
    if (transform == nullptr || !m_gizmo.beginDrag(axis, px, py, *transform)) {
        return false;
    }
    m_groupStart.clear();
    {
        const std::vector<ecs::EntityID>& selection = m_host.editorState().selectedEntities;
        auto selectedAncestor = [&](ecs::EntityID id) {
            for (int guard = 0; guard < 64; ++guard) {
                const ecs::Transform* t = registry.get<ecs::Transform>(id);
                if (t == nullptr || !t->parent.valid()) return false;
                id = t->parent;
                if (std::find(selection.begin(), selection.end(), id) != selection.end()) return true;
            }
            return false;
        };
        for (ecs::EntityID other : selection) {
            if (other == m_gizmoEntity || !registry.alive(other) || selectedAncestor(other)) continue;
            const ecs::Transform* ot = registry.get<ecs::Transform>(other);
            if (ot == nullptr) continue;
            m_groupStart.push_back({other, TransformCommand::capture(*ot), ot->parent == transform->parent});
        }
    }
    static const char* kNames[] = {"Move", "Rotate", "Scale"};
    const int tool = static_cast<int>(m_gizmo.settings().tool) - 1;
    m_host.undoStack().beginMacro(std::string(kNames[std::clamp(tool, 0, 2)]) + " entity");
    m_gizmoMacroOpen = true;
    return true;
}

void ViewportPlaceholderWidget::gizmoMove(const QPointF& pos) {
    if (!m_gizmo.dragging()) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const TransformCommand::State after =
        m_gizmo.dragTo(static_cast<f32>(pos.x() * dpr), static_cast<f32>(pos.y() * dpr));
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const ecs::Transform* transform = registry.get<ecs::Transform>(m_gizmoEntity);
        if (transform == nullptr) {
            return;
        }
        const TransformCommand::State before = TransformCommand::capture(*transform);
        m_host.undoStack().execute(std::make_unique<TransformCommand>(registry, m_gizmoEntity, before, after));
        // Multi-select: the drag carries over to every other selected root (children follow their parents).
        const TransformCommand::State& start = m_gizmo.dragStart();
        for (const GroupStart& g : m_groupStart) {
            const ecs::Transform* ot = registry.get<ecs::Transform>(g.id);
            if (ot == nullptr) continue;
            const TransformCommand::State ob = TransformCommand::capture(*ot);
            const TransformCommand::State oa =
                TransformGizmo::applyGroupDelta(m_gizmo.settings().tool, start, after, g.state, g.sameFrame);
            m_host.undoStack().execute(std::make_unique<TransformCommand>(registry, g.id, ob, oa));
        }
    }
    emit sceneEdited();
    update();
}

void ViewportPlaceholderWidget::gizmoRelease() {
    m_gizmo.endDrag();
    m_groupStart.clear();
    if (m_gizmoMacroOpen) {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_host.undoStack().endMacro();
        m_gizmoMacroOpen = false;
    }
    refreshGizmo();
    emit sceneEdited();
    update();
}

void ViewportPlaceholderWidget::applyClickSelection_(ecs::EntityID hit, bool additive) {
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    EditorState& state = m_host.editorState();
    if (!additive) {
        state.selectedEntities.clear();
        state.primarySelection = ecs::EntityID::null();
        if (hit.valid()) {
            state.selectedEntities.push_back(hit);
            state.primarySelection = hit;
        }
        return;
    }
    if (!hit.valid()) return;
    auto& sel = state.selectedEntities;
    const auto it = std::find(sel.begin(), sel.end(), hit);
    if (it != sel.end()) {
        sel.erase(it);
        if (state.primarySelection == hit) state.primarySelection = sel.empty() ? ecs::EntityID::null() : sel.back();
    } else {
        sel.push_back(hit);
        state.primarySelection = hit;
    }
}

ecs::EntityID ViewportPlaceholderWidget::sceneIconAt_(const QPointF& pos) const {
    if (!m_showSceneIcons) return ecs::EntityID::null();
    for (const SceneIcon& icon : m_sceneIcons) {
        if (std::hypot(icon.pos.x() - pos.x(), icon.pos.y() - pos.y()) <= 11.0) return icon.id;
    }
    return ecs::EntityID::null();
}

namespace {
ecs::vec3 rotateByQuat(const ecs::quat& q, f32 x, f32 y, f32 z) {
    // v' = v + 2w(q x v) + 2 q x (q x v)
    const f32 cx = q.y * z - q.z * y, cy = q.z * x - q.x * z, cz = q.x * y - q.y * x;
    const f32 ccx = q.y * cz - q.z * cy, ccy = q.z * cx - q.x * cz, ccz = q.x * cy - q.y * cx;
    return {x + 2.f * (q.w * cx + ccx), y + 2.f * (q.w * cy + ccy), z + 2.f * (q.w * cz + ccz), 0.f};
}
} // namespace

void ViewportPlaceholderWidget::paintSceneIcons_(QPainter& painter) {
    m_sceneIcons.clear();
    if (!m_showSceneIcons) return;
    enum Kind { Point, Spot, Directional, Cam };
    struct Item {
        ecs::EntityID id;
        Kind kind;
        ecs::vec3 pos;
        ecs::vec3 fwd;
        QColor color;
        bool selected;
    };
    std::vector<Item> items;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const auto& sel = m_host.editorState().selectedEntities;
        registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
            Kind kind;
            ecs::vec3 c{1.f, 1.f, 1.f, 0.f};
            if (const auto* l = registry.get<ecs::PointLight>(id)) { kind = Point; c = l->color; }
            else if (const auto* s = registry.get<ecs::SpotLight>(id)) { kind = Spot; c = s->color; }
            else if (const auto* d = registry.get<ecs::DirectionalLight>(id)) { kind = Directional; c = d->color; }
            else if (registry.get<ecs::Camera>(id) != nullptr) { kind = Cam; c = {0.8f, 0.85f, 0.95f, 0.f}; }
            else return;
            const bool hasParent = t.parent.valid();
            const ecs::vec3 p = hasParent ? ecs::vec3{t.local_to_world.data[12], t.local_to_world.data[13], t.local_to_world.data[14], 1.f}
                                          : ecs::vec3{t.position.x, t.position.y, t.position.z, 1.f};
            const auto toByte = [](f32 v) { return std::clamp(static_cast<int>(v * 255.f), 60, 255); };
            items.push_back({id, kind, p, rotateByQuat(t.rotation, 0.f, 0.f, -1.f),
                             QColor(toByte(c.x), toByte(c.y), toByte(c.z)),
                             std::find(sel.begin(), sel.end(), id) != sel.end()});
        });
    }
    const qreal inv = 1.0 / devicePixelRatioF();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const Item& it : items) {
        f32 sx = 0.f, sy = 0.f;
        if (!m_panel.worldToScreen(it.pos, sx, sy)) continue;
        const QPointF p(sx * inv, sy * inv);
        if (!rect().adjusted(-12, -12, 12, 12).contains(p.toPoint())) continue;
        m_sceneIcons.push_back({it.id, p});
        const QColor line = it.selected ? QColor(255, 200, 40) : it.color;
        painter.setPen(QPen(QColor(0, 0, 0, 160), 4.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(p, 8.0, 8.0);
        painter.setPen(QPen(line, it.selected ? 2.5 : 1.8));
        // Screen-space direction (spot / directional / camera).
        QPointF dir(0, 0);
        if (it.kind != Point) {
            const ecs::vec3 tip{it.pos.x + it.fwd.x, it.pos.y + it.fwd.y, it.pos.z + it.fwd.z, 1.f};
            f32 tx = 0.f, ty = 0.f;
            if (m_panel.worldToScreen(tip, tx, ty)) {
                const QPointF d(tx * inv - p.x(), ty * inv - p.y());
                const qreal len = std::hypot(d.x(), d.y());
                if (len > 0.5) dir = d / len;
            }
        }
        switch (it.kind) {
        case Point:
            painter.setBrush(it.color);
            painter.drawEllipse(p, 5.0, 5.0);
            for (int k = 0; k < 8; ++k) {
                const qreal a = k * M_PI / 4.0;
                const QPointF u(std::cos(a), std::sin(a));
                painter.drawLine(p + u * 8.0, p + u * 12.0);
            }
            break;
        case Spot: {
            const QPointF n(-dir.y(), dir.x());
            painter.drawPolygon(QPolygonF({p, p + dir * 22.0 + n * 9.0, p + dir * 22.0 - n * 9.0}));
            painter.setBrush(it.color);
            painter.drawEllipse(p, 4.0, 4.0);
            break;
        }
        case Directional:
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(p, 6.0, 6.0);
            painter.drawLine(p, p + dir * 26.0);
            painter.drawLine(p + dir * 26.0, p + dir * 20.0 + QPointF(-dir.y(), dir.x()) * 4.0);
            painter.drawLine(p + dir * 26.0, p + dir * 20.0 - QPointF(-dir.y(), dir.x()) * 4.0);
            break;
        case Cam: {
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(p.x() - 7.0, p.y() - 5.0, 10.0, 10.0));
            const QPointF n(-dir.y(), dir.x());
            const QPointF f = dir.isNull() ? QPointF(1, 0) : dir, nn = dir.isNull() ? QPointF(0, 1) : n;
            painter.drawPolygon(QPolygonF({p + f * 5.0, p + f * 18.0 + nn * 8.0, p + f * 18.0 - nn * 8.0}));
            break;
        }
        }
    }
    painter.restore();
}

void ViewportPlaceholderWidget::collectColliderLines_() {
    m_colliderLines.clear();
    m_colliderSelected.clear();
    if (m_colliderDrag < 0) {
        m_colliderHandles.clear();
        m_colliderHandleEntity = ecs::EntityID::null();
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const ecs::EntityID id = m_host.editorState().primarySelection;
        const ecs::Transform* t = id.valid() ? registry.get<ecs::Transform>(id) : nullptr;
        const ecs::Collider* c = id.valid() ? registry.get<ecs::Collider>(id) : nullptr;
        if (t != nullptr && c != nullptr) {
            const auto& m = t->local_to_world.data;
            const qreal inv = 1.0 / devicePixelRatioF();
            const auto add = [&](int axis, f32 dist, int param, f32 sign) {
                f32 p[3] = {0.f, 0.f, 0.f};
                p[axis] = dist;
                f32 q[3] = {p[0], p[1], p[2]};
                q[axis] += (dist >= 0.f ? 1.f : -1.f);
                const auto w = [&](const f32* v) {
                    return ecs::vec3{m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12], m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
                                     m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14], 1.f};
                };
                f32 ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
                if (!m_panel.worldToScreen(w(p), ax, ay) || !m_panel.worldToScreen(w(q), bx, by)) return;
                // axisPx: pixels per local unit moving outward; param changes by `sign` per outward unit.
                m_colliderHandles.push_back({QPointF(ax * inv, ay * inv), QPointF((bx - ax) * inv, (by - ay) * inv), param, sign});
            };
            switch (c->shape) {
            case ecs::Collider::Sphere:
                for (int a = 0; a < 3; ++a) add(a, c->params.x, 0, 1.f);
                break;
            case ecs::Collider::Box: {
                const f32 h[3] = {c->params.x, c->params.y, c->params.z};
                for (int a = 0; a < 3; ++a) {
                    add(a, h[a], a, 1.f);
                    add(a, -h[a], a, 1.f);
                }
                break;
            }
            case ecs::Collider::Capsule:
                add(0, c->params.x, 0, 1.f);
                add(2, c->params.x, 0, 1.f);
                add(1, c->params.y + c->params.x, 1, 1.f);
                add(1, -(c->params.y + c->params.x), 1, 1.f);
                break;
            default:
                break;
            }
            if (!m_colliderHandles.empty()) m_colliderHandleEntity = id;
        }
    }
    struct Seg {
        ecs::vec3 a, b;
        u8 selected;
    };
    std::vector<Seg> segs;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const auto& sel = m_host.editorState().selectedEntities;
        registry.each<ecs::Transform, ecs::Collider>([&](ecs::EntityID id, ecs::Transform& t, ecs::Collider& c) {
            const bool selected = std::find(sel.begin(), sel.end(), id) != sel.end();
            if (!selected && !m_showAllColliders) return;
            const auto& m = t.local_to_world.data;
            const auto xf = [&](f32 x, f32 y, f32 z) {
                return ecs::vec3{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                                 m[2] * x + m[6] * y + m[10] * z + m[14], 1.f};
            };
            const auto line = [&](f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz) {
                segs.push_back({xf(ax, ay, az), xf(bx, by, bz), selected});
            };
            constexpr int kSeg = 24;
            constexpr f32 kTau = 6.2831853f;
            // Circle of radius r around axis (0 = X, 1 = Y, 2 = Z) at offset `o` along that axis; `half`
            // draws only the half with a positive (sign) component along `up` (capsule caps).
            const auto circle = [&](int axis, f32 r, f32 o, int up = -1, f32 sign = 1.f) {
                for (int i = 0; i < kSeg; ++i) {
                    const f32 a0 = kTau * i / kSeg, a1 = kTau * (i + 1) / kSeg;
                    f32 p0[3], p1[3];
                    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                    p0[axis] = o;
                    p1[axis] = o;
                    p0[u] = r * std::cos(a0);
                    p0[v] = r * std::sin(a0);
                    p1[u] = r * std::cos(a1);
                    p1[v] = r * std::sin(a1);
                    if (up >= 0 && (p0[up] * sign < -1e-4f || p1[up] * sign < -1e-4f)) continue;
                    if (up >= 0) {
                        p0[up] += sign * o;
                        p1[up] += sign * o;
                    }
                    line(p0[0], p0[1], p0[2], p1[0], p1[1], p1[2]);
                }
            };
            switch (c.shape) {
            case ecs::Collider::Sphere: {
                const f32 r = c.params.x;
                for (int a = 0; a < 3; ++a) circle(a, r, 0.f);
                break;
            }
            case ecs::Collider::Box: {
                const f32 h[3] = {c.params.x, c.params.y, c.params.z};
                for (int a = 0; a < 3; ++a) {
                    const int u = (a + 1) % 3, v = (a + 2) % 3;
                    for (int s = 0; s < 4; ++s) {
                        f32 p0[3], p1[3];
                        p0[a] = -h[a];
                        p1[a] = h[a];
                        p0[u] = p1[u] = (s & 1) ? h[u] : -h[u];
                        p0[v] = p1[v] = (s & 2) ? h[v] : -h[v];
                        line(p0[0], p0[1], p0[2], p1[0], p1[1], p1[2]);
                    }
                }
                break;
            }
            case ecs::Collider::Capsule: {
                const f32 r = c.params.x, hh = c.params.y;
                circle(1, r, hh);
                circle(1, r, -hh);
                for (int s = 0; s < 4; ++s) {
                    const f32 x = (s == 0) ? r : (s == 1) ? -r : 0.f, z = (s == 2) ? r : (s == 3) ? -r : 0.f;
                    line(x, -hh, z, x, hh, z);
                }
                // Cap arcs: circles around X and Z at the origin, keeping the +Y / -Y halves, shifted by +/- hh.
                circle(0, r, 0.f, 1, 1.f);
                circle(2, r, 0.f, 1, 1.f);
                circle(0, r, 0.f, 1, -1.f);
                circle(2, r, 0.f, 1, -1.f);
                break;
            }
            default:
                break;
            }
        });
        for (usize k = 0; k + 6u <= m_navSegs.size(); k += 6u) {
            segs.push_back({ecs::vec3{m_navSegs[k], m_navSegs[k + 1], m_navSegs[k + 2], 1.f},
                            ecs::vec3{m_navSegs[k + 3], m_navSegs[k + 4], m_navSegs[k + 5], 1.f}, u8{2}});
        }
        if (!editorViewModeWire(editorViewMode())) {
            m_wireCache.clear();
        } else {
            // Real triangle edges, cached per engine mesh id (meshes are immutable once registered).
            usize budget = kMaxWireSegments;
            const auto wire = [&](ecs::EntityID id, const ecs::Transform& t, u32 engineId, const f32 scale[3]) {
                if (budget == 0) return;
                auto it = m_wireCache.find(engineId);
                if (it == m_wireCache.end() || it->second.empty()) {
                    std::vector<f32> edges;
                    m_host.runtimeViewport().copyMeshWireframe(engineId, edges);
                    if (edges.empty()) return;
                    it = m_wireCache.insert_or_assign(engineId, std::move(edges)).first;
                }
                const bool selected = std::find(sel.begin(), sel.end(), id) != sel.end();
                const auto& m = t.local_to_world.data;
                const auto xf = [&](const f32* q) {
                    const f32 p[3] = {q[0] * scale[0], q[1] * scale[1], q[2] * scale[2]};
                    return ecs::vec3{m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                                     m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14], 1.f};
                };
                const std::vector<f32>& e = it->second;
                for (usize k = 0; k + 6u <= e.size() && budget > 0; k += 6u, --budget) {
                    segs.push_back({xf(&e[k]), xf(&e[k + 3]), u8(selected ? 4 : 3)});
                }
            };
            static constexpr f32 kOne[3] = {1.f, 1.f, 1.f};
            registry.each<ecs::Transform, ecs::Mesh>([&](ecs::EntityID id, ecs::Transform& t, ecs::Mesh& mesh) {
                if (mesh.visible && mesh.vertex_buffer.isValid()) wire(id, t, mesh.vertex_buffer.index(), kOne);
            });
            // SDF objects render through proxy meshes (GpuSceneEcsExtractorDesc defaults: 0 = cube, 1 = sphere).
            registry.each<ecs::Transform, ecs::SDFObject>([&](ecs::EntityID id, ecs::Transform& t, ecs::SDFObject& sdf) {
                if (!sdf.visible || sdf.op == ecs::SDFCsgOp::Subtract) return;
                const bool box = sdf.type == ecs::SDFPrimitive::Box;
                const f32 r = sdf.params.x > 0.f ? sdf.params.x : 1.f;
                const f32 sc[3] = {box ? sdf.params.x : r, box ? sdf.params.y : r, box ? sdf.params.z : r};
                wire(id, t, box ? 0u : 1u, sc);
            });
        }
        if (m_showMeshBounds) {
            registry.each<ecs::Transform, ecs::Mesh>([&](ecs::EntityID id, ecs::Transform& t, ecs::Mesh& mesh) {
                const bool selected = std::find(sel.begin(), sel.end(), id) != sel.end();
                const auto& m = t.local_to_world.data;
                const auto xf = [&](f32 x, f32 y, f32 z) {
                    return ecs::vec3{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                                     m[2] * x + m[6] * y + m[10] * z + m[14], 1.f};
                };
                const f32 lo[3] = {mesh.aabb_min.x, mesh.aabb_min.y, mesh.aabb_min.z};
                const f32 hi[3] = {mesh.aabb_max.x, mesh.aabb_max.y, mesh.aabb_max.z};
                if (!(hi[0] > lo[0] || hi[1] > lo[1] || hi[2] > lo[2])) return;
                for (int a = 0; a < 3; ++a) {
                    const int u = (a + 1) % 3, v = (a + 2) % 3;
                    for (int s = 0; s < 4; ++s) {
                        f32 p0[3], p1[3];
                        p0[a] = lo[a];
                        p1[a] = hi[a];
                        p0[u] = p1[u] = (s & 1) ? hi[u] : lo[u];
                        p0[v] = p1[v] = (s & 2) ? hi[v] : lo[v];
                        segs.push_back({xf(p0[0], p0[1], p0[2]), xf(p1[0], p1[1], p1[2]), selected});
                    }
                }
            });
        }
    }
    const qreal inv = 1.0 / devicePixelRatioF();
    for (const Seg& s : segs) {
        f32 ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
        if (!m_panel.worldToScreen(s.a, ax, ay) || !m_panel.worldToScreen(s.b, bx, by)) continue;
        m_colliderLines.emplace_back(ax * inv, ay * inv, bx * inv, by * inv);
        m_colliderSelected.push_back(s.selected);
    }
}

void ViewportPlaceholderWidget::paintColliders_(QPainter& painter) {
    if (!m_colliderHandles.empty()) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (usize i = 0; i < m_colliderHandles.size(); ++i) {
            const bool active = static_cast<int>(i) == m_colliderDrag;
            painter.setPen(QPen(QColor(20, 40, 20), 1.0));
            painter.setBrush(active ? QColor(255, 230, 90) : QColor(120, 255, 140));
            painter.drawRect(QRectF(m_colliderHandles[i].pos - QPointF(4.5, 4.5), QSizeF(9.0, 9.0)));
        }
        painter.restore();
    }
    if (m_colliderLines.empty()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (usize i = 0; i < m_colliderLines.size(); ++i) {
        const u8 kind = m_colliderSelected[i];
        if (kind >= 3) {
            painter.setPen(QPen(kind == 4 ? QColor(255, 200, 80, 220) : QColor(230, 230, 240, 120), 1.0));
            painter.drawLine(m_colliderLines[i]);
            continue;
        }
        painter.setPen(QPen(kind == 2 ? QColor(70, 200, 255, 170) : kind == 1 ? QColor(120, 255, 140, 230) : QColor(90, 200, 110, 150), 1.4));
        painter.drawLine(m_colliderLines[i]);
    }
    painter.restore();
}

void ViewportPlaceholderWidget::paintGizmo_(QPainter& painter) {
    if (!m_inOverlayPaint) collectColliderLines_();
    paintColliders_(painter);
    paintSceneIcons_(painter);
    if (!m_gizmo.dragging()) {
        refreshGizmo();
    }
    if (!m_gizmo.visible()) {
        return;
    }
    static const QColor kAxis[3] = {QColor(235, 80, 80), QColor(110, 210, 90), QColor(80, 140, 245)};
    const qreal inv = 1.0 / devicePixelRatioF();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int a = 0; a < 3; ++a) {
        const GizmoHandle2D& h = m_gizmo.handles()[a];
        if (!h.visible) {
            continue;
        }
        const bool hot = m_gizmo.dragAxis() == a;
        const QPointF p0(h.x0 * inv, h.y0 * inv), p1(h.x1 * inv, h.y1 * inv);
        QColor c = hot ? QColor(255, 220, 60) : kAxis[a];
        painter.setPen(QPen(c, hot ? 4.0 : 3.0));
        painter.drawLine(p0, p1);
        painter.setBrush(c);
        switch (m_gizmo.settings().tool) {
        case TransformTool::Translate: {
            const QPointF d = p1 - p0;
            const qreal len = std::hypot(d.x(), d.y());
            if (len > 1.0) {
                const QPointF u = d / len, n(-u.y(), u.x());
                const QPointF tip = p1 + u * 10.0;
                painter.drawPolygon(QPolygonF({tip, p1 + n * 5.0, p1 - n * 5.0}));
            }
            break;
        }
        case TransformTool::Rotate:
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(p1, 7.0, 7.0);
            break;
        case TransformTool::Scale:
            painter.drawRect(QRectF(p1.x() - 5.0, p1.y() - 5.0, 10.0, 10.0));
            break;
        case TransformTool::Select:
            break;
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(240, 240, 240));
    const GizmoHandle2D& o = m_gizmo.handles()[0];
    painter.drawEllipse(QPointF(o.x0 * inv, o.y0 * inv), 4.0, 4.0);
    painter.restore();
}

// ---- GUI-4 sculpt mode ---------------------------------------------------------------------------

void ViewportPlaceholderWidget::setSculptMode(bool on) {
    if (!on && m_sculptDragging) {
        sculptRelease();
    }
    m_sculptMode = on && m_sculptPanel != nullptr;
    setMouseTracking(m_sculptMode);
    m_hoverValid = false;
    setCursor(m_sculptMode ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

bool ViewportPlaceholderWidget::sculptHit_(const QPointF& pos, ecs::vec3& point, ecs::vec3& normal) {
    syncPanelSize();
    const qreal dpr = devicePixelRatioF();
    const ViewportRay ray = m_panel.screenRay(static_cast<f32>(pos.x() * dpr), static_cast<f32>(pos.y() * dpr));
    normal = ecs::vec3{-ray.direction.x, -ray.direction.y, -ray.direction.z, 0.f};
    const ViewportPickResult hit = m_sceneView.pickRay(m_host.editorScene(), ray);
    if (hit.hit) {
        point = hit.point;
        return true;
    }
    // Miss: build on the ground plane.
    if (std::fabs(ray.direction.y) < 1e-5f) {
        return false;
    }
    const f32 t = -ray.origin.y / ray.direction.y;
    if (t <= 0.f) {
        return false;
    }
    point = ecs::vec3{ray.origin.x + ray.direction.x * t, 0.f, ray.origin.z + ray.direction.z * t, 1.f};
    normal = ecs::vec3{0.f, 1.f, 0.f, 0.f};
    return true;
}

bool ViewportPlaceholderWidget::sculptAt_(const QPointF& pos) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::vec3 point{}, normal{};
        if (!sculptHit_(pos, point, normal)) {
            return false;
        }
        changed = m_sculptPanel->applyBrushStroke(point, normal, m_host.editorScene(), m_host.undoStack());
    }
    if (changed) {
        emit sceneEdited();
        update();
    }
    return changed;
}

bool ViewportPlaceholderWidget::sculptPress(const QPointF& pos) {
    if (!m_sculptMode || m_sculptPanel == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_host.undoStack().beginMacro("Sculpt stroke");
    }
    m_sculptDragging = true;
    m_sculptPanel->endStroke(); // first sample of a drag always lands
    return sculptAt_(pos);
}

void ViewportPlaceholderWidget::sculptMove(const QPointF& pos) {
    if (m_sculptDragging) {
        sculptAt_(pos);
    }
}

void ViewportPlaceholderWidget::sculptRelease() {
    if (!m_sculptDragging) {
        return;
    }
    m_sculptDragging = false;
    if (m_sculptPanel != nullptr) {
        m_sculptPanel->endStroke();
    }
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_host.undoStack().endMacro();
    }
    emit sceneEdited();
    update();
}

void ViewportPlaceholderWidget::paintBrush_(QPainter& painter) {
    if (!m_hoverValid || m_sculptPanel == nullptr) {
        return;
    }
    ecs::vec3 point{}, normal{};
    f32 cx = 0.f, cy = 0.f, ex = 0.f, ey = 0.f;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        if (!sculptHit_(m_hoverPos, point, normal)) {
            return;
        }
    }
    const ecs::vec3 r = m_panel.right();
    const f32 rad = m_sculptPanel->brush().radius;
    const ecs::vec3 edge{point.x + r.x * rad, point.y + r.y * rad, point.z + r.z * rad, 1.f};
    if (!m_panel.worldToScreen(point, cx, cy) || !m_panel.worldToScreen(edge, ex, ey)) {
        return;
    }
    const qreal inv = 1.0 / devicePixelRatioF();
    const qreal radius = std::hypot(ex - cx, ey - cy) * inv;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool carve = m_sculptPanel->brush().op == SdfSculptPanel::BrushOp::Subtract;
    painter.setPen(QPen(carve ? QColor(240, 110, 90) : QColor(120, 200, 255), 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(cx * inv, cy * inv), radius, radius);
    painter.drawEllipse(QPointF(cx * inv, cy * inv), 2.0, 2.0);
    painter.restore();
}

} // namespace fuse::editor::qt

namespace fuse::editor::qt {

// ---- overlay over the embedded Vulkan window ------------------------------------------------------

/// Native child above the Vulkan container; masked to the drawn pixels and forwarding mouse input
/// to the viewport (so handle drags work the same as on the software path).

void ViewportPlaceholderWidget::paintInteractionOverlay(QPainter& painter) {
    if (m_sculptMode) {
        paintBrush_(painter);
    } else {
        paintGizmo_(painter);
    }
}

bool ViewportPlaceholderWidget::overlayActive() const { return m_overlay != nullptr && m_overlay->isVisible(); }

QWidget* ViewportPlaceholderWidget::overlayWidget() const { return m_overlay; }

void ViewportPlaceholderWidget::setOverlayForced(bool on) {
    m_overlayForced = on;
    syncOverlay(true);
}

void ViewportPlaceholderWidget::syncOverlay(bool force) {
    static const bool kOverlayDisabled = qEnvironmentVariableIsSet("FUSE_EDITOR_NO_VK_OVERLAY");
    const bool vkPresenting = !kOverlayDisabled && m_vkSurfacePosted && m_vkContainer != nullptr && m_vkContainer->isVisible();
    if (!vkPresenting && !m_overlayForced) {
        if (m_overlay != nullptr) {
            m_overlay->hide();
        }
        m_overlayImage = QImage();
        m_overlaySignature.clear();
        return;
    }
    if (!m_gizmo.dragging() && !m_sculptMode) {
        refreshGizmo();
    }
    // Change signature: tool state + projected handles (gizmo) or hover position (brush).
    QByteArray sig;
    {
        QDataStream ds(&sig, QIODevice::WriteOnly);
        ds << width() << height() << m_sculptMode << static_cast<int>(m_gizmo.settings().tool)
           << m_gizmo.visible() << m_gizmo.dragAxis();
        collectColliderLines_();
        ds << static_cast<quint32>(m_colliderLines.size());
        for (const QLineF& l : m_colliderLines) ds << l;
        if (m_sculptMode) {
            ds << m_hoverValid << m_hoverPos << (m_sculptPanel ? m_sculptPanel->brush().radius : 0.f)
               << m_panel.camera().positionX << m_panel.camera().positionY << m_panel.camera().positionZ
               << m_panel.camera().yaw << m_panel.camera().pitch;
        } else if (m_gizmo.visible()) {
            for (const GizmoHandle2D& h : m_gizmo.handles()) {
                ds << h.visible << h.x0 << h.y0 << h.x1 << h.y1;
            }
        }
    }
    if (!force && sig == m_overlaySignature) {
        return;
    }
    m_overlaySignature = sig;

    const qreal dpr = devicePixelRatioF();
    QImage img(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        m_inOverlayPaint = true;
        paintInteractionOverlay(p);
        m_inOverlayPaint = false;
    }
    m_overlayImage = img;
    if (m_overlay == nullptr) {
        m_overlay = new ViewportOverlayWidget(this);
    }
    // Shape to the opaque-ish pixels, grown by one px so the antialiased edge isn't clipped hard.
    QRegion region(QBitmap::fromImage(
        img.scaled(size(), Qt::IgnoreAspectRatio, Qt::FastTransformation).createAlphaMask(Qt::ThresholdAlphaDither)));
    if (region.isEmpty()) {
        m_overlay->hide();
        return;
    }
    m_overlay->image = img;
    m_overlay->setGeometry(rect());
    m_overlay->setMask(region);
    m_overlay->show();
    m_overlay->raise();
    m_overlay->update();
}

} // namespace fuse::editor::qt
