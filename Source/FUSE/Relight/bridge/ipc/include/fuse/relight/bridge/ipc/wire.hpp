// FUSE Relight RL-2.1: bridge wire format used by the generated command encoders and decoders.
// Copyright (c) 2026 FUSE contributors (MIT). New code (upstream serialized ad hoc per call site).
//
// Little-endian, unaligned, no padding: scalars are raw bytes; std::array<T, N> is N raw elements;
// std::vector<T> and std::string are a u32 element count followed by the elements. The reader is
// bounds-checked everywhere (a hostile or corrupt payload makes get() return false, never read out of
// range), which the malformed-input fuzz in rl_bridge_ipc_unit relies on.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace fuse::relight::bridge::ipc {

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the bridge wire format assumes a little-endian host");
#endif

template <class T>
inline constexpr bool kWireScalar = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

template <class T, std::enable_if_t<kWireScalar<T>, int> = 0>
constexpr size_t wireSize(const T&) noexcept {
    return sizeof(T);
}
template <class T, size_t N>
constexpr size_t wireSize(const std::array<T, N>&) noexcept {
    static_assert(kWireScalar<T>);
    return sizeof(T) * N;
}
template <class T>
size_t wireSize(const std::vector<T>& v) noexcept {
    static_assert(kWireScalar<T>);
    return 4 + sizeof(T) * v.size();
}
inline size_t wireSize(const std::string& s) noexcept { return 4 + s.size(); }

class WireWriter {
public:
    WireWriter(void* data, size_t size) noexcept
        : cur_(static_cast<uint8_t*>(data)), end_(static_cast<uint8_t*>(data) + size) {}

    template <class T, std::enable_if_t<kWireScalar<T>, int> = 0>
    void put(const T& v) noexcept {
        raw(&v, sizeof(T));
    }
    template <class T, size_t N>
    void put(const std::array<T, N>& a) noexcept {
        raw(a.data(), sizeof(T) * N);
    }
    template <class T>
    void put(const std::vector<T>& v) noexcept {
        put(static_cast<uint32_t>(v.size()));
        raw(v.data(), sizeof(T) * v.size());
    }
    void put(const std::string& s) noexcept {
        put(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }

    size_t remaining() const noexcept { return static_cast<size_t>(end_ - cur_); }
    bool overflowed() const noexcept { return overflow_; }

private:
    void raw(const void* p, size_t n) noexcept {
        if (n > remaining()) {
            overflow_ = true;
            return;
        }
        if (n != 0) {
            std::memcpy(cur_, p, n);
        }
        cur_ += n;
    }
    uint8_t* cur_;
    uint8_t* end_;
    bool overflow_ = false;
};

class WireReader {
public:
    WireReader(const void* data, size_t size) noexcept
        : cur_(static_cast<const uint8_t*>(data)), end_(static_cast<const uint8_t*>(data) + size) {}

    template <class T, std::enable_if_t<kWireScalar<T>, int> = 0>
    bool get(T& v) noexcept {
        return raw(&v, sizeof(T));
    }
    template <class T, size_t N>
    bool get(std::array<T, N>& a) noexcept {
        return raw(a.data(), sizeof(T) * N);
    }
    template <class T>
    bool get(std::vector<T>& v) {
        uint32_t count = 0;
        if (!get(count) || count > remaining() / sizeof(T)) {
            return false;
        }
        v.resize(count);
        return raw(v.data(), sizeof(T) * count);
    }
    bool get(std::string& s) {
        uint32_t count = 0;
        if (!get(count) || count > remaining()) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(cur_), count);
        cur_ += count;
        return true;
    }

    size_t remaining() const noexcept { return static_cast<size_t>(end_ - cur_); }
    bool atEnd() const noexcept { return cur_ == end_; }

private:
    bool raw(void* p, size_t n) noexcept {
        if (n > remaining()) {
            return false;
        }
        if (n != 0) {
            std::memcpy(p, cur_, n);
        }
        cur_ += n;
        return true;
    }
    const uint8_t* cur_;
    const uint8_t* end_;
};

// Field equality for generated operator==: floating-point values compare by bit pattern, so a
// round trip of NaN payloads (the fuzz draws raw bits) is still "equal".
template <class T>
bool wireEqual(const T& a, const T& b) {
    if constexpr (std::is_floating_point_v<T>) {
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    } else {
        return a == b;
    }
}
template <class T, size_t N>
bool wireEqual(const std::array<T, N>& a, const std::array<T, N>& b) {
    return N == 0 || std::memcmp(a.data(), b.data(), sizeof(T) * N) == 0;
}
template <class T>
bool wireEqual(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), sizeof(T) * a.size()) == 0);
}
inline bool wireEqual(const std::string& a, const std::string& b) { return a == b; }

}  // namespace fuse::relight::bridge::ipc
