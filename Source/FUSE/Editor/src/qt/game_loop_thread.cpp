#include "game_loop_thread.hpp"

#include <QTimer>

namespace fuse::editor::qt {

GameLoopThread::GameLoopThread(EditorHost* host, std::mutex* sceneMutex, QObject* parent)
    : QThread(parent), m_host(host), m_sceneMutex(sceneMutex) {}

void GameLoopThread::run() {
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(16);
    connect(&timer, &QTimer::timeout, &timer, [this]() {
        if (m_host == nullptr) {
            return;
        }
        if (m_sceneMutex != nullptr) {
            std::lock_guard<std::mutex> lock(*m_sceneMutex);
            m_host->gameTick();
        } else {
            m_host->gameTick();
        }
    });
    timer.start();
    exec();
}

} // namespace fuse::editor::qt
