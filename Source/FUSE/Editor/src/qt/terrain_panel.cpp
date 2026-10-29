#include "terrain_panel.hpp"

#include "viewport_placeholder_widget.hpp"

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/terrain_builder.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

namespace fuse::editor::qt {

namespace {

class TerrainEditCommand final : public UndoCommand {
public:
    TerrainEditCommand(ecs::Registry& r, ecs::EntityID e, const ecs::Terrain& before, const ecs::Terrain& after, std::string what)
        : m_registry(r), m_entity(e), m_before(before), m_after(after), m_what(std::move(what)) {}
    void execute() override { apply(m_after); }
    void undo() override { apply(m_before); }
    std::string description() const override { return m_what; }

private:
    void apply(const ecs::Terrain& t) {
        if (ecs::Terrain* cur = m_registry.get<ecs::Terrain>(m_entity)) *cur = t;
    }
    ecs::Registry& m_registry;
    ecs::EntityID m_entity;
    ecs::Terrain m_before, m_after;
    std::string m_what;
};

/// Inverse of an affine column-major 4x4 (3x3 linear part + translation); false when singular.
bool invertAffine(const f32* m, f32* out) {
    const f32 a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const f32 A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const f32 det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) return false;
    const f32 s = 1.f / det;
    const f32 inv[9] = {A * s, -(b * i - c * h) * s, (b * f - c * e) * s, B * s, (a * i - c * g) * s, -(a * f - c * d) * s,
                        C * s, -(a * h - b * g) * s, (a * e - b * d) * s};
    // inv is row-major r0 = (inv[0], inv[1], inv[2]) ...
    std::fill(out, out + 16, 0.f);
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) out[r + 4 * k] = inv[r * 3 + k];
    }
    for (int r = 0; r < 3; ++r) out[r + 12] = -(out[r] * m[12] + out[r + 4] * m[13] + out[r + 8] * m[14]);
    out[15] = 1.f;
    return true;
}

} // namespace

TerrainPanel::TerrainPanel(EditorHost& host, std::mutex& sceneMutex, ViewportPlaceholderWidget* viewport, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex), m_viewport(viewport) {
    setObjectName(QStringLiteral("fuseTerrainPanel"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    m_cells = new QSpinBox(this);
    m_cells->setObjectName(QStringLiteral("fuseTerrainCells"));
    m_cells->setRange(2, 256);
    m_cells->setValue(64);
    form->addRow(tr("Cells"), m_cells);
    m_size = new QDoubleSpinBox(this);
    m_size->setObjectName(QStringLiteral("fuseTerrainSize"));
    m_size->setRange(1.0, 10000.0);
    m_size->setValue(64.0);
    m_size->setSuffix(QStringLiteral(" m"));
    form->addRow(tr("Size"), m_size);
    m_height = new QDoubleSpinBox(this);
    m_height->setObjectName(QStringLiteral("fuseTerrainHeight"));
    m_height->setRange(0.0, 1000.0);
    m_height->setValue(6.0);
    m_height->setSuffix(QStringLiteral(" m"));
    form->addRow(tr("Noise height"), m_height);
    m_seed = new QSpinBox(this);
    m_seed->setObjectName(QStringLiteral("fuseTerrainSeed"));
    m_seed->setRange(0, 1 << 30);
    m_seed->setValue(1);
    form->addRow(tr("Seed"), m_seed);
    layout->addLayout(form);
    auto* create = new QPushButton(tr("Create Terrain"), this);
    create->setObjectName(QStringLiteral("fuseTerrainCreate"));
    connect(create, &QPushButton::clicked, this, [this]() { createTerrain(); });
    layout->addWidget(create);

    auto* brush = new QFormLayout();
    m_mode = new QComboBox(this);
    m_mode->setObjectName(QStringLiteral("fuseTerrainMode"));
    m_mode->addItems({tr("Raise"), tr("Lower"), tr("Flatten")});
    brush->addRow(tr("Brush"), m_mode);
    m_radius = new QDoubleSpinBox(this);
    m_radius->setObjectName(QStringLiteral("fuseTerrainRadius"));
    m_radius->setRange(0.1, 1000.0);
    m_radius->setValue(6.0);
    m_radius->setSuffix(QStringLiteral(" m"));
    brush->addRow(tr("Radius"), m_radius);
    m_strength = new QDoubleSpinBox(this);
    m_strength->setObjectName(QStringLiteral("fuseTerrainStrength"));
    m_strength->setRange(-1000.0, 1000.0);
    m_strength->setValue(2.0);
    m_strength->setToolTip(tr("Metres raised / lowered at the centre; Flatten: target height"));
    brush->addRow(tr("Strength"), m_strength);
    layout->addLayout(brush);
    m_sculpt = new QCheckBox(tr("Sculpt in viewport (click)"), this);
    m_sculpt->setObjectName(QStringLiteral("fuseTerrainSculpt"));
    connect(m_sculpt, &QCheckBox::toggled, this, [this](bool on) { setSculptInViewport(on); });
    layout->addWidget(m_sculpt);
    auto* clear = new QPushButton(tr("Clear Sculpts"), this);
    clear->setObjectName(QStringLiteral("fuseTerrainClear"));
    connect(clear, &QPushButton::clicked, this, [this]() { clearSculpts(); });
    layout->addWidget(clear);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("fuseTerrainStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch(1);
    refreshStatus();
}

ecs::EntityID TerrainPanel::createTerrain() {
    ecs::EntityID created;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        EntityComponentSet set{};
        ecs::Transform t{};
        std::get<std::optional<ecs::Transform>>(set) = t;
        ecs::Terrain terrain{};
        terrain.cells = static_cast<u32>(m_cells->value());
        terrain.size = static_cast<f32>(m_size->value());
        terrain.height = static_cast<f32>(m_height->value());
        terrain.seed = static_cast<u32>(m_seed->value());
        std::get<std::optional<ecs::Terrain>>(set) = terrain;
        std::get<std::optional<ecs::Mesh>>(set) = ecs::Mesh{};
        auto cmd = std::make_unique<CreateEntityCommand>(registry, std::move(set), "Create terrain");
        CreateEntityCommand* raw = cmd.get();
        m_host.undoStack().execute(std::move(cmd));
        created = raw->createdEntity();
        if (created.valid()) {
            ecs::TransformSystem::update(registry);
            m_host.runtimeViewport().syncTerrainMeshes(registry);
            m_host.editorState().selectedEntities = {created};
            m_host.editorState().primarySelection = created;
        }
    }
    refreshStatus();
    if (m_viewport != nullptr) {
        emit m_viewport->selectionChanged();
        m_viewport->syncOverlay(true);
        m_viewport->update();
    }
    return created;
}

ecs::EntityID TerrainPanel::activeTerrain() const {
    ecs::Registry& registry = m_host.editorScene().registry();
    const ecs::EntityID sel = m_host.editorState().primarySelection;
    if (sel.valid() && registry.get<ecs::Terrain>(sel) != nullptr) return sel;
    ecs::EntityID first;
    registry.each<ecs::Terrain>([&](ecs::EntityID id, ecs::Terrain&) {
        if (!first.valid()) first = id;
    });
    return first;
}

bool TerrainPanel::commitEdit(ecs::EntityID e, const ecs::Terrain& before, const ecs::Terrain& after, const char* what) {
    ecs::Registry& registry = m_host.editorScene().registry();
    m_host.undoStack().execute(std::make_unique<TerrainEditCommand>(registry, e, before, after, what));
    m_host.runtimeViewport().syncTerrainMeshes(registry);
    return true;
}

bool TerrainPanel::sculptLocal(ecs::EntityID e, float x, float z) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Terrain* t = m_host.editorScene().registry().get<ecs::Terrain>(e);
        if (t != nullptr) {
            const ecs::Terrain before = *t;
            ecs::Terrain after = before;
            ecs::TerrainStamp s{};
            s.x = x;
            s.z = z;
            s.radius = static_cast<f32>(m_radius->value());
            s.amount = static_cast<f32>(m_strength->value());
            s.mode = static_cast<u32>(std::clamp(m_mode->currentIndex(), 0, 2));
            if (addTerrainStamp(after, s)) ok = commitEdit(e, before, after, "Sculpt terrain");
        }
    }
    refreshStatus();
    if (m_viewport != nullptr) {
        m_viewport->syncOverlay(true);
        m_viewport->update();
    }
    return ok;
}

bool TerrainPanel::sculptRay(const float origin[3], const float dir[3]) {
    ecs::EntityID best;
    f32 bestT = 1e30f;
    f32 bestLocal[3] = {0.f, 0.f, 0.f};
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        ecs::TransformSystem::update(registry);
        registry.each<ecs::Transform, ecs::Terrain>([&](ecs::EntityID id, ecs::Transform& tr, ecs::Terrain& terrain) {
            f32 inv[16];
            const f32* m = tr.local_to_world.data.data();
            if (!invertAffine(m, inv)) return;
            const f32 o[3] = {inv[0] * origin[0] + inv[4] * origin[1] + inv[8] * origin[2] + inv[12],
                              inv[1] * origin[0] + inv[5] * origin[1] + inv[9] * origin[2] + inv[13],
                              inv[2] * origin[0] + inv[6] * origin[1] + inv[10] * origin[2] + inv[14]};
            const f32 d[3] = {inv[0] * dir[0] + inv[4] * dir[1] + inv[8] * dir[2], inv[1] * dir[0] + inv[5] * dir[1] + inv[9] * dir[2],
                              inv[2] * dir[0] + inv[6] * dir[1] + inv[10] * dir[2]};
            terrain::Heightfield field;
            buildTerrainHeightfield(terrain, field);
            f32 hit[3];
            if (!raycastTerrain(field, terrain, o, d, hit)) return;
            // Distance along the world ray (local parameter maps 1:1 since d is the transformed world direction).
            const f32 tl = std::fabs(d[0]) > std::fabs(d[2]) ? (hit[0] - o[0]) / d[0] : (std::fabs(d[2]) > 1e-8f ? (hit[2] - o[2]) / d[2] : (hit[1] - o[1]) / d[1]);
            if (tl < bestT) {
                bestT = tl;
                best = id;
                std::copy(hit, hit + 3, bestLocal);
            }
        });
    }
    if (!best.valid()) return false;
    return sculptLocal(best, bestLocal[0], bestLocal[2]);
}

bool TerrainPanel::clearSculpts() {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        const ecs::EntityID e = activeTerrain();
        ecs::Terrain* t = e.valid() ? m_host.editorScene().registry().get<ecs::Terrain>(e) : nullptr;
        if (t != nullptr && t->stamp_count > 0) {
            const ecs::Terrain before = *t;
            ecs::Terrain after = before;
            after.stamp_count = 0;
            for (ecs::TerrainStamp& s : after.stamps) s = ecs::TerrainStamp{};
            ok = commitEdit(e, before, after, "Clear terrain sculpts");
        }
    }
    refreshStatus();
    return ok;
}

void TerrainPanel::setMode(int mode) { m_mode->setCurrentIndex(std::clamp(mode, 0, 2)); }

void TerrainPanel::setBrush(double radius, double strength) {
    m_radius->setValue(radius);
    m_strength->setValue(strength);
}

bool TerrainPanel::sculptInViewport() const { return m_sculpt->isChecked(); }

void TerrainPanel::setSculptInViewport(bool on) {
    if (m_sculpt->isChecked() != on) m_sculpt->setChecked(on); // re-enters through toggled
    if (m_viewport == nullptr) return;
    if (on) {
        m_viewport->setTerrainSculptHandler([this](const float* o, const float* d) { return sculptRay(o, d); });
    } else {
        m_viewport->setTerrainSculptHandler({});
    }
}

void TerrainPanel::refreshStatus() {
    int count = 0;
    u32 stamps = 0;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        registry.each<ecs::Terrain>([&](ecs::EntityID, ecs::Terrain&) { ++count; });
        const ecs::EntityID e = activeTerrain();
        if (const ecs::Terrain* t = e.valid() ? registry.get<ecs::Terrain>(e) : nullptr) stamps = t->stamp_count;
    }
    m_status->setText(tr("%1 terrain(s); active terrain sculpts %2 / %3").arg(count).arg(stamps).arg(ecs::Terrain::kMaxStamps));
}

} // namespace fuse::editor::qt
