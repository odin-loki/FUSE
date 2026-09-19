// FrameAllocator TLS + explicit template instantiation for FUSE_T3D_LEGACY_ENGINE_PROBE.
// commonSwizzles.cpp references thread_local smFrameAllocator; without explicit
// instantiation GCC emits an undefined ManagedAlignedBufferAllocator<U32> dtor.

#include "core/frameAllocator.h"

template class ManagedAlignedBufferAllocator<U32>;

thread_local ManagedAlignedBufferAllocator<U32> FrameAllocator::smFrameAllocator;
