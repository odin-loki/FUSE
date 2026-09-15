#include "game_loop_thread.hpp"

#include <QTimer>

namespace fuse::editor::qt {

GameLoopThread::GameLoopThread(EditorHost* host, QObject* parent)
    : QThread(parent), m_host(host) {}

void GameLoopThread::run() {
    QTimer timer;
    timer.setInterval(16);
    connect(&timer, &QTimer::timeout, this, [this]() {
        if (m_host != nullptr) {
            m_host->gameTick();
        }
    });
    timer.start();
    exec();
}

} // namespace fuse::editor::qt
