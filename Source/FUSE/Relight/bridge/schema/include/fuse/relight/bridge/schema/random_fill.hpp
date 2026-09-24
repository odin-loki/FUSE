// FUSE Relight RL-2.1: random field values for the generated command randomizers (fuzz tests).
// Copyright (c) 2026 FUSE contributors (MIT). New code.
//
// Rng: `uint64_t next()` (uniform 64-bit) and `uint32_t length()` (element count for counted fields;
// the test decides the size profile). Floating-point fields get raw random bits (NaNs, infinities
// and denormals included): the wire format must carry them bit-exactly.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace fuse::relight::bridge::schema {

template <class Rng, class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
void randomFill(Rng& rng, T& v) {
    const uint64_t bits = rng.next();
    std::memcpy(&v, &bits, sizeof(T));
}

template <class Rng, class T, size_t N>
void randomFill(Rng& rng, std::array<T, N>& a) {
    for (T& v : a) {
        randomFill(rng, v);
    }
}

template <class Rng, class T>
void randomFill(Rng& rng, std::vector<T>& v) {
    v.resize(rng.length());
    if constexpr (sizeof(T) == 1) {
        // Bulk fill for blobs (vertex data, textures): 8 bytes per draw.
        size_t i = 0;
        for (; i + 8 <= v.size(); i += 8) {
            const uint64_t bits = rng.next();
            std::memcpy(v.data() + i, &bits, 8);
        }
        for (; i < v.size(); ++i) {
            v[i] = static_cast<T>(rng.next());
        }
    } else {
        for (T& e : v) {
            randomFill(rng, e);
        }
    }
}

template <class Rng>
void randomFill(Rng& rng, std::string& s) {
    s.resize(rng.length());
    for (char& c : s) {
        c = static_cast<char>(rng.next());  // arbitrary bytes, embedded NULs included
    }
}

}  // namespace fuse::relight::bridge::schema
