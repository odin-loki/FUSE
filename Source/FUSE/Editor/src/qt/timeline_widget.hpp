#pragma once

#include <fuse/editor/editor_host.hpp>

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <mutex>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace fuse::editor::qt {

class TimelineCanvas;

/// GUI-5 Timeline dock: keyframe lanes for the edit scene's Transform channels
/// (`EditorHost::timeline()`). Key the selection at the playhead, scrub / play to preview, drag a
/// key diamond to retime it, Delete removes the selected key; every edit is one UndoStack step.
class TimelineWidget final : public QWidget {
    Q_OBJECT

public:
    TimelineWidget(EditorHost& host, std::mutex& sceneMutex, QWidget* parent = nullptr);

    /// Rebuild lanes / labels from the model (cheap; skips when the revision is unchanged).
    void refresh(bool force = false);

    // ---- test API (all go through the same paths as the UI) ----
    bool keySelection();
    void scrubTo(int ms);
    void setPlaying(bool on);
    [[nodiscard]] bool playing() const { return m_playTimer.isActive(); }
    bool selectKey(int track, int key);
    bool moveSelectedKey(int ms);
    bool deleteSelectedKey();
    /// Curve editing on the selected key (one undo step each).
    bool setSelectedKeyInterp(cinematics::KeyInterp interp, cinematics::TangentMode mode);
    bool setSelectedKeyTangents(float inSlope, float outSlope);
    /// Sampled value of the selected key's lane (what the curve strip draws).
    [[nodiscard]] float sampleSelectedTrack(int ms) const;
    [[nodiscard]] int selectedTrack() const { return m_selTrack; }
    [[nodiscard]] int selectedKey() const { return m_selKey; }
    [[nodiscard]] TimelineCanvas* canvas() const { return m_canvas; }
    [[nodiscard]] EditorTimeline& model() { return m_host.timeline(); }

signals:
    void sceneEdited();

private:
    friend class TimelineCanvas;
    void applyAtPlayhead_();
    void onPlayTick_();
    void beginKeyDrag_();
    void dragKeyTo_(int ms);
    void endKeyDrag_();
    bool editSelectedKey_(const char* what, const std::function<bool(EditorTimeline&)>& edit);
    void syncCurveControls_();

    EditorHost& m_host;
    std::mutex& m_sceneMutex;
    TimelineCanvas* m_canvas = nullptr;
    QPushButton* m_keyButton = nullptr;
    QPushButton* m_playButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    QSpinBox* m_timeSpin = nullptr;
    QSpinBox* m_durationSpin = nullptr;
    QLabel* m_status = nullptr;
    QComboBox* m_interpCombo = nullptr;
    QDoubleSpinBox* m_inTangent = nullptr;
    QDoubleSpinBox* m_outTangent = nullptr;
    QTimer m_playTimer;
    QElapsedTimer m_playClock;
    int m_selTrack = -1;
    int m_selKey = -1;
    bool m_dragging = false;
    EditorTimeline::Tracks m_dragBefore;
    u32 m_seenRevision = ~0u;
};

/// Ruler + lanes; click the ruler to scrub, click / drag a diamond to select / retime.
class TimelineCanvas final : public QWidget {
    Q_OBJECT

public:
    static constexpr int kHeaderWidth = 170;
    static constexpr int kRulerHeight = 22;
    static constexpr int kLaneHeight = 18;
    /// Curve strip under the lanes: the selected key's lane plotted value-over-time.
    static constexpr int kCurveHeight = 90;

    explicit TimelineCanvas(TimelineWidget& owner);
    [[nodiscard]] int msToX(int ms) const;
    [[nodiscard]] int xToMs(int x) const;
    [[nodiscard]] QPoint keyPos(int track, int key) const;
    [[nodiscard]] int curveTop() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override { return {600, 160}; }

private:
    TimelineWidget& m_owner;
    bool m_scrubbing = false;
};

} // namespace fuse::editor::qt
