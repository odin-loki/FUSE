#include "lighting_panel.hpp"

#include <fuse/ecs/components/environment.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fuse::editor::qt {
namespace {

Handle<Object> handleOf(ecs::EntityID id) {
    return id.valid() ? Handle<Object>(id.index, id.generation) : Handle<Object>::invalid();
}

QDoubleSpinBox* spin(double lo, double hi, double step, int decimals, const char* name) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setKeyboardTracking(false);
    s->setObjectName(QString::fromLatin1(name));
    return s;
}

void paintSwatch(QPushButton* b, const ecs::vec3& c) {
    const auto to8 = [](float v) { return std::clamp(static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)), 0, 255); };
    const QColor q(to8(c.x), to8(c.y), to8(c.z));
    b->setText(q.name());
    b->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:%2; }")
                         .arg(q.name(), q.lightness() > 128 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

std::string vec3Value(float x, float y, float z) {
    return formatPropertyFloat(x) + "," + formatPropertyFloat(y) + "," + formatPropertyFloat(z);
}

} // namespace

LightingPanel::LightingPanel(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    setObjectName(QStringLiteral("fuseLightingPanel"));
    auto* root = new QVBoxLayout(this);

    auto* sunBox = new QGroupBox(tr("Sun"));
    auto* sunForm = new QFormLayout(sunBox);
    m_sunLabel = new QLabel(tr("(none)"));
    m_addSun = new QPushButton(tr("Add Sun"));
    m_addSun->setObjectName(QStringLiteral("fuseLightingAddSun"));
    m_elevation = spin(-90.0, 90.0, 1.0, 1, "fuseLightingElevation");
    m_azimuth = spin(-180.0, 180.0, 5.0, 1, "fuseLightingAzimuth");
    m_sunIntensity = spin(0.0, 1.0e6, 0.1, 2, "fuseLightingSunIntensity");
    m_sunColor = new QPushButton;
    m_sunColor->setObjectName(QStringLiteral("fuseLightingSunColor"));
    sunForm->addRow(m_sunLabel, m_addSun);
    sunForm->addRow(tr("Elevation"), m_elevation);
    sunForm->addRow(tr("Azimuth"), m_azimuth);
    sunForm->addRow(tr("Intensity"), m_sunIntensity);
    sunForm->addRow(tr("Color"), m_sunColor);
    root->addWidget(sunBox);

    auto* ambBox = new QGroupBox(tr("Ambient"));
    auto* ambForm = new QFormLayout(ambBox);
    m_addAmbient = new QPushButton(tr("Add Ambient Light"));
    m_addAmbient->setObjectName(QStringLiteral("fuseLightingAddAmbient"));
    m_ambientIntensity = spin(0.0, 1.0e3, 0.05, 2, "fuseLightingAmbientIntensity");
    m_ambientColor = new QPushButton;
    m_ambientColor->setObjectName(QStringLiteral("fuseLightingAmbientColor"));
    ambForm->addRow(m_addAmbient);
    ambForm->addRow(tr("Intensity"), m_ambientIntensity);
    ambForm->addRow(tr("Color"), m_ambientColor);
    root->addWidget(ambBox);

    auto* fogBox = new QGroupBox(tr("Fog"));
    auto* fogForm = new QFormLayout(fogBox);
    m_addFog = new QPushButton(tr("Add Fog"));
    m_addFog->setObjectName(QStringLiteral("fuseLightingAddFog"));
    m_fogDensity = spin(0.0, 1.0, 0.001, 4, "fuseLightingFogDensity");
    m_fogDistance = spin(1.0, 1.0e6, 10.0, 0, "fuseLightingFogDistance");
    m_fogColor = new QPushButton;
    m_fogColor->setObjectName(QStringLiteral("fuseLightingFogColor"));
    fogForm->addRow(m_addFog);
    fogForm->addRow(tr("Density"), m_fogDensity);
    fogForm->addRow(tr("Visible distance"), m_fogDistance);
    fogForm->addRow(tr("Color"), m_fogColor);
    root->addWidget(fogBox);
    root->addStretch(1);

    connect(m_addSun, &QPushButton::clicked, this, [this] { addSun(); });
    connect(m_addAmbient, &QPushButton::clicked, this, [this] { addAmbient(); });
    connect(m_addFog, &QPushButton::clicked, this, [this] { addFog(); });
    const auto angles = [this] {
        if (!m_updating) setSunAngles(m_elevation->value(), m_azimuth->value());
    };
    connect(m_elevation, &QDoubleSpinBox::valueChanged, this, angles);
    connect(m_azimuth, &QDoubleSpinBox::valueChanged, this, angles);
    connect(m_sunIntensity, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updating) setSunIntensity(v);
    });
    connect(m_ambientIntensity, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updating) setAmbientIntensity(v);
    });
    connect(m_fogDensity, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updating) setFogDensity(v);
    });
    connect(m_fogDistance, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updating) post_(m_fog, "EnvironmentFog.visible_distance", formatPropertyFloat(static_cast<f32>(v)));
    });
    connect(m_sunColor, &QPushButton::clicked, this, [this] { pickColor_(m_sun, "DirectionalLight.color", m_sunColor); });
    connect(m_ambientColor, &QPushButton::clicked, this,
            [this] { pickColor_(m_ambient, "AmbientLight.color", m_ambientColor); });
    connect(m_fogColor, &QPushButton::clicked, this, [this] { pickColor_(m_fog, "EnvironmentFog.color", m_fogColor); });
    refresh();
}

void LightingPanel::refresh() {
    m_updating = true;
    ecs::DirectionalLight sun{};
    ecs::AmbientLight amb{};
    ecs::EnvironmentFog fog{};
    ecs::quat rot{0.f, 0.f, 0.f, 1.f};
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& reg = m_host.editorScene().registry();
        m_sun = m_ambient = m_fog = ecs::EntityID{};
        reg.each<ecs::DirectionalLight>([&](ecs::EntityID id, ecs::DirectionalLight& l) {
            if (m_sun.valid()) return;
            m_sun = id;
            sun = l;
            if (const ecs::Transform* t = reg.get<ecs::Transform>(id)) rot = t->rotation;
        });
        reg.each<ecs::AmbientLight>([&](ecs::EntityID id, ecs::AmbientLight& a) {
            if (!m_ambient.valid()) { m_ambient = id; amb = a; }
        });
        reg.each<ecs::EnvironmentFog>([&](ecs::EntityID id, ecs::EnvironmentFog& f) {
            if (!m_fog.valid()) { m_fog = id; fog = f; }
        });
    }
    const bool hasSun = m_sun.valid(), hasAmb = m_ambient.valid(), hasFog = m_fog.valid();
    m_sunLabel->setText(hasSun ? tr("Entity #%1").arg(m_sun.index) : tr("(no sun)"));
    m_addSun->setVisible(!hasSun);
    for (QWidget* w : {static_cast<QWidget*>(m_elevation), static_cast<QWidget*>(m_azimuth),
                       static_cast<QWidget*>(m_sunIntensity), static_cast<QWidget*>(m_sunColor)})
        w->setEnabled(hasSun);
    m_addAmbient->setVisible(!hasAmb);
    m_ambientIntensity->setEnabled(hasAmb);
    m_ambientColor->setEnabled(hasAmb);
    m_addFog->setVisible(!hasFog);
    for (QWidget* w : {static_cast<QWidget*>(m_fogDensity), static_cast<QWidget*>(m_fogDistance),
                       static_cast<QWidget*>(m_fogColor)})
        w->setEnabled(hasFog);
    if (hasSun) {
        const ecs::vec3 e = eulerDegFromQuat(rot); // x = pitch (elevation is -pitch), y = yaw
        if (!m_elevation->hasFocus()) m_elevation->setValue(-e.x);
        if (!m_azimuth->hasFocus()) m_azimuth->setValue(e.y);
        if (!m_sunIntensity->hasFocus()) m_sunIntensity->setValue(sun.intensity);
        paintSwatch(m_sunColor, sun.color);
    }
    if (hasAmb) {
        if (!m_ambientIntensity->hasFocus()) m_ambientIntensity->setValue(amb.intensity);
        paintSwatch(m_ambientColor, amb.color);
    }
    if (hasFog) {
        if (!m_fogDensity->hasFocus()) m_fogDensity->setValue(fog.density);
        if (!m_fogDistance->hasFocus()) m_fogDistance->setValue(fog.visible_distance);
        paintSwatch(m_fogColor, fog.color);
    }
    m_updating = false;
}

void LightingPanel::post_(ecs::EntityID e, const char* property, const std::string& value) {
    if (!e.valid()) return;
    m_host.postFromUi(makeSetPropertyCommand(handleOf(e), property, value));
}

void LightingPanel::pickColor_(ecs::EntityID e, const char* property, QPushButton* button) {
    if (!e.valid()) return;
    const QColor c = QColorDialog::getColor(QColor(button->text()), this, tr("Pick colour"));
    if (!c.isValid()) return;
    post_(e, property, vec3Value(static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF())));
}

ecs::EntityID LightingPanel::addSun() {
    if (m_sun.valid()) return m_sun;
    ecs::EntityID created;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        EntityComponentSet set{};
        ecs::Transform t{};
        t.position = ecs::vec3{0.f, 10.f, 0.f, 1.f};
        t.rotation = quatFromEulerDeg(-45.f, 30.f, 0.f);
        std::get<std::optional<ecs::Transform>>(set) = t;
        std::get<std::optional<ecs::DirectionalLight>>(set) = ecs::DirectionalLight{};
        auto cmd = std::make_unique<CreateEntityCommand>(registry, std::move(set), "Add sun");
        CreateEntityCommand* raw = cmd.get();
        m_host.undoStack().execute(std::move(cmd));
        created = raw->createdEntity();
        m_host.editorState().sceneModified = true;
    }
    refresh();
    return created;
}

ecs::EntityID LightingPanel::hostEntity_() {
    if (m_sun.valid()) return m_sun;
    return addSun();
}

bool LightingPanel::addAmbient() {
    if (m_ambient.valid()) return false;
    EditorCommand cmd;
    cmd.kind = CommandKind::AddComponent;
    cmd.target = handleOf(hostEntity_());
    cmd.propertyName = ecs::AmbientLight::component_name;
    m_host.postFromUi(std::move(cmd));
    return true;
}

bool LightingPanel::addFog() {
    if (m_fog.valid()) return false;
    EditorCommand cmd;
    cmd.kind = CommandKind::AddComponent;
    cmd.target = handleOf(hostEntity_());
    cmd.propertyName = ecs::EnvironmentFog::component_name;
    m_host.postFromUi(std::move(cmd));
    return true;
}

void LightingPanel::setSunAngles(double elevationDeg, double azimuthDeg) {
    post_(m_sun, "transform.rotation",
          formatPropertyQuat(quatFromEulerDeg(static_cast<f32>(-elevationDeg), static_cast<f32>(azimuthDeg), 0.f)));
}

void LightingPanel::setSunIntensity(double v) { post_(m_sun, "directional.intensity", formatPropertyFloat(static_cast<f32>(v))); }
void LightingPanel::setAmbientIntensity(double v) {
    post_(m_ambient, "AmbientLight.intensity", formatPropertyFloat(static_cast<f32>(v)));
}
void LightingPanel::setFogDensity(double v) { post_(m_fog, "EnvironmentFog.density", formatPropertyFloat(static_cast<f32>(v))); }

} // namespace fuse::editor::qt
