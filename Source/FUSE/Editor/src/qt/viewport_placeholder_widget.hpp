#pragma once

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/editor/viewport_scene_view.hpp>

#include <QElapsedTimer>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <mutex>


namespace fuse::editor::qt {

/// Editor viewport (B6.3 Qt host): drives the FUSE `ViewportPanel` fly camera from Qt key / mouse
/// events, picks and opens the `EntityContextMenu` through `ViewportSceneView`, and posts resize /
/// Vulkan surface hand-off commands to the game thread (U6).
///
/// Input model: held keys are level-triggered state sampled once per frame, so a key press is
/// visible in the very next `advanceFrame` (no event-queue lag) and auto-repeat events are
/// ignored (they would otherwise toggle the state and stutter the camera). Motion per frame is
/// `moveSpeed * dt`, with `dt` measured on a monotonic clock and clamped to `kMaxFrameDt`.
class ViewportPlaceholderWidget final : public QWidget {
    Q_OBJECT

public:
    static constexpr float kMaxFrameDt = 0.1f;
    static constexpr int kFrameIntervalMs = 16; ///< ~60 Hz frame pump
    /// RMB travel (logical px) below which a right-click opens the context menu instead of flying.
    static constexpr int kContextMenuClickSlop = 4;

    ViewportPlaceholderWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent = nullptr);
    ~ViewportPlaceholderWidget() override;

    void setProjectLabel(const QString& projectName);

    ViewportPanel& panel() { return m_panel; }
    const ViewportPanel& panel() const { return m_panel; }
    ViewportSceneView& sceneView() { return m_sceneView; }
    EntityContextMenu& contextMenuModel() { return m_contextMenu; }
    const EntityContextMenu& contextMenuModel() const { return m_contextMenu; }

    /// One viewport frame: sample held keys + accumulated mouse travel into the camera.
    void advanceFrame(float dt);
    [[nodiscard]] quint64 frameCount() const { return m_frameCount; }

    /// Start / stop the ~60 Hz frame pump (QTimer, precise; dt from a monotonic clock).
    void setFramePumpEnabled(bool enabled);
    [[nodiscard]] bool framePumpEnabled() const { return m_frameTimer.isActive(); }

    /// Open the entity context menu for a right-click at `localPos` (widget logical px) and
    /// `QMenu::popup` it at `EntityContextMenu::placement()`. Returns the open menu.
    QMenu* openContextMenuAt(const QPoint& localPos);
    [[nodiscard]] QMenu* activeContextMenu() const { return m_activeMenu.data(); }
    /// Window-logical rectangle of the placement, mapped to global screen coordinates.
    [[nodiscard]] QRect placementGlobalRect() const;

signals:
    /// ECS scene changed from the viewport (context menu create / delete, pick selection).
    void sceneEdited();
    void selectionChanged();
    /// Emitted at the end of every `advanceFrame` with the (clamped) frame delta in seconds.
    void frameAdvanced(float dt);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct HeldKeys {
        bool forward = false;
        bool back = false;
        bool left = false;
        bool right = false;
        bool up = false;
        bool down = false;
        bool fast = false;
    };

    bool setKey(int key, bool down);
    void syncPanelSize();
    ContextMenuHostGeometry hostGeometry() const;
    void postViewportResize();
    void postVulkanSurfaceHandoffStub();
    void onFrameTimer();

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    ViewportPanel m_panel;
    ViewportSceneView m_sceneView;
    EntityContextMenu m_contextMenu;
    QPointer<QMenu> m_activeMenu;
    QString m_projectName;
    HeldKeys m_keys;
    QTimer m_frameTimer;
    QElapsedTimer m_frameClock;
    QPoint m_lastMousePos;
    QPoint m_rmbPressPos;
    float m_pendingMouseDx = 0.f;
    float m_pendingMouseDy = 0.f;
    quint64 m_frameCount = 0;
    bool m_lookHeld = false;
    bool m_rmbDragged = false;
    bool m_flyKeyUsedDuringLook = false;
};

} // namespace fuse::editor::qt
