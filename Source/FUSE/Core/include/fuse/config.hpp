#pragma once

// Compile-time job system configuration.
// Override at configure time: -DFUSE_JOBS_SINGLE_THREAD=ON
// Or at compile time: -DFUSE_JOBS_SINGLE_THREAD=1

#ifndef FUSE_JOBS_SINGLE_THREAD
#define FUSE_JOBS_SINGLE_THREAD 0
#endif
