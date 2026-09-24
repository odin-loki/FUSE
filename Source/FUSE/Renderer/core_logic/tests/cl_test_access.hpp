// Test-only friend used to corrupt core state so every check_invariants() failure path is
// exercised (WP-0.8 coverage). Defined once, here, for all test translation units.
#pragma once

#include "fuse/core_logic/residency_lru.hpp"
#include "fuse/core_logic/vsm_page_table.hpp"

namespace fuse {
namespace core_logic {

struct ClTestAccess {
    // VsmPageTable
    template <class T> static uint32_t* pte(T& t) { return t.pte_; }
    template <class T> static uint32_t* owner(T& t) { return t.owner_; }
    template <class T> static uint32_t* lastUsed(T& t) { return t.lastUsed_; }
    template <class T> static uint32_t* prev(T& t) { return t.prev_; }
    template <class T> static uint32_t* next(T& t) { return t.next_; }
    template <class T> static uint32_t* freeStack(T& t) { return t.freeStack_; }
    template <class T> static uint32_t& freeTop(T& t) { return t.freeTop_; }
    template <class T> static uint32_t& head(T& t) { return t.head_; }
    template <class T> static uint32_t& tail(T& t) { return t.tail_; }
    template <class T> static uint32_t& lruCount(T& t) { return t.lruCount_; }
    // ClusterResidency
    template <class T> static ResidencyState* state(T& t) { return t.state_; }
    template <class T> static uint32_t* activeChildren(T& t) { return t.activeChildren_; }
    template <class T> static uint64_t& residentBytes(T& t) { return t.residentBytes_; }
    template <class T> static uint64_t& loadingBytes(T& t) { return t.loadingBytes_; }
    template <class T> static uint64_t& coarseBytes(T& t) { return t.coarseBytes_; }
    template <class T> static uint64_t& budget(T& t) { return t.budget_; }
};

} // namespace core_logic
} // namespace fuse
