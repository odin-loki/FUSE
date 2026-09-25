/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/util/util_hash_set_layer.h@0867d3c
//
// One layer's opinion about a hash-set option: positive entries (this layer adds the hash) and
// negative entries (this layer removes it, overriding weaker layers). Written as
// `0x<16 hex>, …, -0x<16 hex>, …` (positives first, each group sorted ascending).
#pragma once

#include <fuse/relight/options/option_types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::options {

class HashSetLayer {
public:
    bool operator==(const HashSetLayer& other) const {
        return m_positives == other.m_positives && m_negatives == other.m_negatives;
    }
    bool operator!=(const HashSetLayer& other) const { return !(*this == other); }

    bool empty() const { return m_positives.empty() && m_negatives.empty(); }
    void clearAll() {
        m_positives.clear();
        m_negatives.clear();
    }

    /// This layer includes `hash` (drops a negative opinion on it).
    void add(Hash64 hash) {
        m_negatives.erase(hash);
        m_positives.insert(hash);
    }
    /// This layer excludes `hash`, overriding weaker layers (drops a positive opinion on it).
    void remove(Hash64 hash) {
        m_positives.erase(hash);
        m_negatives.insert(hash);
    }
    /// Drop any opinion about `hash`.
    void clear(Hash64 hash) {
        m_positives.erase(hash);
        m_negatives.erase(hash);
    }

    bool hasPositive(Hash64 hash) const { return m_positives.count(hash) > 0; }
    bool hasNegative(Hash64 hash) const { return m_negatives.count(hash) > 0; }

    /// 1 when `hash` is a positive entry and not negated, else 0.
    std::size_t count(Hash64 hash) const {
        if (m_negatives.count(hash) > 0) {
            return 0;
        }
        return m_positives.count(hash);
    }

    std::size_t size() const { return m_positives.size(); }
    std::size_t negativeSize() const { return m_negatives.size(); }

    const HashSet& positives() const { return m_positives; }
    const HashSet& negatives() const { return m_negatives; }

    /// Replace the positive entries (Remix `setDeferred(fast_unordered_set)` on a hash-set option).
    /// Negative entries survive, except for hashes that are now positive (one opinion per hash).
    void assignPositives(const HashSet& positives);

    /// Parse entries as split from a config value (`"0x1234"`, `" -0xABCD"` …). Each entry is
    /// trimmed; empty entries are skipped; a leading `-` makes a negative entry; the rest is parsed
    /// as base-16 with an optional `0x` prefix, like std::stoull(…, 16). Returns the number of
    /// entries that failed to parse (skipped; upstream throws out of the loader instead). When
    /// `errors` is given, one message per failure is appended.
    std::size_t parseFromStrings(const std::vector<std::string>& rawInput, std::vector<std::string>* errors = nullptr);

    /// Canonical text form: sorted positives, then sorted `-` negatives, separated by ", ".
    std::string toString() const;

    /// Opinions present here but not in `saved` (new positives and new negatives). Used to export a
    /// delta of unsaved changes.
    HashSetLayer computeAddedOpinions(const HashSetLayer& saved) const;

    /// Human-readable delta against `saved`: `+0x…` added, `~0x…` removed, `+-0x…` / `~-0x…` for
    /// negative entries.
    std::string diffToString(const HashSetLayer& saved) const;

    /// Merge a weaker layer into this accumulated result. The weaker layer's opinion on a hash only
    /// applies when this set has none. Resolution walks layers strongest first.
    void mergeFrom(const HashSetLayer& weaker);

private:
    HashSet m_positives;
    HashSet m_negatives;
};

/// Format a hash the way Remix writes it: `0x` + 16 upper-case hex digits.
std::string formatHash(Hash64 hash);

} // namespace fuse::relight::options
