#pragma once

#include <fuse/types.hpp>

namespace fuse::jobs {

class JobCounter;

namespace detail {

struct WorkerState;

bool isWorkerThread();
bool workerWaitOnCounter(JobCounter* counter);

} // namespace detail

} // namespace fuse::jobs
