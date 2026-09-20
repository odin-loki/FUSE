#pragma once

#include <fuse/alloc/allocator.hpp>

namespace fuse::alloc {

/// Named domain byte budget (e.g. "core", "frame", "scene"). tryCharge fails closed when over cap.
class DomainBudget {
public:
    DomainBudget(const char* name, usize budgetBytes);

    bool tryCharge(usize n);
    void release(usize n);

    const char* name() const { return m_name; }
    usize budget() const { return m_budget; }
    usize used() const { return m_used; }
    usize peak() const { return m_peak; }
    u64 failed() const { return m_failed; }
    AllocStats stats() const;

private:
    const char* m_name = "";
    usize m_budget = 0;
    usize m_used = 0;
    usize m_peak = 0;
    u64 m_failed = 0;
};

} // namespace fuse::alloc
