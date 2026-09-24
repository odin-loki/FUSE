#include <fuse/renderer/upscale/reference_scene.hpp>
#include <fuse/renderer/upscale/reference_scene_kernel.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/math/mat.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace fuse::renderer::refscene {
namespace {

namespace rk = refscene_kernel;
using math::Vec2;
using math::Vec3;
using math::Vec4;

constexpr f32 kFovDeg = 55.f;
constexpr f32 kPi = 3.14159265358979f;

static_assert(kObjectSky == rk::kIdSky && kObjectGround == rk::kIdGround && kObjectThin == rk::kIdThin &&
              kObjectArm == rk::kIdArm);

compute::SdfObject object(compute::SdfPrimitiveType type, Vec3 position, Vec3 params, u32 material, f32 alpha = 0.f,
                          f32 rounding = 0.f) {
    compute::SdfObject o{};
    o.type = static_cast<u32>(type);
    o.position = position;
    o.params = params;
    o.material_id = material;
    o.alpha = alpha;
    o.rounding = rounding;
    return o;
}

using compute::SdfPrimitiveType;

const std::array<compute::SdfObject, 8>& showcaseObjects() {
    static const std::array<compute::SdfObject, 8> objects{
        object(SdfPrimitiveType::Box, {0.f, 1.5f, -3.5f}, {4.5f, 1.5f, 0.2f}, 1u),                 // tiled backdrop
        object(SdfPrimitiveType::Sphere, {-0.9f, 0.9f, -1.4f}, {0.9f, 0.f, 0.f}, 2u),              // sphere
        object(SdfPrimitiveType::Sphere, {-0.2f, 0.4f, -0.9f}, {0.4f, 0.f, 0.f}, 2u, 0.3f),         // smooth blob
        object(SdfPrimitiveType::Torus, {1.6f, 0.9f, -1.0f}, {0.6f, 0.2f, 0.f}, 3u),               // torus
        object(SdfPrimitiveType::Box, {-1.5f, 1.0f, 2.3f}, {0.18f, 1.0f, 0.18f}, 1u, 0.f, 0.02f),  // pillars
        object(SdfPrimitiveType::Box, {0.1f, 1.0f, 2.6f}, {0.18f, 1.0f, 0.18f}, 1u, 0.f, 0.02f),
        object(SdfPrimitiveType::Box, {1.6f, 1.0f, 2.3f}, {0.18f, 1.0f, 0.18f}, 1u, 0.f, 0.02f),
        object(SdfPrimitiveType::Capsule, {2.8f, 0.6f, 0.5f}, {0.25f, 0.35f, 0.f}, 0u),
    };
    return objects;
}

const std::array<compute::SdfObject, 2>& thinFastObjects() {
    static const std::array<compute::SdfObject, 2> objects{
        object(SdfPrimitiveType::Box, {0.f, 1.5f, -1.0f}, {5.f, 1.5f, 0.2f}, 1u),
        object(SdfPrimitiveType::Sphere, {1.9f, 0.5f, 0.2f}, {0.5f, 0.f, 0.f}, 2u),
    };
    return objects;
}

const std::array<compute::SdfObject, 3>& moireObjects() {
    static const std::array<compute::SdfObject, 3> objects{
        object(SdfPrimitiveType::Box, {-2.f, 1.f, -6.f}, {0.3f, 1.f, 0.3f}, 3u, 0.f, 0.03f),
        object(SdfPrimitiveType::Box, {2.5f, 1.f, -9.f}, {0.3f, 1.f, 0.3f}, 3u, 0.f, 0.03f),
        object(SdfPrimitiveType::Sphere, {0.6f, 0.6f, -4.f}, {0.6f, 0.f, 0.f}, 2u),
    };
    return objects;
}

/// Triangle wave in [-amplitude, amplitude] moving at `speed` units per frame.
f32 triangleWave(f32 frame, f32 speed, f32 amplitude) {
    const f32 period = 4.f * amplitude;
    f32 s = std::fmod(frame * speed + amplitude, period);
    if (s < 0.f) {
        s += period;
    }
    return s < 2.f * amplitude ? s - amplitude : 3.f * amplitude - s;
}

struct CameraPose {
    Vec3 position{};
    Vec3 target{};
};

CameraPose cameraPose(ReferenceSceneKind kind, f32 f) {
    switch (kind) {
    case ReferenceSceneKind::Showcase: {
        const Vec3 pos{-1.2f + 0.05f * f, 1.5f, 6.0f};
        return {pos, {0.45f * pos.x, 0.9f, 0.f}};
    }
    case ReferenceSceneKind::ThinFastObject:
        return {{0.f, 1.3f, 5.f}, {0.f, 1.0f, 0.f}};
    case ReferenceSceneKind::MoireFlight: {
        const Vec3 pos{0.3f * std::sin(0.05f * f), 0.55f, 8.f - 0.06f * f};
        const f32 yaw = 0.08f * std::sin(0.07f * f);
        return {pos, pos + Vec3{std::sin(yaw), -0.12f, -std::cos(yaw)}};
    }
    }
    return {};
}

rk::Camera kernelCamera(const CameraPose& pose, f32 aspect) {
    rk::Camera c{};
    c.position = pose.position;
    c.forward = (pose.target - pose.position).normalized();
    c.right = math::cross(c.forward, Vec3{0.f, 1.f, 0.f}).normalized();
    c.up = math::cross(c.right, c.forward);
    c.tan_half_y = std::tan(0.5f * kFovDeg * kPi / 180.f);
    c.aspect = aspect;
    return c;
}

void growBounds(Vec3& lo, Vec3& hi, const Vec3& c, const Vec3& half) {
    lo = {std::min(lo.x, c.x - half.x), std::min(lo.y, c.y - half.y), std::min(lo.z, c.z - half.z)};
    hi = {std::max(hi.x, c.x + half.x), std::max(hi.y, c.y + half.y), std::max(hi.z, c.z + half.z)};
}

template <usize N>
void setStatic(rk::SceneState& s, const std::array<compute::SdfObject, N>& objects) {
    s.sdf.objects = objects.data();
    s.sdf.object_count = static_cast<u32>(N);
    for (const compute::SdfObject& o : objects) {
        Vec3 half{};
        switch (static_cast<SdfPrimitiveType>(o.type)) {
        case SdfPrimitiveType::Sphere:
            half = {o.params.x, o.params.x, o.params.x};
            break;
        case SdfPrimitiveType::Box:
            half = o.params;
            break;
        case SdfPrimitiveType::Capsule:
            half = {o.params.x, o.params.x + o.params.y, o.params.x};
            break;
        case SdfPrimitiveType::Torus:
            half = {o.params.x + o.params.y, o.params.y, o.params.x + o.params.y};
            break;
        }
        growBounds(s.bounds_min, s.bounds_max, o.position, half + Vec3{0.3f, 0.3f, 0.3f});
    }
}

f32 thinX(ReferenceSceneKind kind, f32 f) {
    return kind == ReferenceSceneKind::Showcase ? triangleWave(f, 0.18f, 2.2f) : triangleWave(f, 0.2f, 2.0f);
}

rk::SceneState sceneState(ReferenceSceneKind kind, u32 frame, f32 aspect) {
    rk::SceneState s{};
    const f32 f = static_cast<f32>(frame);
    const f32 fp = frame == 0u ? f : f - 1.f;
    s.camera = kernelCamera(cameraPose(kind, f), aspect);
    s.prev_camera = kernelCamera(cameraPose(kind, fp), aspect);
    s.sdf.max_steps = 96;
    s.sdf.min_dist = 5e-4f;
    s.sdf.max_dist = 80.f;
    s.bounds_min = {1e30f, 1e30f, 1e30f};
    s.bounds_max = {-1e30f, -1e30f, -1e30f};
    switch (kind) {
    case ReferenceSceneKind::Showcase: {
        setStatic(s, showcaseObjects());
        s.has_thin = 1u;
        s.thin_pos = {thinX(kind, f), 1.0f, 3.6f};
        s.thin_prev_pos = {thinX(kind, fp), 1.0f, 3.6f};
        s.thin_radius = 0.035f;
        s.thin_half_length = 0.7f;
        growBounds(s.bounds_min, s.bounds_max, {0.f, 1.0f, 3.6f}, {2.2f + 0.3f, 1.0f, 0.3f});
        s.has_arm = 1u;
        s.arm_pivot = {1.9f, 1.3f, 1.0f};
        s.arm_angle = 0.12f * f;
        s.arm_prev_angle = 0.12f * fp;
        s.arm_length = 1.0f;
        s.arm_radius = 0.08f;
        growBounds(s.bounds_min, s.bounds_max, s.arm_pivot, {1.3f, 1.3f, 0.3f});
        s.particle_count = 6u;
        s.particle_velocity = {0.f, 0.03f, 0.f};
        for (u32 i = 0; i < s.particle_count; ++i) {
            const f32 phase = 0.41f * static_cast<f32>(i);
            const f32 rise = std::fmod(0.03f * f + phase * 2.4f, 2.4f);
            rk::Particle& p = s.particles[i];
            p.position = {-2.3f + 0.3f * static_cast<f32>(i), 0.3f + rise, 3.0f + 0.2f * static_cast<f32>(i % 3u)};
            p.color = {1.0f, 0.55f + 0.05f * static_cast<f32>(i % 2u), 0.2f};
            p.radius = 0.28f;
            p.alpha = 0.55f;
        }
        break;
    }
    case ReferenceSceneKind::ThinFastObject:
        setStatic(s, thinFastObjects());
        s.has_thin = 1u;
        s.thin_pos = {thinX(kind, f), 1.1f, 1.5f};
        s.thin_prev_pos = {thinX(kind, fp), 1.1f, 1.5f};
        s.thin_radius = 0.035f;
        s.thin_half_length = 0.8f;
        growBounds(s.bounds_min, s.bounds_max, {0.f, 1.1f, 1.5f}, {2.3f, 1.2f, 0.3f});
        s.has_arm = 1u;
        s.arm_pivot = {-1.8f, 1.6f, 0.8f};
        s.arm_angle = 0.1f * f;
        s.arm_prev_angle = 0.1f * fp;
        s.arm_length = 0.9f;
        s.arm_radius = 0.07f;
        growBounds(s.bounds_min, s.bounds_max, s.arm_pivot, {1.2f, 1.2f, 0.3f});
        break;
    case ReferenceSceneKind::MoireFlight:
        setStatic(s, moireObjects());
        s.checker_frequency = 3.f;
        s.stripe_frequency = 9.f;
        s.stripe_amount = 0.4f;
        break;
    }
    return s;
}

/// Fraction of the pixel [px, px+1) x [py, py+1) covered by the axis-aligned rectangle [x0, x1) x [y0, y1).
f32 rectCoverage(f32 px, f32 py, f32 x0, f32 y0, f32 x1, f32 y1) {
    const f32 w = std::max(0.f, std::min(px + 1.f, x1) - std::max(px, x0));
    const f32 h = std::max(0.f, std::min(py + 1.f, y1) - std::max(py, y0));
    return w * h;
}

/// Premultiplied "over": dst = src + dst * (1 - src.a).
Vec4 over(const Vec4& src, const Vec4& dst) {
    const f32 k = 1.f - src.w;
    return {src.x + dst.x * k, src.y + dst.y * k, src.z + dst.z * k, src.w + dst.w * k};
}

template <typename T>
kernel::Span<T> spanOf(std::vector<T>& v) {
    return kernel::make_span(v.data(), static_cast<u32>(v.size()));
}

} // namespace

const char* referenceSceneLabel(ReferenceSceneKind kind) {
    switch (kind) {
    case ReferenceSceneKind::Showcase:
        return "Showcase";
    case ReferenceSceneKind::ThinFastObject:
        return "ThinFastObject";
    case ReferenceSceneKind::MoireFlight:
        return "MoireFlight";
    }
    return "Unknown";
}

UpscaleInputs ReferenceFrame::inputs() const {
    UpscaleInputs in{};
    in.resolution = resolution;
    in.color = color.data();
    in.depth = depth.data();
    in.motion = motion.data();
    in.reactive = reactive.empty() ? nullptr : reactive.data();
    in.transparency_composition = transparency.empty() ? nullptr : transparency.data();
    in.ui = ui.empty() ? nullptr : ui.data();
    in.jitter_px = jitter_px;
    in.jitter_phase = jitter_phase;
    in.jitter_phase_count = jitter_phase_count;
    in.exposure = exposure;
    in.mip_bias = mip_bias;
    in.frame_index = frame_index;
    in.camera = camera;
    in.previous_camera = previous_camera;
    return in;
}

ReferenceScene::ReferenceScene(ReferenceSceneKind kind) : m_kind(kind) {}

UpscaleCamera ReferenceScene::camera(u32 frame, f32 aspect) const {
    const CameraPose pose = cameraPose(m_kind, static_cast<f32>(frame));
    UpscaleCamera c{};
    c.position = pose.position;
    c.view = math::lookAt(pose.position, pose.target, Vec3{0.f, 1.f, 0.f});
    c.near_plane = 0.05f;
    c.far_plane = 200.f;
    c.projection = math::perspective(kFovDeg, aspect, c.near_plane, c.far_plane);
    c.vertical_fov_rad = kFovDeg * kPi / 180.f;
    c.aspect = aspect;
    return c;
}

bool ReferenceScene::renderFrame(u32 frame, const UpscaleResolution& resolution, ReferenceFrame& out,
                                 kernel::Backend backend, bool withUi) const {
    if (!resolution.valid()) {
        return false;
    }
    const u32 w = resolution.render_width;
    const u32 h = resolution.render_height;
    const usize n = static_cast<usize>(w) * h;
    // Aspect of the display (render keeps it up to rounding; the projection is defined by the display).
    const f32 aspect = static_cast<f32>(resolution.display_width) / static_cast<f32>(resolution.display_height);
    out.resolution = resolution;
    out.frame_index = frame;
    out.jitter_phase_count = upscaleJitterPhaseCount(resolution);
    out.jitter_phase = frame % out.jitter_phase_count;
    out.jitter_px = upscaleJitterOffset(frame, out.jitter_phase_count);
    out.mip_bias = upscaleTextureMipBias(resolution);
    out.exposure = 1.f;
    out.camera = camera(frame, aspect);
    out.previous_camera = camera(frame == 0u ? 0u : frame - 1u, aspect);
    out.color.resize(n);
    out.depth.resize(n);
    out.motion.resize(n);
    out.reactive.resize(n);
    out.transparency.resize(n);
    out.object_id.resize(n);

    rk::Params p{};
    p.scene = sceneState(m_kind, frame, aspect);
    p.width = w;
    p.height = h;
    p.jitter_px = out.jitter_px;
    p.samples_per_axis = 1u;
    p.footprint_scale = std::exp2(out.mip_bias);
    p.color = spanOf(out.color);
    p.depth = spanOf(out.depth);
    p.motion = spanOf(out.motion);
    p.reactive = spanOf(out.reactive);
    p.transparency = spanOf(out.transparency);
    p.object_id = spanOf(out.object_id);
    const kernel::KernelLaunch launch{rk::kName, kernel::extent2(w, h), rk::kWorkgroup};
    if (!kernel::launch(backend, launch, rk::Kernel{}, p).ok) {
        return false;
    }
    if (withUi) {
        renderUi(resolution.display_width, resolution.display_height, frame, out.ui);
    } else {
        out.ui.clear();
    }
    return true;
}

bool ReferenceScene::renderGroundTruth(u32 frame, u32 width, u32 height, u32 samplesPerAxis, GroundTruthFrame& out,
                                       kernel::Backend backend) const {
    if (width == 0u || height == 0u || samplesPerAxis == 0u || samplesPerAxis > rk::kMaxSamplesPerAxis) {
        return false;
    }
    const usize n = static_cast<usize>(width) * height;
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    out.width = width;
    out.height = height;
    out.color.resize(n);
    out.depth.resize(n);
    out.motion.resize(n);
    out.object_id.resize(n);
    out.disoccluded.resize(n);
    rk::Params p{};
    p.scene = sceneState(m_kind, frame, aspect);
    p.width = width;
    p.height = height;
    p.samples_per_axis = samplesPerAxis;
    p.footprint_scale = 1.f / static_cast<f32>(samplesPerAxis);
    p.color = spanOf(out.color);
    p.depth = spanOf(out.depth);
    p.motion = spanOf(out.motion);
    p.object_id = spanOf(out.object_id);
    p.disoccluded = spanOf(out.disoccluded);
    const kernel::KernelLaunch launch{rk::kName, kernel::extent2(width, height), rk::kWorkgroup};
    return kernel::launch(backend, launch, rk::Kernel{}, p).ok;
}

void ReferenceScene::renderUi(u32 width, u32 height, u32 frame, std::vector<Vec4>& out) {
    out.assign(static_cast<usize>(width) * height, Vec4{});
    const f32 cx = 0.5f * static_cast<f32>(width);
    const f32 cy = 0.5f * static_cast<f32>(height);
    const f32 arm = 0.03f * static_cast<f32>(height) + 2.f;
    const f32 thick = 0.75f; // sub-pixel stroke: anti-aliased edges in the HUD itself
    const f32 barW = 0.25f * static_cast<f32>(width);
    const f32 fill = 0.35f + 0.5f * (0.5f + 0.5f * std::sin(0.2f * static_cast<f32>(frame)));
    const f32 barX0 = 6.f;
    const f32 barY0 = static_cast<f32>(height) - 12.f;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 px = static_cast<f32>(x);
            const f32 py = static_cast<f32>(y);
            Vec4 c{};
            // Bar background, then fill.
            const f32 bg = rectCoverage(px, py, barX0, barY0, barX0 + barW, barY0 + 6.5f);
            c = over(Vec4{0.02f * 0.6f * bg, 0.02f * 0.6f * bg, 0.02f * 0.6f * bg, 0.6f * bg}, c);
            const f32 fg = rectCoverage(px, py, barX0 + 1.f, barY0 + 1.f, barX0 + 1.f + (barW - 2.f) * fill, barY0 + 5.5f);
            c = over(Vec4{0.2f * fg, 0.85f * fg, 0.3f * fg, fg}, c);
            // Crosshair (white, 90% opacity), 0.75 px strokes offset by a quarter pixel.
            const f32 h = rectCoverage(px, py, cx - arm, cy - 0.5f * thick + 0.25f, cx + arm, cy + 0.5f * thick + 0.25f);
            const f32 v = rectCoverage(px, py, cx - 0.5f * thick + 0.25f, cy - arm, cx + 0.5f * thick + 0.25f, cy + arm);
            const f32 a = 0.9f * std::min(1.f, h + v);
            c = over(Vec4{a, a, a, a}, c);
            out[static_cast<usize>(y) * width + x] = c;
        }
    }
}

} // namespace fuse::renderer::refscene
