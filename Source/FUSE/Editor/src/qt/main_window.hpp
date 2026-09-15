#pragma once

#include <fuse/editor/editor_host.hpp>

#include "game_loop_thread.hpp"
#include "project_hub_widget.hpp"
#include "viewport_placeholder_widget.hpp"

#include <QMainWindow>
#include <QTimer>

class QLabel;
class QStatusBar;

namespace fuse::editor::qt {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString& samplesRoot, QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void onProjectOpenRequested(const QString& projectDirectory);
    void refreshStatusBar();

    EditorHost m_host;
    GameLoopThread m_gameThread;
    ProjectHubWidget* m_projectHub = nullptr;
    ViewportPlaceholderWidget* m_viewport = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTimer m_statusTimer;
};

} // namespace fuse::editor::qt
