#pragma once

// Terrain panel (GUI-6): creates heightfield Terrain entities and sculpts them (Raise / Lower / Flatten stamps)
// by clicking in the viewport or through the API. Each edit is one undo step; the runtime viewport rebuilds the
// mesh (fuse::terrain::build_chunk_mesh) and hands it to the renderer.

#include <fuse/ecs/components/terrain.hpp>
#include <fuse/ecs/entity.hpp>

#include <QWidget>

#include <mutex>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace fuse::editor {
class EditorHost;
}

namespace fuse::editor::qt {

class ViewportPlaceholderWidget;

class TerrainPanel final : public QWidget {
    Q_OBJECT
public:
    TerrainPanel(EditorHost& host, std::mutex& sceneMutex, ViewportPlaceholderWidget* viewport, QWidget* parent = nullptr);

    /// New Terrain entity at the origin from the panel's settings (one undo step); selects it.
    ecs::EntityID createTerrain();
    /// Stamp with the panel's brush at local (x, z) of `terrain`; false if it has no Terrain or the budget is full.
    bool sculptLocal(ecs::EntityID terrain, float x, float z);
    /// Viewport click: world ray vs every Terrain; stamps the nearest hit. True when one was hit.
    bool sculptRay(const float origin[3], const float dir[3]);
    /// Drag stroke (local space of `terrain`): stamps every ~half brush radius along the path; the whole stroke
    /// is one undo step committed by endStroke(). Returns false if `terrain` has no Terrain.
    bool beginStroke(ecs::EntityID terrain, float x, float z);
    void strokeTo(float x, float z);
    bool endStroke();
    [[nodiscard]] bool stroking() const { return m_strokeEntity.valid(); }
    /// Viewport stroke: phase 0 = press (true if a terrain was hit), 1 = move, 2 = release.
    bool sculptRayPhase(int phase, const float origin[3], const float dir[3]);
    /// Remove every stamp of the selected terrain (one undo step).
    bool clearSculpts();

    void setMode(int mode); ///< ecs::TerrainStamp::Raise / Lower / Flatten
    void setBrush(double radius, double strength);
    void setSculptInViewport(bool on);
    [[nodiscard]] bool sculptInViewport() const;
    /// Primary selection if it has a Terrain, else the first Terrain in the scene.
    [[nodiscard]] ecs::EntityID activeTerrain() const;
    [[nodiscard]] QLabel* statusLabel() const { return m_status; }

private:
    bool pickTerrain(const float origin[3], const float dir[3], ecs::EntityID* only, ecs::EntityID& out, float local[3]);
    ecs::TerrainStamp brushStamp(float x, float z) const;
    void refreshStatus();
    bool commitEdit(ecs::EntityID e, const ecs::Terrain& before, const ecs::Terrain& after, const char* what);

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    ViewportPlaceholderWidget* m_viewport = nullptr;
    QSpinBox* m_cells = nullptr;
    QDoubleSpinBox* m_size = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QSpinBox* m_seed = nullptr;
    QComboBox* m_mode = nullptr;
    QDoubleSpinBox* m_radius = nullptr;
    QDoubleSpinBox* m_strength = nullptr;
    QCheckBox* m_sculpt = nullptr;
    QLabel* m_status = nullptr;
    ecs::EntityID m_strokeEntity;
    ecs::Terrain m_strokeBefore{};
    float m_strokeLast[2] = {0.f, 0.f};
};

} // namespace fuse::editor::qt
