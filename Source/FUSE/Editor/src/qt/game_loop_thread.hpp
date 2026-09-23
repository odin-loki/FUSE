#pragma once

#include <fuse/editor/editor_host.hpp>

#include <QThread>

#include <mutex>

namespace fuse::editor::qt {

/// Runs EditorHost::gameTick() on a dedicated game thread via QTimer (~60 Hz). Each tick holds
/// `sceneMutex` (when given) so UI-thread panels never observe a half-applied frame.
class GameLoopThread final : public QThread {
    Q_OBJECT

public:
    explicit GameLoopThread(EditorHost* host, std::mutex* sceneMutex = nullptr, QObject* parent = nullptr);

protected:
    void run() override;

private:
    EditorHost* m_host = nullptr;
    std::mutex* m_sceneMutex = nullptr;
};

} // namespace fuse::editor::qt
