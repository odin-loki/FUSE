#include <fuse/cinematics/cue_queue.hpp>

namespace fuse::cinematics {

void CueQueue::enqueue(const CueEntry& entry) {
    pending_.push_back(entry);
    ++total_enqueued_;
    if (dispatch_hook_) {
        dispatch_hook_(entry);
    }
}

void CueQueue::clear() {
    pending_.clear();
}

std::vector<CueEntry> CueQueue::drain() {
    std::vector<CueEntry> drained = std::move(pending_);
    pending_.clear();
    return drained;
}

} // namespace fuse::cinematics
