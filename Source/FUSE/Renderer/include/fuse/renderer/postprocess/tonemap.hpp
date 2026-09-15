#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Tone-map operator identifiers (B5.10 — P5 §5.10).
enum class ToneMapper : u8 {
    ACES = 0,
    Filmic = 1,
    Reinhard = 2,
    Neutral = 3,
};

/// CPU reference tone mapping — mirrors renderer/postprocess/tonemapping.cuh.
fuse::math::Vec3 aces_tonemap(const fuse::math::Vec3& x);
fuse::math::Vec3 filmic_tonemap(const fuse::math::Vec3& x);
fuse::math::Vec3 reinhard_tonemap(const fuse::math::Vec3& x);

fuse::math::Vec3 apply_tone_map(const fuse::math::Vec3& hdr, ToneMapper mapper);
const char* tone_mapper_name(ToneMapper mapper);

/// Host-side tone-map pass stub (CUDA kernel deferred).
class ToneMap {
public:
    void setMapper(ToneMapper mapper) { m_mapper = mapper; }
    ToneMapper mapper() const { return m_mapper; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    fuse::math::Vec3 apply(const fuse::math::Vec3& hdr) const;

private:
    ToneMapper m_mapper = ToneMapper::ACES;
    bool m_ready = false;
};

} // namespace fuse::renderer
