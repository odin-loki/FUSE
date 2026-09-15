#pragma once

#include <fuse/editor/editor_host.hpp>

#include <QThread>

namespace fuse::editor::qt {

/// Runs EditorHost::gameTick() on a dedicated game thread via QTimer (~60 Hz).
class GameLoopThread final : public QThread {
    Q_OBJECT

public:
    explicit GameLoopThread(EditorHost* host, QObject* parent = nullptr);

protected:
    void run() override;

private:
    EditorHost* m_host = nullptr;
};

} // namespace fuse::editor::qt
