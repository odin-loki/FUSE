#pragma once

#include <fuse/ecs/entity.hpp>

#include <QWidget>

#include <mutex>

class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace fuse::editor {
class EditorHost;
}

namespace fuse::editor::qt {

/// Lighting / environment dock: the scene's sun (DirectionalLight + its Transform rotation as
/// elevation / azimuth), ambient light and fog in one place. Edits go through SetProperty /
/// AddComponent commands, so they are undoable like inspector edits; missing pieces get an Add button.
class LightingPanel final : public QWidget {
    Q_OBJECT
public:
    LightingPanel(EditorHost& host, std::mutex& sceneMutex, QWidget* parent = nullptr);

    /// Re-read the scene (called by the main window's status timer and after edits).
    void refresh();

    [[nodiscard]] ecs::EntityID sunEntity() const { return m_sun; }
    [[nodiscard]] ecs::EntityID ambientEntity() const { return m_ambient; }
    [[nodiscard]] ecs::EntityID fogEntity() const { return m_fog; }

    /// Create a sun entity (undoable) when the scene has none; returns it.
    ecs::EntityID addSun();
    /// Add AmbientLight / EnvironmentFog to the sun entity (or a new one) when missing.
    bool addAmbient();
    bool addFog();

    /// Programmatic edits (the same path as the widgets).
    void setSunAngles(double elevationDeg, double azimuthDeg);
    void setSunIntensity(double v);
    void setAmbientIntensity(double v);
    void setFogDensity(double v);

private:
    void post_(ecs::EntityID e, const char* property, const std::string& value);
    void pickColor_(ecs::EntityID e, const char* property, QPushButton* button);
    ecs::EntityID hostEntity_();

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    ecs::EntityID m_sun{}, m_ambient{}, m_fog{};
    bool m_updating = false;

    QLabel* m_sunLabel = nullptr;
    QPushButton* m_addSun = nullptr;
    QDoubleSpinBox* m_elevation = nullptr;
    QDoubleSpinBox* m_azimuth = nullptr;
    QDoubleSpinBox* m_sunIntensity = nullptr;
    QPushButton* m_sunColor = nullptr;

    QPushButton* m_addAmbient = nullptr;
    QDoubleSpinBox* m_ambientIntensity = nullptr;
    QPushButton* m_ambientColor = nullptr;

    QPushButton* m_addFog = nullptr;
    QDoubleSpinBox* m_fogDensity = nullptr;
    QDoubleSpinBox* m_fogDistance = nullptr;
    QPushButton* m_fogColor = nullptr;
};

} // namespace fuse::editor::qt
