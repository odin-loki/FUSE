#include "navmesh_panel.hpp"

#include "viewport_placeholder_widget.hpp"

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/editor_host.hpp>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fuse::editor::qt {

namespace {
QDoubleSpinBox* coordBox(QWidget* parent, double value) {
    auto* b = new QDoubleSpinBox(parent);
    b->setRange(-10000.0, 10000.0);
    b->setDecimals(2);
    b->setSingleStep(0.5);
    b->setValue(value);
    return b;
}
} // namespace

NavmeshPanel::NavmeshPanel(EditorHost& host, std::mutex& sceneMutex, ViewportPlaceholderWidget* viewport, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex), m_viewport(viewport) {
    setObjectName(QStringLiteral("fuseNavmeshPanel"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    m_cell = new QDoubleSpinBox(this);
    m_cell->setObjectName(QStringLiteral("fuseNavCellSize"));
    m_cell->setRange(0.05, 50.0);
    m_cell->setSingleStep(0.25);
    m_cell->setValue(0.5);
    m_cell->setSuffix(QStringLiteral(" m"));
    form->addRow(tr("Cell size"), m_cell);
    m_extent = new QSpinBox(this);
    m_extent->setObjectName(QStringLiteral("fuseNavExtent"));
    m_extent->setRange(1, 512);
    m_extent->setValue(40);
    m_extent->setToolTip(tr("Half size of the grid in cells, centred on the world origin"));
    form->addRow(tr("Half extent (cells)"), m_extent);
    layout->addLayout(form);

    auto* bakeButton = new QPushButton(tr("Bake Navmesh"), this);
    bakeButton->setObjectName(QStringLiteral("fuseNavBake"));
    connect(bakeButton, &QPushButton::clicked, this, [this]() { bake(); });
    layout->addWidget(bakeButton);
    m_show = new QCheckBox(tr("Show in viewport"), this);
    m_show->setObjectName(QStringLiteral("fuseNavShow"));
    m_show->setChecked(true);
    connect(m_show, &QCheckBox::toggled, this, [this]() { refreshOverlay(); });
    layout->addWidget(m_show);

    auto* pathForm = new QFormLayout();
    for (int i = 0; i < 2; ++i) {
        m_from[i] = coordBox(this, i == 0 ? -5.0 : 0.0);
        m_to[i] = coordBox(this, i == 0 ? 5.0 : 0.0);
    }
    auto pair = [this](QDoubleSpinBox* a, QDoubleSpinBox* b) {
        auto* w = new QWidget(this);
        auto* row = new QHBoxLayout(w);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(a);
        row->addWidget(b);
        return w;
    };
    pathForm->addRow(tr("From x/z"), pair(m_from[0], m_from[1]));
    pathForm->addRow(tr("To x/z"), pair(m_to[0], m_to[1]));
    layout->addLayout(pathForm);
    auto* pathButton = new QPushButton(tr("Test Path"), this);
    pathButton->setObjectName(QStringLiteral("fuseNavTestPath"));
    connect(pathButton, &QPushButton::clicked, this, [this]() {
        testPath(static_cast<float>(m_from[0]->value()), static_cast<float>(m_from[1]->value()),
                 static_cast<float>(m_to[0]->value()), static_cast<float>(m_to[1]->value()));
    });
    layout->addWidget(pathButton);
    m_status = new QLabel(tr("Not baked"), this);
    m_status->setObjectName(QStringLiteral("fuseNavStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    layout->addStretch(1);
}

void NavmeshPanel::setCellSize(double size) { m_cell->setValue(size); }
void NavmeshPanel::setHalfExtent(int cells) { m_extent->setValue(cells); }
void NavmeshPanel::setOverlayVisible(bool on) { m_show->setChecked(on); }

float NavmeshPanel::originX() const { return -static_cast<float>(m_extent->value() * m_cell->value()); }
float NavmeshPanel::originZ() const { return originX(); }

bool NavmeshPanel::bake() {
    const auto n = static_cast<u32>(m_extent->value() * 2);
    const auto cs = static_cast<f32>(m_cell->value());
    const f32 ox = originX(), oz = originZ();
    struct Box {
        f32 x0, x1, z0, z1;
    };
    std::vector<Box> boxes;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_host.editorScene().registry().each<ecs::Transform, ecs::Collider>(
            [&](ecs::EntityID, ecs::Transform& t, ecs::Collider& c) {
                if (c.is_trigger) return;
                f32 h[3] = {0.f, 0.f, 0.f};
                switch (c.shape) {
                case ecs::Collider::Sphere: h[0] = h[1] = h[2] = c.params.x; break;
                case ecs::Collider::Box: h[0] = c.params.x; h[1] = c.params.y; h[2] = c.params.z; break;
                case ecs::Collider::Capsule: h[0] = h[2] = c.params.x; h[1] = c.params.x + c.params.y; break;
                default: return;
                }
                const auto& m = t.local_to_world.data;
                // World AABB of the oriented box: |M| * h around the translation.
                const f32 ex = std::fabs(m[0]) * h[0] + std::fabs(m[4]) * h[1] + std::fabs(m[8]) * h[2];
                const f32 ez = std::fabs(m[2]) * h[0] + std::fabs(m[6]) * h[1] + std::fabs(m[10]) * h[2];
                const f32 ey = std::fabs(m[1]) * h[0] + std::fabs(m[5]) * h[1] + std::fabs(m[9]) * h[2];
                // Ground-level geometry (floor slabs below y = 0.05) is walkable, not an obstacle.
                if (m[13] + ey <= 0.05f) return;
                boxes.push_back({m[12] - ex, m[12] + ex, m[14] - ez, m[14] + ez});
            });
    }
    m_blockedCells.assign(static_cast<usize>(n) * n, 0u);
    m_blocked = 0;
    const bool ok = m_mesh.build(n, n, cs, [&](u32 x, u32 y) {
        const f32 cx = ox + (static_cast<f32>(x) + 0.5f) * cs, cz = oz + (static_cast<f32>(y) + 0.5f) * cs;
        for (const Box& b : boxes) {
            if (cx >= b.x0 && cx <= b.x1 && cz >= b.z0 && cz <= b.z1) {
                m_blockedCells[static_cast<usize>(y) * n + x] = 1u;
                ++m_blocked;
                return true;
            }
        }
        return false;
    });
    m_path.clear();
    m_status->setText(ok ? tr("Baked %1 x %1 cells (%2 m): %3 blocked by %4 collider(s)")
                               .arg(n)
                               .arg(cs)
                               .arg(m_blocked)
                               .arg(boxes.size())
                         : tr("Bake failed: invalid grid"));
    refreshOverlay();
    return ok;
}

int NavmeshPanel::testPath(float ax, float az, float bx, float bz) {
    m_path.clear();
    if (!m_mesh.built()) {
        m_status->setText(tr("Bake the navmesh first"));
        return 0;
    }
    const f32 ox = originX(), oz = originZ();
    navigation::Cell a, b;
    if (!m_mesh.cell_at(ax - ox, az - oz, a) || !m_mesh.cell_at(bx - ox, bz - oz, b)) {
        m_status->setText(tr("Path endpoints are outside the navmesh"));
        refreshOverlay();
        return 0;
    }
    const auto points = m_mesh.find_path(a, b);
    for (const auto& p : points) {
        m_path.push_back(ox + p.x);
        m_path.push_back(oz + p.y);
    }
    m_status->setText(points.empty() ? tr("No path") : tr("Path: %1 waypoints, %2 m")
                                                           .arg(points.size())
                                                           .arg((points.size() - 1) * m_mesh.cell_size()));
    refreshOverlay();
    return static_cast<int>(points.size());
}

void NavmeshPanel::refreshOverlay() {
    if (m_viewport == nullptr) return;
    std::vector<f32> segs;
    if (m_show->isChecked() && m_mesh.built()) {
        const u32 w = m_mesh.width(), h = m_mesh.height();
        const f32 cs = m_mesh.cell_size(), ox = originX(), oz = originZ(), y = 0.02f;
        const auto seg = [&](f32 ax, f32 az, f32 bx, f32 bz, f32 yy) {
            segs.insert(segs.end(), {ax, yy, az, bx, yy, bz});
        };
        const f32 x1 = ox + static_cast<f32>(w) * cs, z1 = oz + static_cast<f32>(h) * cs;
        seg(ox, oz, x1, oz, y);
        seg(x1, oz, x1, z1, y);
        seg(x1, z1, ox, z1, y);
        seg(ox, z1, ox, oz, y);
        for (u32 cy = 0; cy < h; ++cy) {
            for (u32 cx = 0; cx < w; ++cx) {
                if (m_blockedCells[static_cast<usize>(cy) * w + cx] == 0u) continue;
                const f32 x0 = ox + static_cast<f32>(cx) * cs, z0 = oz + static_cast<f32>(cy) * cs;
                seg(x0, z0, x0 + cs, z0 + cs, y);
                seg(x0 + cs, z0, x0, z0 + cs, y);
            }
        }
        for (usize i = 0; i + 3 < m_path.size(); i += 2) seg(m_path[i], m_path[i + 1], m_path[i + 2], m_path[i + 3], 0.1f);
    }
    m_viewport->setNavOverlay(std::move(segs));
}

} // namespace fuse::editor::qt
