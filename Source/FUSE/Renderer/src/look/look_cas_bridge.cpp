#include <fuse/renderer/look/look_cas_bridge.hpp>

#if __has_include(<fuse/renderer/upscale/upscale_passes.hpp>)
#include <fuse/renderer/upscale/upscale_passes.hpp>
#define FUSE_LOOK_HAS_CAS 1
#else
#define FUSE_LOOK_HAS_CAS 0
#endif

namespace fuse::renderer::look {

bool LookCasSharpener::available() {
    return FUSE_LOOK_HAS_CAS != 0;
}

bool LookCasSharpener::init(u32 width, u32 height, kernel::Backend backend) {
    if (width == 0u || height == 0u) {
        return false;
    }
    m_width = width;
    m_height = height;
    m_backend = backend;
    m_src.assign(static_cast<size_t>(width) * height, math::Vec4{});
    m_dst.assign(static_cast<size_t>(width) * height, math::Vec4{});
    return true;
}

bool LookCasSharpener::hook(const math::Vec3* src, math::Vec3* dst, u32 width, u32 height, f32 sharpness, void* user) {
#if FUSE_LOOK_HAS_CAS
    auto* self = static_cast<LookCasSharpener*>(user);
    if (self == nullptr || src == nullptr || dst == nullptr || width != self->m_width || height != self->m_height) {
        return false;
    }
    const size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; ++i) {
        self->m_src[i] = math::Vec4{src[i], 1.f};
    }
    upscale::PassOptions options{};
    options.backend = self->m_backend;
    const upscale::ConstRgbaImage in{self->m_src.data(), width, height};
    const upscale::RgbaImage out{self->m_dst.data(), width, height};
    if (!upscale::run_cas(in, out, sharpness, options)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        dst[i] = math::Vec3{self->m_dst[i].x, self->m_dst[i].y, self->m_dst[i].z};
    }
    ++self->m_calls;
    return true;
#else
    (void)src;
    (void)dst;
    (void)width;
    (void)height;
    (void)sharpness;
    (void)user;
    return false;
#endif
}

} // namespace fuse::renderer::look
