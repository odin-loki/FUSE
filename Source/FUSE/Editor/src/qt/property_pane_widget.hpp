#pragma once

#include <fuse/editor/feature_pane_bridge.hpp>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace fuse::editor::qt {

/// WP-08 property pane — posts play/stop commands through FeaturePaneBridge on the UI thread.
class PropertyPaneWidget final : public QWidget {
    Q_OBJECT

public:
    explicit PropertyPaneWidget(FeaturePaneBridge& bridge, QWidget* parent = nullptr);

    void refresh();

private:
    void onPlayClicked();
    void onStopClicked();

    FeaturePaneBridge& m_bridge;
    QLabel* m_summaryLabel = nullptr;
    QPushButton* m_playButton = nullptr;
    QPushButton* m_stopButton = nullptr;
};

} // namespace fuse::editor::qt
