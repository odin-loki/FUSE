// FUSE Relight RL-5.4: the C++ dialect of the hash-grid radiance cache (kernels/radiance_cache_core.h, the path-tracer
// side radiance_cache_path.h), compiled after the path tracer's C++ core (pt_reference_cpp.hpp): the CPU reference of
// the "relight.radiance_cache.*" passes and the renderReference hook. The table is accessed through std::atomic_ref
// (relaxed; integer adds and compare-and-swap are order-independent, so kernel::Backend::CpuReference and CpuParallel
// give the same per-cell results). Everything here is allocation-free.
#pragma once

#include "pt_reference_cpp.hpp"

#include <fuse/types.hpp>

#include <atomic>
#include <bit>

namespace fuse::relight::ptk {

/// What the cache core reads and writes on the CPU (not owned).
struct RcCpuContext {
    u32* table = nullptr;     ///< kRcHeaderWords + capacity x kRcSlotWords
    float4* records = nullptr; ///< training records (kRcRecordWords per record)
    float4* paths = nullptr;  ///< per training tile: kRcMaxVertices x 3 vertex words
    uint width = 0;           ///< image width (training tile of a pixel)
    uint stride = 1;          ///< training tile edge
};

#define RC_FN inline
#define RC_CONST inline constexpr
#define RC_OUT(T) T&
#define RC_INOUT(T) T&
#define RC_CTX_PARAM const RcCpuContext &rc,
#define RC_CTX_ARG rc,
#define RC_ASUINT(x) std::bit_cast<uint>(float(x))
#define RC_ASFLOAT(x) std::bit_cast<float>(uint(x))
#define RC_PRECISE
#define RC_PARAM_WORDS(name) const float4* name

#include "radiance_cache_types.h"

inline uint rcTableLoad(const RcCpuContext& rc, uint w) {
    return std::atomic_ref<u32>(rc.table[w]).load(std::memory_order_relaxed);
}
inline void rcTableStore(const RcCpuContext& rc, uint w, uint x) {
    std::atomic_ref<u32>(rc.table[w]).store(x, std::memory_order_relaxed);
}
inline uint rcTableCas(const RcCpuContext& rc, uint w, uint c, uint x) {
    u32 expected = c;
    std::atomic_ref<u32>(rc.table[w]).compare_exchange_strong(expected, x, std::memory_order_relaxed);
    return expected;
}
inline uint rcTableAdd(const RcCpuContext& rc, uint w, uint x) {
    return std::atomic_ref<u32>(rc.table[w]).fetch_add(x, std::memory_order_relaxed);
}
inline uint rcCpuTile(const RcCpuContext& rc, uint px, uint py) {
    const uint s = rc.stride > 0u ? rc.stride : 1u;
    return (py / s) * ((rc.width + s - 1u) / s) + px / s;
}
inline float4 rcPathLoad(const RcCpuContext& rc, uint px, uint py, uint k) {
    return rc.paths[rcCpuTile(rc, px, py) * kRcMaxVertices * 3u + k];
}
inline void rcPathStore(const RcCpuContext& rc, uint px, uint py, uint k, float4 v) {
    rc.paths[rcCpuTile(rc, px, py) * kRcMaxVertices * 3u + k] = v;
}
inline void rcRecordStore(const RcCpuContext& rc, uint record, uint k, float4 v) {
    rc.records[record * kRcRecordWords + k] = v;
}

#include "radiance_cache_core.h"
#include "radiance_cache_path.h"

#undef RC_FN
#undef RC_CONST
#undef RC_OUT
#undef RC_INOUT
#undef RC_CTX_PARAM
#undef RC_CTX_ARG
#undef RC_ASUINT
#undef RC_ASFLOAT
#undef RC_PRECISE
#undef RC_PARAM_WORDS

} // namespace fuse::relight::ptk
