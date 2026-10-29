#pragma once

// Multi-viewport (GUI-8): orthographic Top / Front / Side schematic views beside the perspective
// viewport. Drawn with QPainter from the edit registry (world AABBs of meshes, markers for other
// entities), so they cost no extra GPU swapchain. Click selects, dragging the selection moves it in the
// view plane (one undo step per drag), middle / right drag pans, the wheel zooms at the cursor, F frames.

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <mutex>
#include <vector>

namespace fuse::editor {
class EditorHost;
}

namespace fuse::editor::qt {

class OrthoViewWidget final : public QWidget {
    Q_OBJECT
public:
    enum class Axis { Top, Front, Side };

    OrthoViewWidget(EditorHost& host, std::mutex& sceneMutex, Axis axis, QWidget* parent = nullptr);

    [[nodiscard]] Axis axis() const { return m_axis; }
    [[nodiscard]] QString axisName() const;
    /// World -> widget pixels (logical) and back (the coordinate along the view direction is kept from `depthFrom`).
    [[nodiscard]] QPointF worldToView(const ecs::vec3& p) const;
    [[nodiscard]] ecs::vec3 viewToWorld(QPointF pos, const ecs::vec3& depthFrom) const;
    [[nodiscard]] float pixelsPerUnit() const { return m_scale; }
    /// Default framing (startup / layout change): the selection if any, else everything except huge
    /// boxes such as ground planes (> kHugeExtentUnits) unless they are all there is.
    bool frameAuto() { return frame_(true, 8.0, true); }
    void setPixelsPerUnit(float s);
    void setCenter(float u, float v);
    /// Fit the selection (or everything when nothing is selected). False when the scene is empty.
    bool frameSelection();
    /// Fit every entity (min 8 units across). Done automatically the first time the view gets a size.
    bool frameAll();
    /// Entity under a view position (nearest box / marker), null when none.
    [[nodiscard]] ecs::EntityID pick(QPointF pos);
    /// Entities drawn by the last paint (tests).
    [[nodiscard]] int drawnCount() const { return m_drawn; }
    /// Moves the primary selection by a view-space drag from `from` to `to` (one undo step). Tests / mouse.
    bool dragSelection(QPointF from, QPointF to);

signals:
    void selectionChanged();
    void sceneEdited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Item {
        ecs::EntityID id;
        QRectF rect;  // view pixels; empty size = marker
        QPointF center;
        bool selected = false;
    };
    void collect_();
    [[nodiscard]] QPointF project_(float x, float y, float z) const;
    void uv_(const ecs::vec3& p, float& u, float& v) const;

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    Axis m_axis;
    float m_centerU = 0.f;
    float m_centerV = 0.f;
    float m_scale = 40.f;
    std::vector<Item> m_items;
    int m_drawn = 0;
    // interaction
    enum class Drag { None, Pan, Move } m_drag = Drag::None;
    QPointF m_last;
    QPointF m_dragSnapped;
    QPointF m_pressPos;
    ecs::EntityID m_dragEntity = ecs::EntityID::null();
    ecs::vec3 m_dragStartPos{};
    bool m_macroOpen = false;
    bool m_framed = false;
    bool frame_(bool selectionOnly, double minUnits, bool skipHuge = false);
    static constexpr double kHugeExtentUnits = 100.0;
};

} // namespace fuse::editor::qt
