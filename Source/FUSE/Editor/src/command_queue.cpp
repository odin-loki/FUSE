#include <fuse/editor/command_queue.hpp>

#include <cstdio>

namespace fuse::editor {

std::string formatPropertyFloat(f32 value) {
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return written > 0 ? std::string(buffer, static_cast<usize>(written)) : std::string("0");
}

namespace {

bool canCoalesceSetProperty_(const EditorCommand& previous, const EditorCommand& incoming) {
    if (previous.kind != incoming.kind || previous.kind != CommandKind::SetProperty) {
        return false;
    }

    if (previous.propertyName.empty() || incoming.propertyName.empty()) {
        return false;
    }

    if (!previous.target.isValid() || !incoming.target.isValid()) {
        return false;
    }

    if (!incoming.propertyValueBefore.empty()) {
        return false;
    }

    if (previous.propertyValue.empty() || incoming.propertyValue.empty()) {
        return false;
    }

    return previous.target == incoming.target && previous.propertyName == incoming.propertyName;
}

} // namespace

void CommandQueue::post(EditorCommand command) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pendingDeque.empty() && canCoalesceSetProperty_(m_pendingDeque.back(), command)) {
        m_pendingDeque.back().propertyValue = std::move(command.propertyValue);
        ++m_coalescedPosts;
        return;
    }

    m_pendingDeque.push_back(std::move(command));
    ++m_pending;
}

void CommandQueue::drain() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_pendingDeque.empty()) {
        // Idle frame: no batch to move. (A default-constructed libstdc++ deque allocates its map, so
        // skipping it keeps an idle editor tick allocation-free.)
        lock.unlock();
        m_lastDrained.clear();
        return;
    }
    std::deque<EditorCommand> batch;
    batch.swap(m_pendingDeque);
    m_pending = 0;
    lock.unlock();

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
