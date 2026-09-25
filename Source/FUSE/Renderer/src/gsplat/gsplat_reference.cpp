// WP-9.2 CPU reference renderer: see include/fuse/renderer/gsplat/gsplat_reference.hpp. Every expression is
// written in the order of shaders/gsplat/gs_*.{comp,slang}; keep them in step.
#include <fuse/renderer/gsplat/gsplat_reference.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>

namespace fuse::renderer::gsplat {

namespace {
constexpr f32 kShC0 = 0.28209479177387814f;
constexpr f32 kShC1 = 0.4886025119029199f;
constexpr f32 kShC20 = 1.0925484305920792f;
constexpr f32 kShC21 = -1.0925484305920792f;
constexpr f32 kShC22 = 0.31539156525252005f;
constexpr f32 kShC23 = -1.0925484305920792f;
constexpr f32 kShC24 = 0.5462742152960396f;
constexpr f32 kShC30 = -0.5900435899266435f;
constexpr f32 kShC31 = 2.890611442640554f;
constexpr f32 kShC32 = -0.4570457994644658f;
constexpr f32 kShC33 = 0.3731763325901154f;
constexpr f32 kShC34 = -0.4570457994644658f;
constexpr f32 kShC35 = 1.445305721320277f;
constexpr f32 kShC36 = -0.5900435899266435f;

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (hi < v ? hi : v); }
} // namespace

GsCamera gs_camera_look_at(const f32 (&eye)[3], const f32 (&target)[3], const f32 (&up)[3], f32 fovY, u32 width,
                           u32 height, f32 nearPlane, f32 farPlane) {
    auto norm = [](f64 (&v)[3]) {
        const f64 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (f64& c : v) c = l > 0.0 ? c / l : 0.0;
    };
    auto cross = [](const f64 (&a)[3], const f64 (&b)[3], f64 (&o)[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1];
        o[1] = a[2] * b[0] - a[0] * b[2];
        o[2] = a[0] * b[1] - a[1] * b[0];
    };
    f64 f[3] = {f64(target[0]) - eye[0], f64(target[1]) - eye[1], f64(target[2]) - eye[2]};
    norm(f);
    const f64 u[3] = {up[0], up[1], up[2]};
    f64 r[3];
    cross(f, u, r);
    norm(r);
    f64 d[3];
    cross(f, r, d);
    GsCamera c{};
    const f64* rows[3] = {r, d, f};
    for (u32 i = 0; i < 3u; ++i) {
        c.view[i * 4u + 0u] = static_cast<f32>(rows[i][0]);
        c.view[i * 4u + 1u] = static_cast<f32>(rows[i][1]);
        c.view[i * 4u + 2u] = static_cast<f32>(rows[i][2]);
        c.view[i * 4u + 3u] = static_cast<f32>(-(rows[i][0] * eye[0] + rows[i][1] * eye[1] + rows[i][2] * eye[2]));
    }
    const f64 focal = 0.5 * height / std::tan(0.5 * fovY);
    c.fx = c.fy = static_cast<f32>(focal);
    c.cx = 0.5f * static_cast<f32>(width);
    c.cy = 0.5f * static_cast<f32>(height);
    c.nearZ = nearPlane;
    c.depthA = static_cast<f32>(f64(farPlane) / (f64(farPlane) - nearPlane));
    c.depthB = static_cast<f32>(-f64(farPlane) * nearPlane / (f64(farPlane) - nearPlane));
    c.reversedZ = false;
    return c;
}

bool gs_resolve_constants(const GsCamera& camera, const GsSettings& settings, u32 width, u32 height, u32 splatCount,
                          u32 assetShDegree, u32 capacity, bool hasDepth, GsFrameConstants& out) {
    if (width == 0u || height == 0u || !(camera.fx > 0.f) || !(camera.fy > 0.f) || !(camera.nearZ > 0.f)) {
        return false;
    }
    GsFrameConstants f{};
    f.splatCount = splatCount;
    f.capacity = capacity;
    f.width = width;
    f.height = height;
    f.tilesX = (width + kGsTile - 1u) / kGsTile;
    f.tilesY = (height + kGsTile - 1u) / kGsTile;
    f.tileCount = f.tilesX * f.tilesY;
    f.flags = 0u;
    if (hasDepth && settings.depthTest) {
        f.flags |= kGsFlagDepthTest;
        if (camera.reversedZ) {
            f.flags |= kGsFlagReversedZ;
        }
    }
    f.shDegree = std::min({settings.shDegree, assetShDegree, kGsMaxShDegree});
    std::memcpy(f.view, camera.view, sizeof(f.view));
    // Camera position -R^T t (f64, then rounded).
    for (u32 k = 0; k < 3u; ++k) {
        f64 p = 0.0;
        for (u32 i = 0; i < 3u; ++i) {
            p -= f64(camera.view[i * 4u + k]) * camera.view[i * 4u + 3u];
        }
        f.camPos[k] = static_cast<f32>(p);
    }
    f.fx = camera.fx;
    f.fy = camera.fy;
    f.cx = camera.cx;
    f.cy = camera.cy;
    f.nearZ = camera.nearZ;
    f.tanFovX = static_cast<f32>(0.5 * width / camera.fx);
    f.tanFovY = static_cast<f32>(0.5 * height / camera.fy);
    f.lowPass = settings.lowPass;
    f.depthA = camera.depthA;
    f.depthB = camera.depthB;
    f.alphaMin = settings.alphaMin;
    f.transmittanceMin = settings.transmittanceMin;
    out = f;
    return true;
}

u32 gs_sort_key_bits(u32 tileCount) {
    const u32 tileBits = static_cast<u32>(std::bit_width(tileCount));
    u32 passes = (32u + tileBits + 7u) / 8u;
    if ((passes & 1u) != 0u) {
        ++passes;
    }
    return std::min(passes * 8u, 64u);
}

void gs_eval_sh(const GsSplat& s, u32 degree, f32 x, f32 y, f32 z, f32 (&rgb)[3]) {
    const f32 xx = x * x;
    const f32 yy = y * y;
    const f32 zz = z * z;
    const f32 xy = x * y;
    const f32 yz = y * z;
    const f32 xz = x * z;
    for (u32 c = 0; c < 3u; ++c) {
        const f32* sh = s.sh;
        f32 res = kShC0 * sh[0 * 3 + c];
        if (degree > 0u) {
            res = res - kShC1 * y * sh[1 * 3 + c] + kShC1 * z * sh[2 * 3 + c] - kShC1 * x * sh[3 * 3 + c];
            if (degree > 1u) {
                res = res + kShC20 * xy * sh[4 * 3 + c] + kShC21 * yz * sh[5 * 3 + c] +
                      kShC22 * (2.f * zz - xx - yy) * sh[6 * 3 + c] + kShC23 * xz * sh[7 * 3 + c] +
                      kShC24 * (xx - yy) * sh[8 * 3 + c];
                if (degree > 2u) {
                    res = res + kShC30 * y * (3.f * xx - yy) * sh[9 * 3 + c] + kShC31 * xy * z * sh[10 * 3 + c] +
                          kShC32 * y * (4.f * zz - xx - yy) * sh[11 * 3 + c] +
                          kShC33 * z * (2.f * zz - 3.f * xx - 3.f * yy) * sh[12 * 3 + c] +
                          kShC34 * x * (4.f * zz - xx - yy) * sh[13 * 3 + c] + kShC35 * z * (xx - yy) * sh[14 * 3 + c] +
                          kShC36 * x * (xx - 3.f * yy) * sh[15 * 3 + c];
                }
            }
        }
        res = res + 0.5f;
        rgb[c] = res > 0.f ? res : 0.f;
    }
}

GsProjected gs_preprocess_splat(const GsSplat& s, const GsFrameConstants& f) {
    GsProjected p{};
    const f32* v = f.view;
    const f32 px = s.position[0];
    const f32 py = s.position[1];
    const f32 pz = s.position[2];
    const f32 tx = v[0] * px + v[1] * py + v[2] * pz + v[3];
    const f32 ty = v[4] * px + v[5] * py + v[6] * pz + v[7];
    const f32 tz = v[8] * px + v[9] * py + v[10] * pz + v[11];
    if (!(tz > f.nearZ)) {
        return p;
    }
    // 3D covariance: M = R(q) diag(s), Sigma = M M^T.
    const f32 qw = s.rotation[0];
    const f32 qx = s.rotation[1];
    const f32 qy = s.rotation[2];
    const f32 qz = s.rotation[3];
    const f32 sx = s.scale[0];
    const f32 sy = s.scale[1];
    const f32 sz = s.scale[2];
    const f32 m00 = (1.f - 2.f * (qy * qy + qz * qz)) * sx;
    const f32 m01 = (2.f * (qx * qy - qw * qz)) * sy;
    const f32 m02 = (2.f * (qx * qz + qw * qy)) * sz;
    const f32 m10 = (2.f * (qx * qy + qw * qz)) * sx;
    const f32 m11 = (1.f - 2.f * (qx * qx + qz * qz)) * sy;
    const f32 m12 = (2.f * (qy * qz - qw * qx)) * sz;
    const f32 m20 = (2.f * (qx * qz - qw * qy)) * sx;
    const f32 m21 = (2.f * (qy * qz + qw * qx)) * sy;
    const f32 m22 = (1.f - 2.f * (qx * qx + qy * qy)) * sz;
    const f32 s00 = m00 * m00 + m01 * m01 + m02 * m02;
    const f32 s01 = m00 * m10 + m01 * m11 + m02 * m12;
    const f32 s02 = m00 * m20 + m01 * m21 + m02 * m22;
    const f32 s11 = m10 * m10 + m11 * m11 + m12 * m12;
    const f32 s12 = m10 * m20 + m11 * m21 + m12 * m22;
    const f32 s22 = m20 * m20 + m21 * m21 + m22 * m22;
    // EWA: J (perspective Jacobian at the clamped position) times the view rotation.
    const f32 limx = 1.3f * f.tanFovX;
    const f32 limy = 1.3f * f.tanFovY;
    const f32 cxz = clampf(tx / tz, -limx, limx) * tz;
    const f32 cyz = clampf(ty / tz, -limy, limy) * tz;
    const f32 tz2 = tz * tz;
    const f32 j00 = f.fx / tz;
    const f32 j02 = -(f.fx * cxz) / tz2;
    const f32 j11 = f.fy / tz;
    const f32 j12 = -(f.fy * cyz) / tz2;
    const f32 t00 = j00 * v[0] + j02 * v[8];
    const f32 t01 = j00 * v[1] + j02 * v[9];
    const f32 t02 = j00 * v[2] + j02 * v[10];
    const f32 t10 = j11 * v[4] + j12 * v[8];
    const f32 t11 = j11 * v[5] + j12 * v[9];
    const f32 t12 = j11 * v[6] + j12 * v[10];
    const f32 u00 = t00 * s00 + t01 * s01 + t02 * s02;
    const f32 u01 = t00 * s01 + t01 * s11 + t02 * s12;
    const f32 u02 = t00 * s02 + t01 * s12 + t02 * s22;
    const f32 u10 = t10 * s00 + t11 * s01 + t12 * s02;
    const f32 u11 = t10 * s01 + t11 * s11 + t12 * s12;
    const f32 u12 = t10 * s02 + t11 * s12 + t12 * s22;
    const f32 a = u00 * t00 + u01 * t01 + u02 * t02 + f.lowPass;
    const f32 b = u00 * t10 + u01 * t11 + u02 * t12;
    const f32 c = u10 * t10 + u11 * t11 + u12 * t12 + f.lowPass;
    const f32 det = a * c - b * b;
    if (!(det > 0.f)) {
        return p;
    }
    const f32 inv = 1.f / det;
    const f32 mid = 0.5f * (a + c);
    const f32 disc = std::sqrt(std::max(0.1f, mid * mid - det));
    const f32 lambda = mid + disc;
    const f32 radius = std::ceil(3.f * std::sqrt(lambda));
    const f32 mx = f.fx * tx / tz + f.cx;
    const f32 my = f.fy * ty / tz + f.cy;
    const f32 tiles = static_cast<f32>(kGsTile);
    const u32 minX = static_cast<u32>(clampf((mx - radius) / tiles, 0.f, static_cast<f32>(f.tilesX)));
    const u32 minY = static_cast<u32>(clampf((my - radius) / tiles, 0.f, static_cast<f32>(f.tilesY)));
    const u32 maxX = static_cast<u32>(clampf((mx + radius + (tiles - 1.f)) / tiles, 0.f, static_cast<f32>(f.tilesX)));
    const u32 maxY = static_cast<u32>(clampf((my + radius + (tiles - 1.f)) / tiles, 0.f, static_cast<f32>(f.tilesY)));
    if (!(radius > 0.f) || maxX <= minX || maxY <= minY) {
        return p;
    }
    // View-dependent colour: direction camera -> splat.
    const f32 dx = px - f.camPos[0];
    const f32 dy = py - f.camPos[1];
    const f32 dz = pz - f.camPos[2];
    const f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
    f32 nx = 0.f;
    f32 ny = 0.f;
    f32 nz = 0.f;
    if (len > 0.f) {
        nx = dx / len;
        ny = dy / len;
        nz = dz / len;
    }
    gs_eval_sh(s, f.shDegree, nx, ny, nz, p.color);
    p.mean[0] = mx;
    p.mean[1] = my;
    p.viewZ = tz;
    p.depth = f.depthA + f.depthB / tz;
    p.conic[0] = c * inv;
    p.conic[1] = -b * inv;
    p.conic[2] = a * inv;
    p.opacity = s.opacity;
    p.radius = static_cast<u32>(radius);
    p.rect[0] = minX;
    p.rect[1] = minY;
    p.rect[2] = maxX;
    p.rect[3] = maxY;
    return p;
}

void gs_raster_pixel(const GsProjected* projected, const u32* values, u32 begin, u32 end, u32 x, u32 y,
                     f32 sceneDepth, const GsFrameConstants& f, f32 (&out)[4]) {
    const f32 pxc = static_cast<f32>(x) + 0.5f;
    const f32 pyc = static_cast<f32>(y) + 0.5f;
    const bool depthTest = (f.flags & kGsFlagDepthTest) != 0u;
    const bool reversed = (f.flags & kGsFlagReversedZ) != 0u;
    f32 t = 1.f;
    f32 r = 0.f;
    f32 g = 0.f;
    f32 b = 0.f;
    for (u32 i = begin; i < end; ++i) {
        const GsProjected& p = projected[values[i]];
        if (depthTest && !(reversed ? p.depth > sceneDepth : p.depth < sceneDepth)) {
            break;
        }
        const f32 dx = p.mean[0] - pxc;
        const f32 dy = p.mean[1] - pyc;
        const f32 power = -0.5f * (p.conic[0] * dx * dx + p.conic[2] * dy * dy) - p.conic[1] * dx * dy;
        if (power > 0.f) {
            continue;
        }
        const f32 alpha = std::min(0.99f, p.opacity * std::exp(power));
        if (alpha < f.alphaMin) {
            continue;
        }
        const f32 testT = t * (1.f - alpha);
        if (testT < f.transmittanceMin) {
            break;
        }
        r = r + p.color[0] * alpha * t;
        g = g + p.color[1] * alpha * t;
        b = b + p.color[2] * alpha * t;
        t = testT;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = t;
}

void gs_render_reference(const GsSplat* splats, const GsFrameConstants& f, const f32* sceneDepth, GsReferenceFrame& out) {
    const u32 n = f.splatCount;
    out.projected.assign(n, GsProjected{});
    out.offsets.assign(n, 0u);
    u32 total = 0;
    for (u32 i = 0; i < n; ++i) {
        out.projected[i] = gs_preprocess_splat(splats[i], f);
        out.offsets[i] = total;
        total += gs_tiles_touched(out.projected[i]);
    }
    out.entries = total;
    out.dropped = total > f.capacity ? total - f.capacity : 0u;
    const u32 count = std::min(total, f.capacity);
    out.keys.assign(f.capacity, gs_pad_key(f.tileCount));
    out.values.assign(f.capacity, kGsPadValue);
    for (u32 i = 0; i < n; ++i) {
        const GsProjected& p = out.projected[i];
        u32 at = out.offsets[i];
        const u64 depthBits = std::bit_cast<u32>(p.viewZ);
        for (u32 ty = p.rect[1]; ty < p.rect[3]; ++ty) {
            for (u32 tx = p.rect[0]; tx < p.rect[2]; ++tx) {
                if (at < f.capacity) {
                    out.keys[at] = (static_cast<u64>(ty * f.tilesX + tx) << 32) | depthBits;
                    out.values[at] = i;
                }
                ++at;
            }
        }
    }
    std::vector<u32> order(f.capacity);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return out.keys[a] < out.keys[b]; });
    std::vector<u64> keys(f.capacity);
    std::vector<u32> values(f.capacity);
    for (u32 i = 0; i < f.capacity; ++i) {
        keys[i] = out.keys[order[i]];
        values[i] = out.values[order[i]];
    }
    out.keys.swap(keys);
    out.values.swap(values);
    out.ranges.assign(static_cast<usize>(f.tileCount) * 2u, 0u);
    for (u32 i = 0; i < count; ++i) {
        const u32 tile = static_cast<u32>(out.keys[i] >> 32);
        if (i == 0u || static_cast<u32>(out.keys[i - 1u] >> 32) != tile) {
            out.ranges[tile * 2u] = i;
        }
        if (i + 1u == count || static_cast<u32>(out.keys[i + 1u] >> 32) != tile) {
            out.ranges[tile * 2u + 1u] = i + 1u;
        }
    }
    out.image.assign(static_cast<usize>(f.width) * f.height * 4u, 0.f);
    for (u32 y = 0; y < f.height; ++y) {
        for (u32 x = 0; x < f.width; ++x) {
            const u32 tile = (y / kGsTile) * f.tilesX + x / kGsTile;
            const usize pixel = static_cast<usize>(y) * f.width + x;
            f32 px[4];
            gs_raster_pixel(out.projected.data(), out.values.data(), out.ranges[tile * 2u], out.ranges[tile * 2u + 1u], x, y,
                            sceneDepth != nullptr ? sceneDepth[pixel] : 1.f, f, px);
            std::memcpy(&out.image[pixel * 4u], px, sizeof(px));
        }
    }
}

f64 gs_psnr(const f32* a, const f32* b, usize pixels, u32 stride) {
    f64 se = 0.0;
    for (usize i = 0; i < pixels; ++i) {
        for (u32 c = 0; c < 3u; ++c) {
            const f64 d = f64(a[i * stride + c]) - f64(b[i * stride + c]);
            se += d * d;
        }
    }
    if (pixels == 0u || se == 0.0) {
        return 200.0;
    }
    const f64 mse = se / (3.0 * f64(pixels));
    return 10.0 * std::log10(1.0 / mse);
}

} // namespace fuse::renderer::gsplat
