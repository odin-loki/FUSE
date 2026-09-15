#include <fuse/jobs/work_steal.hpp>

namespace fuse::jobs {

u32 pickStealVictim(u32 thief, u32 workerCount, u32 round) {
    if (workerCount <= 1) {
        return thief;
    }

    const u32 span = workerCount - 1;
    const u32 offset = round % span;
    return (thief + 1 + offset) % workerCount;
}

u32 stealHalfQueueBatchSize(std::size_t victimQueueSize) {
    if (victimQueueSize == 0) {
        return 0;
    }
    return static_cast<u32>((victimQueueSize + 1) / 2);
}

bool canStealFromVictim(std::size_t victimQueueSize) {
    return victimQueueSize >= WorkStealParams::kMinVictimQueueSize;
}

} // namespace fuse::jobs
