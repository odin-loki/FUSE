#include <fuse/editor/command_queue.hpp>

namespace fuse::editor {

void CommandQueue::post(EditorCommand command) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingDeque.push_back(std::move(command));
    ++m_pending;
}

void CommandQueue::drain() {
    std::deque<EditorCommand> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        batch.swap(m_pendingDeque);
        m_pending = 0;
    }

    m_lastDrained.clear();
    m_lastDrained.reserve(batch.size());
    while (!batch.empty()) {
        m_lastDrained.push_back(std::move(batch.front()));
        batch.pop_front();
    }

    m_applied += static_cast<u32>(m_lastDrained.size());
}

u32 CommandQueue::pendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending;
}

} // namespace fuse::editor
