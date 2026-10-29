#include "timeline_widget.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QPainterPath>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace fuse::editor::qt {

TimelineWidget::TimelineWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent)
    : QWidget(parent), m_host(host), m_sceneMutex(sceneMutex) {
    setObjectName(QStringLiteral("fuseTimeline"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout();
    m_playButton = new QPushButton(tr("Play"));
    m_playButton->setObjectName(QStringLiteral("fuseTimelinePlay"));
    m_playButton->setCheckable(true);
    m_keyButton = new QPushButton(tr("Key Selection"));
    m_keyButton->setObjectName(QStringLiteral("fuseTimelineKey"));
    m_keyButton->setToolTip(tr("Key the selected entity's position / rotation / scale at the playhead (K)"));
    m_deleteButton = new QPushButton(tr("Delete Key"));
    m_deleteButton->setObjectName(QStringLiteral("fuseTimelineDeleteKey"));
    m_timeSpin = new QSpinBox();
    m_timeSpin->setObjectName(QStringLiteral("fuseTimelineTime"));
    m_timeSpin->setSuffix(tr(" ms"));
    m_timeSpin->setSingleStep(100);
    m_durationSpin = new QSpinBox();
    m_durationSpin->setObjectName(QStringLiteral("fuseTimelineDuration"));
    m_durationSpin->setRange(100, 600000);
    m_durationSpin->setSingleStep(500);
    m_durationSpin->setSuffix(tr(" ms"));
    m_status = new QLabel();
    m_interpCombo = new QComboBox();
    m_interpCombo->setObjectName(QStringLiteral("fuseTimelineInterp"));
    m_interpCombo->setToolTip(tr("Interpolation leaving the selected key"));
    m_interpCombo->addItems({tr("Constant"), tr("Linear"), tr("Cubic (auto)"), tr("Cubic (flat)"), tr("Cubic (user)")});
    m_inTangent = new QDoubleSpinBox();
    m_inTangent->setObjectName(QStringLiteral("fuseTimelineInTangent"));
    m_outTangent = new QDoubleSpinBox();
    m_outTangent->setObjectName(QStringLiteral("fuseTimelineOutTangent"));
    for (QDoubleSpinBox* sb : {m_inTangent, m_outTangent}) {
        sb->setRange(-100000.0, 100000.0);
        sb->setDecimals(3);
        sb->setSingleStep(0.5);
        sb->setToolTip(tr("Tangent slope (units per second); editing switches the key to user tangents"));
    }
    bar->addWidget(m_playButton);
    bar->addWidget(m_keyButton);
    bar->addWidget(m_deleteButton);
    bar->addWidget(new QLabel(tr("Time")));
    bar->addWidget(m_timeSpin);
    bar->addWidget(new QLabel(tr("Length")));
    bar->addWidget(m_durationSpin);
    bar->addWidget(m_interpCombo);
    bar->addWidget(new QLabel(tr("In")));
    bar->addWidget(m_inTangent);
    bar->addWidget(new QLabel(tr("Out")));
    bar->addWidget(m_outTangent);
    bar->addStretch(1);
    bar->addWidget(m_status);
    root->addLayout(bar);
    m_canvas = new TimelineCanvas(*this);
    auto* scroll = new QScrollArea();
    scroll->setWidget(m_canvas);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll, 1);

    connect(m_keyButton, &QPushButton::clicked, this, [this]() { keySelection(); });
    connect(m_deleteButton, &QPushButton::clicked, this, [this]() { deleteSelectedKey(); });
    connect(m_playButton, &QPushButton::toggled, this, [this](bool on) { setPlaying(on); });
    connect(m_timeSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int ms) {
        if (ms != m_host.timeline().playheadMs()) {
            scrubTo(ms);
        }
    });
    connect(m_durationSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int ms) {
        if (ms != m_host.timeline().durationMs()) {
            m_host.timeline().setDurationMs(ms);
            m_host.timeline().touch();
            refresh(true);
        }
    });
    connect(m_interpCombo, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        using cinematics::KeyInterp;
        using cinematics::TangentMode;
        static constexpr std::pair<KeyInterp, TangentMode> kModes[] = {
            {KeyInterp::Constant, TangentMode::Auto}, {KeyInterp::Linear, TangentMode::Auto},
            {KeyInterp::Cubic, TangentMode::Auto},    {KeyInterp::Cubic, TangentMode::Flat},
            {KeyInterp::Cubic, TangentMode::User}};
        if (i >= 0 && i < 5) {
            setSelectedKeyInterp(kModes[i].first, kModes[i].second);
        }
    });
    auto tangentEdited = [this]() { setSelectedKeyTangents(static_cast<float>(m_inTangent->value()), static_cast<float>(m_outTangent->value())); };
    connect(m_inTangent, &QDoubleSpinBox::editingFinished, this, tangentEdited);
    connect(m_outTangent, &QDoubleSpinBox::editingFinished, this, tangentEdited);
    m_playTimer.setInterval(16);
    connect(&m_playTimer, &QTimer::timeout, this, &TimelineWidget::onPlayTick_);
    refresh(true);
}

void TimelineWidget::refresh(bool force) {
    EditorTimeline& tl = m_host.timeline();
    if (!force && tl.revision() == m_seenRevision) {
        return;
    }
    m_seenRevision = tl.revision();
    if (m_selTrack >= static_cast<int>(tl.tracks().size()) ||
        (m_selTrack >= 0 && m_selKey >= static_cast<int>(tl.tracks()[static_cast<size_t>(m_selTrack)].keyframes().size()))) {
        m_selTrack = m_selKey = -1;
    }
    const QSignalBlocker b1(m_timeSpin), b2(m_durationSpin);
    m_durationSpin->setValue(tl.durationMs());
    m_timeSpin->setRange(0, tl.durationMs());
    m_timeSpin->setValue(tl.playheadMs());
    m_deleteButton->setEnabled(m_selKey >= 0);
    syncCurveControls_();
    m_status->setText(tr("%1 lanes · %2 keys").arg(tl.tracks().size()).arg(tl.keyCount()));
    m_canvas->setMinimumHeight(TimelineCanvas::kRulerHeight +
                               TimelineCanvas::kLaneHeight * static_cast<int>(std::max<size_t>(tl.tracks().size(), 1)) + 4 +
                               TimelineCanvas::kCurveHeight);
    m_canvas->update();
}

bool TimelineWidget::keySelection() {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorTimeline& tl = m_host.timeline();
        const ecs::EntityID e = m_host.editorState().primarySelection;
        EditorTimeline::Tracks before = tl.tracks();
        ok = tl.keyTransform(m_host.editorScene().registry(), e, tl.playheadMs());
        if (ok) {
            m_host.undoStack().execute(std::make_unique<TimelineEditCommand>(
                tl, std::move(before), tl.tracks(), "Key transform @ " + std::to_string(tl.playheadMs()) + " ms"));
        }
    }
    refresh(true);
    if (ok) {
        emit sceneEdited();
    }
    return ok;
}

void TimelineWidget::applyAtPlayhead_() {
    u32 touched = 0;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        // Preview only in edit mode: PIE owns its own registry.
        if (m_host.playSession().isActive()) {
            return;
        }
        touched = m_host.timeline().apply(m_host.editorScene().registry(), m_host.timeline().playheadMs());
    }
    if (touched > 0u) {
        emit sceneEdited();
    }
}

void TimelineWidget::scrubTo(int ms) {
    m_host.timeline().setPlayheadMs(ms);
    applyAtPlayhead_();
    refresh(true);
}

void TimelineWidget::setPlaying(bool on) {
    if (on == m_playTimer.isActive()) {
        return;
    }
    {
        const QSignalBlocker block(m_playButton);
        m_playButton->setChecked(on);
        m_playButton->setText(on ? tr("Pause") : tr("Play"));
    }
    if (on) {
        if (m_host.timeline().playheadMs() >= m_host.timeline().durationMs()) {
            m_host.timeline().setPlayheadMs(0);
        }
        m_playClock.start();
        m_playTimer.start();
    } else {
        m_playTimer.stop();
    }
}

void TimelineWidget::onPlayTick_() {
    const qint64 dt = m_playClock.restart();
    EditorTimeline& tl = m_host.timeline();
    int next = tl.playheadMs() + static_cast<int>(dt);
    if (next >= tl.durationMs()) {
        next = tl.durationMs();
        setPlaying(false);
    }
    scrubTo(next);
}

bool TimelineWidget::selectKey(int track, int key) {
    const EditorTimeline& tl = m_host.timeline();
    if (track < 0 || track >= static_cast<int>(tl.tracks().size()) || key < 0 ||
        key >= static_cast<int>(tl.tracks()[static_cast<size_t>(track)].keyframes().size())) {
        m_selTrack = m_selKey = -1;
        refresh(true);
        return false;
    }
    m_selTrack = track;
    m_selKey = key;
    refresh(true);
    return true;
}

void TimelineWidget::beginKeyDrag_() {
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    m_dragBefore = m_host.timeline().tracks();
    m_dragging = true;
}

void TimelineWidget::dragKeyTo_(int ms) {
    if (!m_dragging || m_selKey < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        m_selKey = m_host.timeline().moveKey(m_selTrack, m_selKey, ms);
    }
    refresh(true);
}

void TimelineWidget::endKeyDrag_() {
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorTimeline& tl = m_host.timeline();
        if (tl.tracks().size() != m_dragBefore.size() ||
            !std::equal(tl.tracks().begin(), tl.tracks().end(), m_dragBefore.begin(),
                        [](const cinematics::PropertyTrack& a, const cinematics::PropertyTrack& b) {
                            if (a.keyframes().size() != b.keyframes().size()) {
                                return false;
                            }
                            for (size_t i = 0; i < a.keyframes().size(); ++i) {
                                if (a.keyframes()[i].time_ms != b.keyframes()[i].time_ms) {
                                    return false;
                                }
                            }
                            return true;
                        })) {
            m_host.undoStack().execute(
                std::make_unique<TimelineEditCommand>(tl, std::move(m_dragBefore), tl.tracks(), "Move key"));
        }
        m_dragBefore.clear();
    }
    applyAtPlayhead_();
    refresh(true);
    emit sceneEdited();
}

bool TimelineWidget::moveSelectedKey(int ms) {
    if (m_selKey < 0) {
        return false;
    }
    beginKeyDrag_();
    dragKeyTo_(ms);
    endKeyDrag_();
    return m_selKey >= 0;
}

bool TimelineWidget::deleteSelectedKey() {
    if (m_selKey < 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorTimeline& tl = m_host.timeline();
        EditorTimeline::Tracks before = tl.tracks();
        if (!tl.deleteKey(m_selTrack, m_selKey)) {
            return false;
        }
        m_host.undoStack().execute(std::make_unique<TimelineEditCommand>(tl, std::move(before), tl.tracks(), "Delete key"));
    }
    m_selTrack = m_selKey = -1;
    refresh(true);
    emit sceneEdited();
    return true;
}

void TimelineWidget::syncCurveControls_() {
    const bool has = m_selKey >= 0;
    m_interpCombo->setEnabled(has);
    m_inTangent->setEnabled(has);
    m_outTangent->setEnabled(has);
    if (!has) {
        return;
    }
    const auto& track = m_host.timeline().tracks()[static_cast<size_t>(m_selTrack)];
    const auto& k = track.keyframes()[static_cast<size_t>(m_selKey)];
    int index = 1;
    if (k.interp == cinematics::KeyInterp::Constant) {
        index = 0;
    } else if (k.interp == cinematics::KeyInterp::Cubic) {
        index = k.tangent_mode == cinematics::TangentMode::Flat ? 3 : k.tangent_mode == cinematics::TangentMode::User ? 4 : 2;
    }
    float in = 0.f;
    float out = 0.f;
    track.effective_tangents(static_cast<size_t>(m_selKey), in, out);
    const QSignalBlocker b0(m_interpCombo), b1(m_inTangent), b2(m_outTangent);
    m_interpCombo->setCurrentIndex(index);
    if (!m_inTangent->hasFocus()) {
        m_inTangent->setValue(in);
    }
    if (!m_outTangent->hasFocus()) {
        m_outTangent->setValue(out);
    }
}

bool TimelineWidget::editSelectedKey_(const char* what, const std::function<bool(EditorTimeline&)>& edit) {
    if (m_selKey < 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_sceneMutex);
        EditorTimeline& tl = m_host.timeline();
        EditorTimeline::Tracks before = tl.tracks();
        if (!edit(tl)) {
            return false;
        }
        m_host.undoStack().execute(std::make_unique<TimelineEditCommand>(tl, std::move(before), tl.tracks(), what));
    }
    applyAtPlayhead_();
    refresh(true);
    emit sceneEdited();
    return true;
}

bool TimelineWidget::setSelectedKeyInterp(cinematics::KeyInterp interp, cinematics::TangentMode mode) {
    const int track = m_selTrack;
    const int key = m_selKey;
    return editSelectedKey_("Key interpolation",
                            [&](EditorTimeline& tl) { return tl.setKeyInterp(track, key, interp, mode); });
}

bool TimelineWidget::setSelectedKeyTangents(float inSlope, float outSlope) {
    const int track = m_selTrack;
    const int key = m_selKey;
    return editSelectedKey_("Key tangents",
                            [&](EditorTimeline& tl) { return tl.setKeyTangents(track, key, inSlope, outSlope); });
}

float TimelineWidget::sampleSelectedTrack(int ms) const {
    if (m_selTrack < 0 || m_selTrack >= static_cast<int>(m_host.timeline().tracks().size())) {
        return 0.f;
    }
    return m_host.timeline().tracks()[static_cast<size_t>(m_selTrack)].sample_at(ms);
}

// ---- canvas ----------------------------------------------------------------------------------------

TimelineCanvas::TimelineCanvas(TimelineWidget& owner) : QWidget(&owner), m_owner(owner) {
    setObjectName(QStringLiteral("fuseTimelineCanvas"));
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);
}

int TimelineCanvas::msToX(int ms) const {
    const int span = std::max(1, width() - kHeaderWidth - 12);
    return kHeaderWidth + static_cast<int>(std::lround(static_cast<double>(ms) * span / m_owner.m_host.timeline().durationMs()));
}

int TimelineCanvas::xToMs(int x) const {
    const int span = std::max(1, width() - kHeaderWidth - 12);
    const double ms = static_cast<double>(x - kHeaderWidth) * m_owner.m_host.timeline().durationMs() / span;
    return std::clamp(static_cast<int>(std::lround(ms / 10.0) * 10), 0, m_owner.m_host.timeline().durationMs());
}

int TimelineCanvas::curveTop() const {
    return kRulerHeight + kLaneHeight * static_cast<int>(std::max<size_t>(m_owner.m_host.timeline().tracks().size(), 1)) + 4;
}

QPoint TimelineCanvas::keyPos(int track, int key) const {
    const auto& t = m_owner.m_host.timeline().tracks()[static_cast<size_t>(track)];
    return {msToX(t.keyframes()[static_cast<size_t>(key)].time_ms), kRulerHeight + track * kLaneHeight + kLaneHeight / 2};
}

void TimelineCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 33, 40));
    const EditorTimeline& tl = m_owner.m_host.timeline();
    // Ruler.
    p.fillRect(QRect(0, 0, width(), kRulerHeight), QColor(40, 44, 54));
    p.setPen(QColor(150, 160, 175));
    const int dur = tl.durationMs();
    const int step = dur <= 2000 ? 250 : dur <= 10000 ? 1000 : 5000;
    for (int ms = 0; ms <= dur; ms += step) {
        const int x = msToX(ms);
        p.drawLine(x, kRulerHeight - 6, x, kRulerHeight);
        p.drawText(x + 2, kRulerHeight - 8, QString::number(ms / 1000.0, 'f', ms % 1000 ? 2 : 0) + "s");
    }
    // Lanes.
    for (size_t i = 0; i < tl.tracks().size(); ++i) {
        const auto& t = tl.tracks()[i];
        const int y = kRulerHeight + static_cast<int>(i) * kLaneHeight;
        p.fillRect(QRect(0, y, width(), kLaneHeight), (i % 2) ? QColor(34, 38, 46) : QColor(38, 42, 51));
        p.setPen(QColor(190, 198, 210));
        p.drawText(QRect(6, y, kHeaderWidth - 10, kLaneHeight), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("#%1  %2").arg(t.target_object_id().c_str(), t.property_path().c_str()));
        for (size_t k = 0; k < t.keyframes().size(); ++k) {
            const QPoint c = keyPos(static_cast<int>(i), static_cast<int>(k));
            const bool sel = m_owner.m_selTrack == static_cast<int>(i) && m_owner.m_selKey == static_cast<int>(k);
            p.setPen(QPen(QColor(20, 20, 20), 1));
            p.setBrush(sel ? QColor(255, 220, 60) : QColor(120, 180, 255));
            switch (t.keyframes()[k].interp) {
            case cinematics::KeyInterp::Constant: // square: step
                p.drawRect(QRect(c - QPoint(5, 5), QSize(10, 10)));
                break;
            case cinematics::KeyInterp::Cubic: // circle: smooth
                p.drawEllipse(c, 6, 6);
                break;
            case cinematics::KeyInterp::Linear: // diamond
                p.drawPolygon(QPolygon({c + QPoint(0, -6), c + QPoint(6, 0), c + QPoint(0, 6), c + QPoint(-6, 0)}));
                break;
            }
        }
    }
    if (tl.tracks().empty()) {
        p.setPen(QColor(140, 150, 165));
        p.drawText(QRect(kHeaderWidth, kRulerHeight, width() - kHeaderWidth, kLaneHeight * 2), Qt::AlignVCenter,
                   tr("Select an entity and press Key Selection to add keyframes."));
    }
    // Curve strip: the selected key's lane, value over time, with the selected key's tangent handles.
    if (m_owner.m_selTrack >= 0 && m_owner.m_selTrack < static_cast<int>(tl.tracks().size())) {
        const auto& t = tl.tracks()[static_cast<size_t>(m_owner.m_selTrack)];
        const QRect strip(kHeaderWidth, curveTop(), width() - kHeaderWidth - 12, kCurveHeight - 8);
        p.fillRect(QRect(0, strip.top(), width(), strip.height()), QColor(26, 29, 35));
        p.setPen(QColor(150, 160, 175));
        p.drawText(QRect(6, strip.top(), kHeaderWidth - 10, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Curve #%1 %2").arg(t.target_object_id().c_str(), t.property_path().c_str()));
        float lo = 1e30f;
        float hi = -1e30f;
        const int samples = std::max(2, strip.width() / 2);
        std::vector<float> values(static_cast<size_t>(samples) + 1);
        for (int i = 0; i <= samples; ++i) {
            values[static_cast<size_t>(i)] = t.sample_at(static_cast<cinematics::TimelineMs>(
                static_cast<long long>(i) * dur / samples));
            lo = std::min(lo, values[static_cast<size_t>(i)]);
            hi = std::max(hi, values[static_cast<size_t>(i)]);
        }
        if (hi - lo < 1e-4f) {
            hi += 0.5f;
            lo -= 0.5f;
        }
        auto toY = [&](float v) { return strip.bottom() - 4 - (v - lo) / (hi - lo) * (strip.height() - 8); };
        QPainterPath path;
        for (int i = 0; i <= samples; ++i) {
            const QPointF pt(msToX(static_cast<int>(static_cast<long long>(i) * dur / samples)), toY(values[static_cast<size_t>(i)]));
            i == 0 ? path.moveTo(pt) : path.lineTo(pt);
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(120, 220, 140), 1.6));
        p.drawPath(path);
        if (m_owner.m_selKey >= 0) {
            const auto& k = t.keyframes()[static_cast<size_t>(m_owner.m_selKey)];
            float in = 0.f;
            float out = 0.f;
            t.effective_tangents(static_cast<size_t>(m_owner.m_selKey), in, out);
            const QPointF c(msToX(k.time_ms), toY(k.value));
            const double pxPerMs = static_cast<double>(std::max(1, strip.width())) / std::max(1, dur);
            const double pxPerUnit = (strip.height() - 8) / static_cast<double>(hi - lo);
            auto handle = [&](float slope, double sign) {
                // 30 px long handle in screen space along the slope.
                const double dx = 1.0;
                const double dy = -slope / 1000.0 / pxPerMs * pxPerUnit;
                const double len = std::sqrt(dx * dx + dy * dy);
                return c + QPointF(sign * 30.0 * dx / len, sign * 30.0 * dy / len);
            };
            p.setPen(QPen(QColor(255, 220, 60), 1));
            const QPointF hin = handle(in, -1.0);
            const QPointF hout = handle(out, 1.0);
            p.drawLine(hin, c);
            p.drawLine(c, hout);
            p.setBrush(QColor(255, 220, 60));
            p.drawEllipse(hin, 3, 3);
            p.drawEllipse(hout, 3, 3);
            p.drawEllipse(c, 4, 4);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    // Playhead.
    const int px = msToX(tl.playheadMs());
    p.setPen(QPen(QColor(240, 90, 80), 2));
    p.drawLine(px, 0, px, height());
}

void TimelineCanvas::mousePressEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    if (e->button() != Qt::LeftButton) {
        return;
    }
    if (pos.y() < kRulerHeight) {
        m_scrubbing = true;
        m_owner.scrubTo(xToMs(pos.x()));
        return;
    }
    const auto& tracks = m_owner.m_host.timeline().tracks();
    const int lane = (pos.y() - kRulerHeight) / kLaneHeight;
    if (lane >= 0 && lane < static_cast<int>(tracks.size())) {
        for (size_t k = 0; k < tracks[static_cast<size_t>(lane)].keyframes().size(); ++k) {
            if (std::abs(keyPos(lane, static_cast<int>(k)).x() - pos.x()) <= 6) {
                m_owner.selectKey(lane, static_cast<int>(k));
                m_owner.beginKeyDrag_();
                return;
            }
        }
    }
    m_owner.selectKey(-1, -1);
}

void TimelineCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (m_scrubbing) {
        m_owner.scrubTo(xToMs(e->position().toPoint().x()));
    } else if (m_owner.m_dragging) {
        m_owner.dragKeyTo_(xToMs(e->position().toPoint().x()));
    }
}

void TimelineCanvas::mouseReleaseEvent(QMouseEvent*) {
    m_scrubbing = false;
    m_owner.endKeyDrag_();
}

void TimelineCanvas::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        m_owner.deleteSelectedKey();
    } else if (e->key() == Qt::Key_K) {
        m_owner.keySelection();
    } else if (e->key() == Qt::Key_Space) {
        m_owner.setPlaying(!m_owner.playing());
    } else {
        QWidget::keyPressEvent(e);
    }
}

} // namespace fuse::editor::qt
