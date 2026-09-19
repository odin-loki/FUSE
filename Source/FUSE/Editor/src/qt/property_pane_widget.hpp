#pragma once

#include <fuse/editor/feature_pane_bridge.hpp>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

class QDoubleSpinBox;

namespace fuse::editor::qt {

/// WP-08 property pane — posts play/stop and live property edits through FeaturePaneBridge.
class PropertyPaneWidget final : public QWidget {
    Q_OBJECT

public:
    explicit PropertyPaneWidget(FeaturePaneBridge& bridge, QWidget* parent = nullptr);

    void refresh();

private:
    void onPlayClicked();
    void onStopClicked();
    void onPositionEdited();
    void syncPositionFields();

    FeaturePaneBridge& m_bridge;
    QLabel* m_summaryLabel = nullptr;
    QDoubleSpinBox* m_posX = nullptr;
    QDoubleSpinBox* m_posY = nullptr;
    QDoubleSpinBox* m_posZ = nullptr;
    QPushButton* m_playButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    bool m_syncingFields = false;
};

} // namespace fuse::editor::qt
