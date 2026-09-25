// FUSE Relight RL-5.7: demodulate / composite (see composite.hpp).
#include <fuse/relight/render/post/composite.hpp>

namespace fuse::relight::render::post {

bool compositeRadiance(const CompositeInputs& in, math::Vec3* out) {
    if (in.emissive == nullptr || in.diffuse == nullptr || in.specular == nullptr || in.albedoD == nullptr ||
        in.albedoS == nullptr || (out == nullptr && in.count != 0u)) {
        return false;
    }
    const float* d = in.denoisedDiffuse != nullptr ? in.denoisedDiffuse : in.diffuse;
    const float* s = in.denoisedSpecular != nullptr ? in.denoisedSpecular : in.specular;
    for (std::size_t i = 0; i < in.count; ++i) {
        const std::size_t k = i * 4u;
        float c[3];
        for (u32 j = 0; j < 3u; ++j) {
            c[j] = in.emissive[k + j] + d[k + j] * in.albedoD[k + j] + s[k + j] * in.albedoS[k + j];
        }
        out[i] = math::Vec3(c[0], c[1], c[2]);
    }
    return true;
}

} // namespace fuse::relight::render::post
