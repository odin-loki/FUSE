#pragma once

// Navigation panel: bakes a fuse::navigation::GridNavmesh on the XZ plane from the scene's colliders (every
// non-trigger collider's world footprint blocks the cells it covers), shows it in the viewport (blocked cells
// crossed out, grid outline) and tests a path between two points.

#include <fuse/navigation/grid_navmesh.hpp>

#include <QWidget>

#include <mutex>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace fuse::editor {
class EditorHost;
}

namespace fuse::editor::qt {

class ViewportPlaceholderWidget;

class NavmeshPanel final : public QWidget {
    Q_OBJECT
public:
    NavmeshPanel(EditorHost& host, std::mutex& sceneMutex, ViewportPlaceholderWidget* viewport, QWidget* parent = nullptr);

    /// Bakes from the current scene; false when the grid arguments are invalid.
    bool bake();
    /// A* between two world XZ points on the baked mesh; returns the waypoint count (0: no path).
    int testPath(float ax, float az, float bx, float bz);

    void setCellSize(double size);
    void setHalfExtent(int cells);
    void setOverlayVisible(bool on);

    [[nodiscard]] const navigation::GridNavmesh& navmesh() const { return m_mesh; }
    [[nodiscard]] int blockedCount() const { return m_blocked; }
    [[nodiscard]] QLabel* statusLabel() const { return m_status; }

private:
    void refreshOverlay();
    [[nodiscard]] float originX() const;
    [[nodiscard]] float originZ() const;

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    ViewportPlaceholderWidget* m_viewport = nullptr;
    navigation::GridNavmesh m_mesh;
    std::vector<unsigned char> m_blockedCells;
    std::vector<float> m_path; ///< world xz pairs of the last test path
    int m_blocked = 0;
    QDoubleSpinBox* m_cell = nullptr;
    QSpinBox* m_extent = nullptr;
    QCheckBox* m_show = nullptr;
    QDoubleSpinBox* m_from[2] = {};
    QDoubleSpinBox* m_to[2] = {};
    QLabel* m_status = nullptr;
};

} // namespace fuse::editor::qt
