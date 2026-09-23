#pragma once

#include <fuse/types.hpp>

#include <atomic>

namespace fuse::jobs {

class JobCounter;

namespace detail {

struct WorkerState;

bool isWorkerThread();
bool workerWaitOnCounter(JobCounter* counter);

/// On a worker job fiber, park until `*word` reads zero (acquire) and return true; the worker keeps
/// running other jobs meanwhile. Returns false without waiting when the caller is not on a fiber.
bool workerWaitOnZero(const std::atomic<u32>* word);

} // namespace detail

} // namespace fuse::jobs
