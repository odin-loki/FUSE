#include <fuse/alloc/domain_budget.hpp>

#include <fuse/alloc/alloc_stats.hpp>

namespace fuse::alloc {

DomainBudget::DomainBudget(const char* name, usize budgetBytes)
    : m_name(name != nullptr ? name : "")
    , m_budget(budgetBytes) {}

AllocStats DomainBudget::stats() const {
    AllocStats s{};
    s.usedBytes = m_used;
    s.totalBytes = m_budget;
    s.peakUsedBytes = m_peak;
    s.failedAllocs = m_failed;
    return s;
}

bool DomainBudget::tryCharge(usize n) {
    if (n > m_budget || m_used > m_budget - n) {
        ++m_failed;
        notifyStats(m_name, stats());
        return false;
    }

    m_used += n;
    if (m_used > m_peak) {
        m_peak = m_used;
    }
    notifyStats(m_name, stats());
    return true;
}

void DomainBudget::release(usize n) {
    if (n >= m_used) {
        m_used = 0;
    } else {
        m_used -= n;
    }
    notifyStats(m_name, stats());
}

} // namespace fuse::alloc
