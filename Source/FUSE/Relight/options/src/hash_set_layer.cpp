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

#include <fuse/relight/options/hash_set_layer.hpp>
#include <fuse/relight/options/option_value.hpp>

#include <algorithm>

namespace fuse::relight::options {

namespace {

std::vector<Hash64> sorted(const HashSet& set) {
    std::vector<Hash64> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    return out;
}

void appendList(std::string& out, const std::vector<Hash64>& hashes, const char* prefix) {
    for (Hash64 hash : hashes) {
        if (!out.empty()) {
            out += ", ";
        }
        out += prefix;
        out += formatHash(hash);
    }
}

} // namespace

std::string formatHash(Hash64 hash) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out = "0x0000000000000000";
    for (int i = 0; i < 16; ++i) {
        out[static_cast<std::size_t>(17 - i)] = kDigits[(hash >> (4 * i)) & 0xFu];
    }
    return out;
}

void HashSetLayer::assignPositives(const HashSet& positives) {
    m_positives = positives;
    for (Hash64 hash : positives) {
        m_negatives.erase(hash);
    }
}

std::size_t HashSetLayer::parseFromStrings(const std::vector<std::string>& rawInput, std::vector<std::string>* errors) {
    std::size_t failures = 0;
    for (const std::string& entry : rawInput) {
        const std::size_t start = entry.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) {
            continue; // empty or all whitespace
        }
        const std::size_t end = entry.find_last_not_of(" \t\n\r");
        std::string_view trimmed(entry.data() + start, end - start + 1);
        const bool negative = trimmed.front() == '-';
        if (negative) {
            trimmed.remove_prefix(1);
        }
        Hash64 hash = 0;
        if (!parseHashValue(trimmed, hash)) {
            ++failures;
            if (errors) {
                errors->push_back("invalid hash '" + std::string(entry) + "'");
            }
            continue;
        }
        if (negative) {
            m_negatives.insert(hash);
        } else {
            m_positives.insert(hash);
        }
    }
    return failures;
}

std::string HashSetLayer::toString() const {
    std::string out;
    appendList(out, sorted(m_positives), "");
    appendList(out, sorted(m_negatives), "-");
    return out;
}

HashSetLayer HashSetLayer::computeAddedOpinions(const HashSetLayer& saved) const {
    HashSetLayer added;
    for (Hash64 hash : m_positives) {
        if (saved.m_positives.count(hash) == 0) {
            added.m_positives.insert(hash);
        }
    }
    for (Hash64 hash : m_negatives) {
        if (saved.m_negatives.count(hash) == 0) {
            added.m_negatives.insert(hash);
        }
    }
    return added;
}

std::string HashSetLayer::diffToString(const HashSetLayer& saved) const {
    auto missingFrom = [](const HashSet& from, const HashSet& in) {
        std::vector<Hash64> out;
        for (Hash64 hash : from) {
            if (in.count(hash) == 0) {
                out.push_back(hash);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    std::string out;
    appendList(out, missingFrom(m_positives, saved.m_positives), "+");
    appendList(out, missingFrom(saved.m_positives, m_positives), "~");
    appendList(out, missingFrom(m_negatives, saved.m_negatives), "+-");
    appendList(out, missingFrom(saved.m_negatives, m_negatives), "~-");
    return out;
}

void HashSetLayer::mergeFrom(const HashSetLayer& weaker) {
    for (Hash64 hash : weaker.m_positives) {
        if (!hasPositive(hash) && !hasNegative(hash)) {
            m_positives.insert(hash);
        }
    }
    for (Hash64 hash : weaker.m_negatives) {
        if (!hasPositive(hash) && !hasNegative(hash)) {
            m_negatives.insert(hash);
        }
    }
}

} // namespace fuse::relight::options
