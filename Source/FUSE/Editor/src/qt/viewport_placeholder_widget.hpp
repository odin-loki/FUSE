#pragma once

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>
#include <fuse/editor/transform_gizmo.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/editor/viewport_scene_view.hpp>

#include <QElapsedTimer>
#include <QImage>
#include <QMenu>
#include <QPointer>
#include <algorithm>
#include <unordered_map>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <mutex>

class QVulkanInstance;
class QPainter;

namespace fuse::editor::qt {

class ViewportVulkanWindow;
class ViewportOverlayWidget;

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

    /// GUI-2 transform gizmo (tool / space / snap live in its settings).
    TransformGizmo& gizmo() { return m_gizmo; }
    const TransformGizmo& gizmo() const { return m_gizmo; }
    void setTransformTool(TransformTool tool);
    void setTransformSpace(TransformSpace space);
    void setTransformSnap(bool snap);
    /// Re-project handles for the primary selection; false when nothing is drawn.
    bool refreshGizmo();
    /// Drag helpers in logical widget pixels (also used by tests): press on a handle, move, release.
    bool gizmoPress(const QPointF& pos);
    void gizmoMove(const QPointF& pos);
    void gizmoRelease();

    /// GUI-4 sculpt mode: left-drag applies `panel`'s brush at the surface under the cursor (or
    /// the y = 0 ground plane on a miss); one drag is one UndoStack step.
    void setSculptPanel(SdfSculptPanel* panel) { m_sculptPanel = panel; }
    void setSculptMode(bool on);
    [[nodiscard]] bool sculptMode() const { return m_sculptMode; }
    bool sculptPress(const QPointF& pos);
    void sculptMove(const QPointF& pos);
    void sculptRelease();
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

    // ---- embedded Vulkan viewport (presenting path) ----------------------------------------------
    /// Ask the game thread for a VkInstance with this platform's WSI extensions; once published it
    /// is adopted (`QVulkanInstance::setVkInstance`), a Vulkan child window is embedded over the
    /// widget and its surface handed back for a real swapchain. Returns false (software placeholder
    /// stays) when there is no display, no WSI extension set for the platform, or the
    /// `FUSE_EDITOR_HEADLESS_VIEWPORT` override is set. Call before the game thread starts ticking.
    bool enableEmbeddedVulkanViewport(bool enableValidation = false);
    /// UI-side poll of the game thread's present state (runs on a timer; public for tests).
    void pollEmbeddedVulkanViewport();
    /// True once the Vulkan child window is embedded and its surface handed to the game thread.
    [[nodiscard]] bool embeddedVulkanViewportActive() const { return m_vkSurfacePosted; }
    [[nodiscard]] ViewportVulkanWindow* vulkanWindow() const { return m_vkWindow; }
    [[nodiscard]] QWidget* vulkanContainer() const { return m_vkContainer; }
    /// Destroy the child window (and with it Qt's VkSurfaceKHR), then the adopted QVulkanInstance.
    /// The game thread must be stopped and `RuntimeViewportHook::releaseWindowSurface()` called
    /// first; the FUSE VkInstance itself is destroyed later with the host.
    void releaseVulkanViewport();

    /// Gizmo / brush overlay over the embedded Vulkan window: a native child stacked above the
    /// container and shaped (XShape / SetWindowRgn) to the drawn handles, so the swapchain stays
    /// visible everywhere else. Inactive (hidden) on the software viewport, which paints inline.
    [[nodiscard]] bool overlayActive() const;
    [[nodiscard]] QWidget* overlayWidget() const;
    /// Overlay contents as last rendered (ARGB, logical size × dpr); null when inactive.
    [[nodiscard]] const QImage& overlayImage() const { return m_overlayImage; }
    /// Re-render the overlay + mask if the gizmo / brush changed. Force skips the change check.
    void syncOverlay(bool force = false);
    /// Test hook: build the overlay even without a Vulkan surface (so CI can check it headless).
    void setOverlayForced(bool on);
    /// Paint the gizmo or the sculpt brush (whichever is active) with `painter`.
    void paintInteractionOverlay(QPainter& painter);

signals:
    /// ECS scene changed from the viewport (context menu create / delete, pick selection).
    void sceneEdited();
    void transformToolChanged();
    void selectionChanged();
    /// Emitted at the end of every `advanceFrame` with the (clamped) frame delta in seconds.
    void frameAdvanced(float dt);

protected:
    void paintGizmo_(QPainter& painter);
    /// Light / camera icons (point = bulb + rays, spot = cone, directional = sun + arrow, camera =
    /// frustum); clicking one selects its entity (lights / cameras have no pickable geometry).
    void paintSceneIcons_(QPainter& painter);
    ecs::EntityID sceneIconAt_(const QPointF& pos) const;
    /// Click selection: plain click replaces, Ctrl / Shift click toggles membership.
    void applyClickSelection_(ecs::EntityID hit, bool additive);
public:
    [[nodiscard]] int sceneIconCount() const { return static_cast<int>(m_sceneIcons.size()); }
    void setSceneIconsVisible(bool on) { m_showSceneIcons = on; update(); }
    /// Collider wireframes (sphere / box / capsule): selected entities always, every collider when `all`.
    void setAllCollidersVisible(bool all) { m_showAllColliders = all; syncOverlay(true); update(); }
    [[nodiscard]] bool allCollidersVisible() const { return m_showAllColliders; }
    /// Mesh bounding boxes (local AABB through the world transform), drawn with the collider overlay.
    /// Navigation overlay: world-space segments (ax ay az bx by bz per segment), drawn cyan. Empty clears it.
    void setNavOverlay(std::vector<f32> segments) { m_navSegs = std::move(segments); syncOverlay(true); update(); }
    [[nodiscard]] int navOverlaySegmentCount() const { return static_cast<int>(m_navSegs.size() / 6u); }
    void setMeshBoundsVisible(bool on) { m_showMeshBounds = on; syncOverlay(true); update(); }
    [[nodiscard]] bool meshBoundsVisible() const { return m_showMeshBounds; }
    /// Wireframe segments in the last overlay pass (tests).
    [[nodiscard]] usize wireframeSegmentCount() const {
        return static_cast<usize>(std::count_if(m_colliderSelected.begin(), m_colliderSelected.end(), [](u8 k) { return k >= 3; }));
    }
    /// Projected collider segments of the last paint / overlay refresh.
    [[nodiscard]] int colliderLineCount() const { return static_cast<int>(m_colliderLines.size()); }
    /// Moves the camera (keeping yaw / pitch) so every selected entity fits the view. False when nothing is selected.
    bool frameSelection();
private:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
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
    bool attachVulkanWindow_(void* vkInstance);
    void fallBackToPlaceholder_();

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    ViewportPanel m_panel;
    ViewportSceneView m_sceneView;
    EntityContextMenu m_contextMenu;
    TransformGizmo m_gizmo;
    ecs::EntityID m_gizmoEntity = ecs::EntityID::null();
    bool m_gizmoMacroOpen = false;
    /// Multi-select drag: start states of the other selected roots (and whether they share the primary's parent).
    struct GroupStart {
        ecs::EntityID id;
        TransformCommand::State state;
        bool sameFrame = false;
    };
    std::vector<GroupStart> m_groupStart;
    struct SceneIcon {
        ecs::EntityID id;
        QPointF pos; ///< widget (logical) pixels
    };
    std::vector<SceneIcon> m_sceneIcons;
    bool m_showSceneIcons = true;
    bool m_showAllColliders = false;
    bool m_showMeshBounds = false;
    /// View Mode > Wireframe: engine mesh id -> object-space edges (copied once from the MeshRegistry).
    std::unordered_map<u32, std::vector<f32>> m_wireCache;
    static constexpr usize kMaxWireSegments = 400000;
    bool m_inOverlayPaint = false; ///< syncOverlay already collected the collider lines
    std::vector<QLineF> m_colliderLines;
    std::vector<u8> m_colliderSelected; ///< 0 collider / bounds, 1 selected, 2 navmesh overlay
    std::vector<f32> m_navSegs;         ///< world-space segments (6 floats each) from setNavOverlay
    void collectColliderLines_();
    void paintColliders_(QPainter& painter);
    int m_gizmoHover = -1;
    SdfSculptPanel* m_sculptPanel = nullptr;
    bool m_sculptMode = false;
    bool m_sculptDragging = false;
    QPointF m_hoverPos;
    bool m_hoverValid = false;
    bool sculptHit_(const QPointF& pos, ecs::vec3& point, ecs::vec3& normal);
    bool sculptAt_(const QPointF& pos);
    void paintBrush_(QPainter& painter);
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

    std::unique_ptr<QVulkanInstance> m_qtVulkanInstance;
    ViewportVulkanWindow* m_vkWindow = nullptr; ///< owned by m_vkContainer
    QWidget* m_vkContainer = nullptr;
    QTimer m_presentPollTimer;
    bool m_windowPresentRequested = false;
    bool m_vkSurfacePosted = false;
    ViewportOverlayWidget* m_overlay = nullptr;
    QImage m_overlayImage;
    QByteArray m_overlaySignature;
    bool m_overlayForced = false;
};

} // namespace fuse::editor::qt
