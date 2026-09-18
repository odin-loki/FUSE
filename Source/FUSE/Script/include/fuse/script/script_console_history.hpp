#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::script {

/// Fixed-capacity ring buffer for REPL command history with duplicate coalescing.
class ScriptConsoleHistoryBuffer {
public:
    static constexpr u32 kDefaultCapacity = 64;

    void setCapacity(u32 capacity);
    [[nodiscard]] u32 capacity() const { return m_capacity; }
    [[nodiscard]] u32 count() const { return m_size; }
    [[nodiscard]] bool is_empty() const { return m_size == 0; }
    [[nodiscard]] bool is_valid_index(u32 index) const { return index < m_size; }
    [[nodiscard]] const std::string& at(u32 index) const;

    void push(const char* line);
    void clear();

    [[nodiscard]] const std::string& newest() const;
    [[nodiscard]] const std::string& oldest() const;

    /// Navigate history (`previous=true` recalls older entries).
    [[nodiscard]] const std::string& recall(bool previous);
    void resetNavigation();
    [[nodiscard]] s32 navigationCursor() const { return m_navigationCursor; }
    /// True when `recall(true)` would move to an older stored entry.
    [[nodiscard]] bool can_recall_previous() const;
    /// True when `recall(false)` would move toward the live input position.
    [[nodiscard]] bool can_recall_next() const;
    /// True when navigation cursor sits at the live-input position (past newest entry).
    [[nodiscard]] bool is_navigation_at_end() const;

private:
    [[nodiscard]] u32 ringIndex_(u32 offset) const;

    std::vector<std::string> m_entries;
    u32 m_capacity = kDefaultCapacity;
    u32 m_start = 0;
    u32 m_size = 0;
    s32 m_navigationCursor = -1;
};

} // namespace fuse::script
