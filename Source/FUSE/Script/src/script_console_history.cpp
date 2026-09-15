#include <fuse/script/script_console_history.hpp>

#include <algorithm>
#include <cctype>

namespace fuse::script {

namespace {

std::string trim(const std::string& text) {
    const auto begin = std::find_if_not(text.begin(), text.end(),
                                        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto end = std::find_if_not(text.rbegin(), text.rend(),
                                      [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

} // namespace

void ScriptConsoleHistoryBuffer::setCapacity(u32 capacity) {
    m_capacity = capacity == 0 ? 1u : capacity;

    while (m_size > m_capacity) {
        m_start = (m_start + 1u) % m_capacity;
        --m_size;
    }

    if (m_size < m_capacity) {
        std::vector<std::string> compact;
        compact.reserve(m_size);
        for (u32 i = 0; i < m_size; ++i) {
            compact.push_back(at(i));
        }
        m_entries = std::move(compact);
        m_start = 0;
    } else if (m_entries.size() > m_capacity) {
        m_entries.resize(m_capacity);
    }

    if (m_navigationCursor > static_cast<s32>(m_size)) {
        m_navigationCursor = static_cast<s32>(m_size);
    }
}

const std::string& ScriptConsoleHistoryBuffer::at(u32 index) const {
    static const std::string kEmpty;
    if (index >= m_size || m_entries.empty()) {
        return kEmpty;
    }

    if (m_size < m_capacity) {
        return m_entries[index];
    }

    return m_entries[ringIndex_(index)];
}

void ScriptConsoleHistoryBuffer::clear() {
    m_entries.clear();
    m_start = 0;
    m_size = 0;
    m_navigationCursor = -1;
}

void ScriptConsoleHistoryBuffer::push(const char* line) {
    if (line == nullptr) {
        return;
    }

    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return;
    }

    if (m_size > 0 && at(m_size - 1) == trimmed) {
        return;
    }

    if (m_size < m_capacity) {
        m_entries.push_back(trimmed);
        ++m_size;
    } else {
        m_entries[m_start] = trimmed;
        m_start = (m_start + 1u) % m_capacity;
    }

    m_navigationCursor = static_cast<s32>(m_size);
}

const std::string& ScriptConsoleHistoryBuffer::recall(bool previous) {
    static const std::string kEmpty;

    if (m_size == 0) {
        return kEmpty;
    }

    if (previous) {
        if (m_navigationCursor <= 0) {
            m_navigationCursor = 0;
        } else {
            --m_navigationCursor;
        }
        return at(static_cast<u32>(m_navigationCursor));
    }

    if (m_navigationCursor >= static_cast<s32>(m_size) - 1) {
        m_navigationCursor = static_cast<s32>(m_size);
        return kEmpty;
    }

    ++m_navigationCursor;
    if (m_navigationCursor >= static_cast<s32>(m_size)) {
        m_navigationCursor = static_cast<s32>(m_size);
        return kEmpty;
    }

    return at(static_cast<u32>(m_navigationCursor));
}

void ScriptConsoleHistoryBuffer::resetNavigation() {
    m_navigationCursor = static_cast<s32>(m_size);
}

u32 ScriptConsoleHistoryBuffer::ringIndex_(u32 offset) const {
    return (m_start + offset) % m_capacity;
}

} // namespace fuse::script
