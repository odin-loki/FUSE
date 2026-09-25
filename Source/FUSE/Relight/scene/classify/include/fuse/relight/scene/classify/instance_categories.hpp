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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_types.h@0867d3c (InstanceCategories, CategoryFlags).
//
// The 25 instance categories a draw call can carry, in Remix's order (the bit positions are the
// enumerator values, as Remix's Flags<InstanceCategories>).
#pragma once

#include <cstdint>
#include <string>

namespace fuse::relight::scene {

enum class InstanceCategories : std::uint32_t {
    WorldUI,
    WorldMatte,
    Sky,
    Ignore,
    IgnoreLights,
    IgnoreAntiCulling,
    IgnoreMotionBlur,
    IgnoreOpacityMicromap,
    IgnoreAlphaChannel,
    Hidden,
    Particle,
    Beam,
    DecalStatic,
    DecalDynamic,
    DecalSingleOffset,
    DecalNoOffset,
    AlphaBlendToCutout,
    Terrain,
    AnimatedWater,
    ThirdPersonPlayerModel,
    ThirdPersonPlayerBody,
    IgnoreBakedLighting,
    ParticleEmitter,
    SmoothNormals,
    HairCards,

    Count,
};

inline constexpr std::uint32_t kInstanceCategoryCount = static_cast<std::uint32_t>(InstanceCategories::Count);

/// Name of a category as Remix spells the enumerator ("WorldUI", ...).
const char* instanceCategoryName(InstanceCategories category);

/// Remix CategoryFlags (Flags<InstanceCategories>).
class CategoryFlags {
public:
    constexpr CategoryFlags() = default;
    constexpr explicit CategoryFlags(std::uint32_t raw) : m_raw(raw) {}

    constexpr void set(InstanceCategories c) { m_raw |= bit(c); }
    constexpr void clr(InstanceCategories c) { m_raw &= ~bit(c); }
    constexpr bool test(InstanceCategories c) const { return (m_raw & bit(c)) != 0; }
    constexpr bool any() const { return m_raw != 0; }
    constexpr std::uint32_t raw() const { return m_raw; }
    friend constexpr bool operator==(CategoryFlags a, CategoryFlags b) { return a.m_raw == b.m_raw; }
    friend constexpr bool operator!=(CategoryFlags a, CategoryFlags b) { return a.m_raw != b.m_raw; }

    /// Set names joined with '|' ("" when empty).
    std::string toString() const;

private:
    static constexpr std::uint32_t bit(InstanceCategories c) { return 1u << static_cast<std::uint32_t>(c); }
    std::uint32_t m_raw = 0;
};

} // namespace fuse::relight::scene
