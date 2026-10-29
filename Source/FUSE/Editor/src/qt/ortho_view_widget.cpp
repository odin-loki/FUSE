#include "ortho_view_widget.hpp"

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::editor::qt {

OrthoViewWidget::OrthoViewWidget(EditorHost& host, std::mutex& sceneMutex, Axis axis, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex), m_axis(axis) {
    setObjectName(QStringLiteral("fuseOrthoView%1").arg(axisName()));
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);
    setMinimumSize(120, 90);
    setToolTip(tr("%1 view: click selects, drag moves the selection, middle/right drag pans, wheel zooms, F frames")
                   .arg(axisName()));
}

QString OrthoViewWidget::axisName() const {
    switch (m_axis) {
    case Axis::Top: return QStringLiteral("Top");
    case Axis::Front: return QStringLiteral("Front");
    case Axis::Side: return QStringLiteral("Side");
    }
    return {};
}

// Top: looking down -Y, u = X, v = Z (screen down = +Z). Front: looking down -Z, u = X, v = Y (up).
// Side: looking down -X (from +X), u = -Z, v = Y.
void OrthoViewWidget::uv_(const ecs::vec3& p, float& u, float& v) const {
    switch (m_axis) {
    case Axis::Top: u = p.x; v = -p.z; break;
    case Axis::Front: u = p.x; v = p.y; break;
    case Axis::Side: u = -p.z; v = p.y; break;
    }
}

QPointF OrthoViewWidget::project_(float x, float y, float z) const {
    float u = 0.f, v = 0.f;
    uv_(ecs::vec3{x, y, z, 1.f}, u, v);
    return {width() * 0.5 + (u - m_centerU) * m_scale, height() * 0.5 - (v - m_centerV) * m_scale};
}

QPointF OrthoViewWidget::worldToView(const ecs::vec3& p) const { return project_(p.x, p.y, p.z); }

ecs::vec3 OrthoViewWidget::viewToWorld(QPointF pos, const ecs::vec3& depthFrom) const {
    const float u = m_centerU + static_cast<float>((pos.x() - width() * 0.5) / m_scale);
    const float v = m_centerV - static_cast<float>((pos.y() - height() * 0.5) / m_scale);
    ecs::vec3 out = depthFrom;
    switch (m_axis) {
    case Axis::Top: out.x = u; out.z = -v; break;
    case Axis::Front: out.x = u; out.y = v; break;
    case Axis::Side: out.z = -u; out.y = v; break;
    }
    return out;
}

void OrthoViewWidget::setPixelsPerUnit(float s) {
    m_scale = std::clamp(s, 0.5f, 2000.f);
    update();
}

void OrthoViewWidget::setCenter(float u, float v) {
    m_framed = true;
    m_centerU = u;
    m_centerV = v;
    update();
}

void OrthoViewWidget::collect_() {
    m_items.clear();
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    ecs::Registry& registry = m_host.editorScene().registry();
    ecs::TransformSystem::update(registry);
    const auto& sel = m_host.editorState().selectedEntities;
    const ecs::EntityID primary = m_host.editorState().primarySelection;
    registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
        Item item;
        item.id = id;
        item.selected = id == primary || std::find(sel.begin(), sel.end(), id) != sel.end();
        const auto& m = t.local_to_world.data;
        item.center = project_(m[12], m[13], m[14]);
        const ecs::Mesh* mesh = registry.get<ecs::Mesh>(id);
        if (mesh != nullptr && mesh->aabb_max.x >= mesh->aabb_min.x && mesh->aabb_max.y >= mesh->aabb_min.y &&
            mesh->aabb_max.z >= mesh->aabb_min.z) {
            double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30; // QRectF::united ignores zero-size rects
            for (int c = 0; c < 8; ++c) {
                const float lx = (c & 1) ? mesh->aabb_max.x : mesh->aabb_min.x;
                const float ly = (c & 2) ? mesh->aabb_max.y : mesh->aabb_min.y;
                const float lz = (c & 4) ? mesh->aabb_max.z : mesh->aabb_min.z;
                const QPointF p = project_(m[0] * lx + m[4] * ly + m[8] * lz + m[12], m[1] * lx + m[5] * ly + m[9] * lz + m[13],
                                           m[2] * lx + m[6] * ly + m[10] * lz + m[14]);
                x0 = std::min(x0, p.x());
                y0 = std::min(y0, p.y());
                x1 = std::max(x1, p.x());
                y1 = std::max(y1, p.y());
            }
            item.rect = QRectF(QPointF(x0, y0), QPointF(x1, y1));
        }
        m_items.push_back(item);
    });
    // Selected items last so they draw on top.
    std::stable_partition(m_items.begin(), m_items.end(), [](const Item& i) { return !i.selected; });
}

void OrthoViewWidget::paintEvent(QPaintEvent*) {
    collect_();
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 33, 40));
    // Grid: minor lines every 10^k units so they are >= 12 px apart, major every 10 minor.
    float step = 0.01f;
    while (step * m_scale < 12.f) step *= 10.f;
    const auto drawGrid = [&](float s, QColor c) {
        p.setPen(QPen(c, 1));
        const float u0 = m_centerU - static_cast<float>(width() * 0.5 / m_scale);
        const float u1 = m_centerU + static_cast<float>(width() * 0.5 / m_scale);
        const float v0 = m_centerV - static_cast<float>(height() * 0.5 / m_scale);
        const float v1 = m_centerV + static_cast<float>(height() * 0.5 / m_scale);
        for (float u = std::floor(u0 / s) * s; u <= u1; u += s) {
            const double x = width() * 0.5 + (u - m_centerU) * m_scale;
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
        for (float v = std::floor(v0 / s) * s; v <= v1; v += s) {
            const double y = height() * 0.5 - (v - m_centerV) * m_scale;
            p.drawLine(QPointF(0, y), QPointF(width(), y));
        }
    };
    drawGrid(step, QColor(40, 44, 52));
    drawGrid(step * 10.f, QColor(55, 60, 70));
    // World axes through the origin.
    const QPointF o = project_(0.f, 0.f, 0.f);
    const QColor uColor = m_axis == Axis::Side ? QColor(70, 110, 230) : QColor(220, 70, 70);
    const QColor vColor = m_axis == Axis::Top ? QColor(70, 110, 230) : QColor(80, 200, 90);
    p.setPen(QPen(uColor, 1.5));
    p.drawLine(QPointF(0, o.y()), QPointF(width(), o.y()));
    p.setPen(QPen(vColor, 1.5));
    p.drawLine(QPointF(o.x(), 0), QPointF(o.x(), height()));

    p.setRenderHint(QPainter::Antialiasing, true);
    m_drawn = 0;
    for (const Item& it : m_items) {
        const QColor c = it.selected ? QColor(255, 165, 40) : QColor(150, 175, 205);
        if (!it.rect.isEmpty() || it.rect.width() > 0.5 || it.rect.height() > 0.5) {
            QRectF r = it.rect;
            if (r.width() < 2) r.adjust(-1, 0, 1, 0);
            if (r.height() < 2) r.adjust(0, -1, 0, 1);
            p.setPen(QPen(c, it.selected ? 2.0 : 1.2));
            p.setBrush(QColor(c.red(), c.green(), c.blue(), it.selected ? 50 : 22));
            p.drawRect(r);
        } else {
            p.setPen(QPen(c, it.selected ? 2.0 : 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawLine(it.center + QPointF(-5, 0), it.center + QPointF(5, 0));
            p.drawLine(it.center + QPointF(0, -5), it.center + QPointF(0, 5));
            p.drawEllipse(it.center, 3, 3);
        }
        ++m_drawn;
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    // Label.
    static const char* kAxes[] = {"X / Z", "X / Y", "-Z / Y"};
    const QString label = QStringLiteral("%1  (%2)").arg(axisName(), QLatin1String(kAxes[static_cast<int>(m_axis)]));
    p.setPen(QColor(200, 205, 215));
    p.drawText(QRect(8, 4, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, label);
    if (hasFocus()) {
        p.setPen(QPen(QColor(90, 150, 230), 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

ecs::EntityID OrthoViewWidget::pick(QPointF pos) {
    collect_();
    ecs::EntityID best = ecs::EntityID::null();
    double bestScore = std::numeric_limits<double>::max();
    for (const Item& it : m_items) {
        double score;
        if (it.rect.width() > 1.0 && it.rect.height() > 1.0 && it.rect.adjusted(-3, -3, 3, 3).contains(pos)) {
            score = it.rect.width() * it.rect.height(); // smallest box under the cursor wins
        } else {
            const double d = std::hypot(it.center.x() - pos.x(), it.center.y() - pos.y());
            if (d > 8.0) continue;
            score = d; // markers beat boxes
        }
        if (score < bestScore) {
            bestScore = score;
            best = it.id;
        }
    }
    return best;
}

bool OrthoViewWidget::frameSelection() {
    m_framed = true;
    return frame_(true, 2.0);
}

bool OrthoViewWidget::frameAll() {
    m_framed = true;
    return frame_(false, 8.0);
}

void OrthoViewWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Until the user pans / zooms / frames, keep the whole scene fitted as the splitter settles.
    if (!m_framed && width() > 50 && height() > 50) {
        frame_(true, 8.0, true);
    }
}

bool OrthoViewWidget::frame_(bool selectionOnly, double minUnits, bool skipHuge) {
    collect_();
    QRectF bounds;
    bool any = false;
    const bool selOnly = selectionOnly && std::any_of(m_items.begin(), m_items.end(), [](const Item& i) { return i.selected; });
    // Auto-frame skips huge boxes (ground planes, terrain) unless nothing else exists, so ordinary
    // objects stay readable; frameAll() (Home) still fits everything.
    const auto huge = [&](const Item& i) {
        return std::max(i.rect.width(), i.rect.height()) / m_scale > kHugeExtentUnits;
    };
    const bool skip = skipHuge && !selOnly && std::any_of(m_items.begin(), m_items.end(), [&](const Item& i) { return !huge(i); });
    for (const Item& it : m_items) {
        if (selOnly && !it.selected) continue;
        if (skip && huge(it)) continue;
        const QRectF r = it.rect.width() > 0.5 || it.rect.height() > 0.5 ? it.rect : QRectF(it.center - QPointF(4, 4), QSizeF(8, 8));
        bounds = any ? bounds.united(r) : r;
        any = true;
    }
    if (!any) return false;
    // Convert the pixel bounds back to view units, then centre and fit with a margin.
    const float cu = m_centerU + static_cast<float>((bounds.center().x() - width() * 0.5) / m_scale);
    const float cv = m_centerV - static_cast<float>((bounds.center().y() - height() * 0.5) / m_scale);
    const double wUnits = std::max(bounds.width() / m_scale, minUnits);
    const double hUnits = std::max(bounds.height() / m_scale, minUnits);
    m_centerU = cu;
    m_centerV = cv;
    setPixelsPerUnit(static_cast<float>(std::min(width() / (wUnits * 1.6), height() / (hUnits * 1.6))));
    return true;
}

bool OrthoViewWidget::dragSelection(QPointF from, QPointF to) {
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        ecs::Registry& registry = m_host.editorScene().registry();
        const ecs::EntityID id = m_host.editorState().primarySelection;
        const ecs::Transform* t = id.valid() && registry.alive(id) ? registry.get<ecs::Transform>(id) : nullptr;
        if (t != nullptr && !m_host.editorState().playing) {
            const TransformCommand::State before = TransformCommand::capture(*t);
            TransformCommand::State after = before;
            const ecs::vec3 a = viewToWorld(from, before.position);
            const ecs::vec3 b = viewToWorld(to, before.position);
            after.position.x += b.x - a.x;
            after.position.y += b.y - a.y;
            after.position.z += b.z - a.z;
            m_host.undoStack().execute(std::make_unique<TransformCommand>(registry, id, before, after));
            m_host.editorState().sceneModified = true;
            moved = true;
        }
    }
    if (moved) {
        emit sceneEdited();
        update();
    }
    return moved;
}

void OrthoViewWidget::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    m_last = pos;
    m_pressPos = pos;
    if (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton) {
        m_drag = Drag::Pan;
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    const ecs::EntityID hit = pick(pos);
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorState& st = m_host.editorState();
        const bool additive = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (!additive) {
            const bool alreadySel = hit.valid() && std::find(st.selectedEntities.begin(), st.selectedEntities.end(), hit) != st.selectedEntities.end();
            if (!alreadySel) st.selectedEntities.clear();
            if (hit.valid()) {
                if (!alreadySel) st.selectedEntities.push_back(hit);
                st.primarySelection = hit;
            } else {
                st.primarySelection = ecs::EntityID::null();
            }
        } else if (hit.valid()) {
            auto& sel = st.selectedEntities;
            const auto it = std::find(sel.begin(), sel.end(), hit);
            if (it != sel.end()) {
                sel.erase(it);
                if (st.primarySelection == hit) st.primarySelection = sel.empty() ? ecs::EntityID::null() : sel.back();
            } else {
                sel.push_back(hit);
                st.primarySelection = hit;
            }
        }
    }
    emit selectionChanged();
    update();
    if (hit.valid() && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
        m_drag = Drag::Move;
        m_dragEntity = hit;
    }
}

void OrthoViewWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (m_drag == Drag::Pan) {
        m_framed = true;
        m_centerU -= static_cast<float>((pos.x() - m_last.x()) / m_scale);
        m_centerV += static_cast<float>((pos.y() - m_last.y()) / m_scale);
        update();
    } else if (m_drag == Drag::Move) {
        if (!m_macroOpen) {
            if (std::hypot(pos.x() - m_pressPos.x(), pos.y() - m_pressPos.y()) < 3.0) return;
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_host.undoStack().beginMacro("Move entity (" + axisName().toStdString() + " view)");
            m_macroOpen = true;
        }
        dragSelection(m_last, pos);
    }
    m_last = pos;
}

void OrthoViewWidget::mouseReleaseEvent(QMouseEvent*) {
    if (m_macroOpen) {
        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_host.undoStack().endMacro();
        }
        m_macroOpen = false;
        emit sceneEdited(); // outside the lock: listeners read the scene
    }
    m_drag = Drag::None;
    m_dragEntity = ecs::EntityID::null();
}

void OrthoViewWidget::wheelEvent(QWheelEvent* e) {
    const int d = e->angleDelta().y();
    if (d == 0) return;
    const QPointF pos = e->position();
    m_framed = true;
    const ecs::vec3 before = viewToWorld(pos, ecs::vec3{0.f, 0.f, 0.f, 1.f});
    setPixelsPerUnit(m_scale * (d > 0 ? 1.2f : 1.f / 1.2f));
    const ecs::vec3 after = viewToWorld(pos, ecs::vec3{0.f, 0.f, 0.f, 1.f});
    float ub = 0.f, vb = 0.f, ua = 0.f, va = 0.f;
    uv_(before, ub, vb);
    uv_(after, ua, va);
    m_centerU += ub - ua;
    m_centerV += vb - va;
    update();
    e->accept();
}

void OrthoViewWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F) {
        frameSelection();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace fuse::editor::qt
