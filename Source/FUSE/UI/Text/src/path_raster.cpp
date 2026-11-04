// Path coverage rasteriser: non-zero winding, exact-area anti-aliasing by signed-area accumulation (the font-rs /
// stb_truetype v2 scheme). Curves are flattened in pixel space; contours are closed implicitly.
#include "glyph_paint.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::text::detail {
namespace {

constexpr double kTolerance = 0.05; // max flattening error in pixels
constexpr int kMaxSegments = 1024;  // per curve

struct Pt {
    double x, y;
};

Pt apply(const Affine& m, double x, double y) { return {m.xx * x + m.xy * y + m.dx, m.yx * x + m.yy * y + m.dy}; }

// Accumulates the signed area of edges into `acc` (rows of stride w + 2: x is clamped to [0, w]).
class Accumulator {
public:
    Accumulator(int w, int h) : m_w(w), m_h(h), m_stride(std::size_t(w) + 2), m_acc(m_stride * std::size_t(h), 0.0) {}

    // Edge from a to b; x outside [0, w] is projected onto the nearest side (same coverage to the right).
    void line(Pt a, Pt b) {
        if (a.y == b.y || !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) return;
        const double w = double(m_w);
        // split at x = 0 and x = w so each piece lies on one side
        double ts[4] = {0.0, 1.0, 1.0, 1.0};
        int nt = 1;
        const double dx = b.x - a.x;
        if (dx != 0.0) {
            for (const double edge : {0.0, w}) {
                const double t = (edge - a.x) / dx;
                if (t > 0.0 && t < 1.0) ts[nt++] = t;
            }
        }
        ts[nt++] = 1.0;
        std::sort(ts, ts + nt);
        for (int i = 0; i + 1 < nt; ++i) {
            const double t0 = ts[i], t1 = ts[i + 1];
            if (t1 <= t0) continue;
            Pt p0{a.x + dx * t0, a.y + (b.y - a.y) * t0};
            Pt p1{a.x + dx * t1, a.y + (b.y - a.y) * t1};
            p0.x = std::clamp(p0.x, 0.0, w);
            p1.x = std::clamp(p1.x, 0.0, w);
            if (i == 0) p0 = {std::clamp(a.x, 0.0, w), a.y};
            if (i + 2 == nt) p1 = {std::clamp(b.x, 0.0, w), b.y};
            segment(p0, p1);
        }
    }

    void resolve(float* coverage) const {
        for (int y = 0; y < m_h; ++y) {
            const double* row = m_acc.data() + std::size_t(y) * m_stride;
            float* out = coverage + std::size_t(y) * std::size_t(m_w);
            double sum = 0.0;
            for (int x = 0; x < m_w; ++x) {
                sum += row[x];
                out[x] = float(std::min(std::abs(sum), 1.0));
            }
        }
    }

private:
    // Edge with x inside [0, w].
    void segment(Pt p0, Pt p1) {
        if (p0.y == p1.y) return;
        double dir = 1.0;
        if (p0.y > p1.y) {
            std::swap(p0, p1);
            dir = -1.0;
        }
        const double h = double(m_h);
        if (p1.y <= 0.0 || p0.y >= h) return;
        const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
        double x = p0.x;
        if (p0.y < 0.0) x = std::clamp(x - p0.y * dxdy, 0.0, double(m_w));
        const int yStart = std::max(0, int(std::floor(p0.y)));
        const int yEnd = std::min(m_h, int(std::ceil(p1.y)));
        const int maxIdx = m_w + 1;
        for (int y = yStart; y < yEnd; ++y) {
            double* row = m_acc.data() + std::size_t(y) * m_stride;
            const double dy = std::min(double(y + 1), p1.y) - std::max(double(y), p0.y);
            if (dy <= 0.0) continue;
            double xnext = x + dxdy * dy;
            xnext = std::clamp(xnext, 0.0, double(m_w)); // rounding only
            const double d = dy * dir;
            const double xa = std::min(x, xnext), xb = std::max(x, xnext);
            const double xaFloor = std::floor(xa);
            const int xai = int(xaFloor);
            const double xbCeil = std::ceil(xb);
            const int xbi = int(xbCeil);
            if (xbi <= xai + 1) {
                // within one pixel column: area right of the edge's midpoint
                const double xmf = 0.5 * (x + xnext) - xaFloor;
                row[std::min(xai, maxIdx)] += d - d * xmf;
                row[std::min(xai + 1, maxIdx)] += d * xmf;
            } else {
                const double s = 1.0 / (xb - xa);
                const double xaf = xa - xaFloor;
                const double a0 = 0.5 * s * (1.0 - xaf) * (1.0 - xaf);
                const double xbf = xb - xbCeil + 1.0;
                const double am = 0.5 * s * xbf * xbf;
                row[xai] += d * a0;
                if (xbi == xai + 2) {
                    row[xai + 1] += d * (1.0 - a0 - am);
                } else {
                    const double a1 = s * (1.5 - xaf);
                    row[xai + 1] += d * (a1 - a0);
                    for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += d * s;
                    const double a2 = a1 + double(xbi - xai - 3) * s;
                    row[xbi - 1] += d * (1.0 - a2 - am);
                }
                row[std::min(xbi, maxIdx)] += d * am;
            }
            x = xnext;
        }
    }

    int m_w, m_h;
    std::size_t m_stride;
    std::vector<double> m_acc;
};

int segmentsFor(double dd) {
    if (!(dd > 0.0)) return 1;
    const double n = std::ceil(std::sqrt(dd / (8.0 * kTolerance)));
    return int(std::clamp(n, 1.0, double(kMaxSegments)));
}

} // namespace

void rasterisePath(std::span<const OutlineCmd> path, const Affine& toPixels, float* coverage, int w, int h) {
    if (w <= 0 || h <= 0 || !coverage) return;
    Accumulator acc(w, h);
    Pt start{0, 0}, cur{0, 0};
    bool open = false;
    for (const OutlineCmd& c : path) {
        const Pt p = apply(toPixels, c.x, c.y);
        switch (c.kind) {
        case OutlineCmd::Move:
            if (open) acc.line(cur, start);
            start = cur = p;
            open = true;
            break;
        case OutlineCmd::Line:
            acc.line(cur, p);
            cur = p;
            break;
        case OutlineCmd::Quad: {
            const Pt q = apply(toPixels, c.cx, c.cy);
            // flattening error of n uniform segments <= |p0 - 2 p1 + p2| / (8 n^2)
            const int n = segmentsFor(std::hypot(cur.x - 2 * q.x + p.x, cur.y - 2 * q.y + p.y));
            Pt prev = cur;
            for (int i = 1; i <= n; ++i) {
                const double t = double(i) / n, u = 1.0 - t;
                const Pt e = i == n ? p : Pt{u * u * cur.x + 2 * u * t * q.x + t * t * p.x,
                                             u * u * cur.y + 2 * u * t * q.y + t * t * p.y};
                acc.line(prev, e);
                prev = e;
            }
            cur = p;
            break;
        }
        case OutlineCmd::Cubic: {
            const Pt q0 = apply(toPixels, c.cx, c.cy), q1 = apply(toPixels, c.cx1, c.cy1);
            // |B''| <= 6 max(|p0 - 2 p1 + p2|, |p1 - 2 p2 + p3|)
            const double dd = 0.75 * 8.0 *
                              std::max(std::hypot(cur.x - 2 * q0.x + q1.x, cur.y - 2 * q0.y + q1.y),
                                       std::hypot(q0.x - 2 * q1.x + p.x, q0.y - 2 * q1.y + p.y));
            const int n = segmentsFor(dd);
            Pt prev = cur;
            for (int i = 1; i <= n; ++i) {
                const double t = double(i) / n, u = 1.0 - t;
                const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
                const Pt e = i == n ? p
                                    : Pt{b0 * cur.x + b1 * q0.x + b2 * q1.x + b3 * p.x,
                                         b0 * cur.y + b1 * q0.y + b2 * q1.y + b3 * p.y};
                acc.line(prev, e);
                prev = e;
            }
            cur = p;
            break;
        }
        }
    }
    if (open) acc.line(cur, start);
    acc.resolve(coverage);
}

} // namespace fuse::text::detail
