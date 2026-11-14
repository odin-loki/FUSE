// Multi-channel signed distance fields of glyph outlines: RGB hold per-channel pseudo-distances, A the true distance.
// Algorithm after msdfgen by Viktor Chlumsky (MIT): simple edge colouring, pseudo-distances, overlapping-contour
// combining, scanline sign correction and artifact correction with edge / corner protection. Own implementation;
// single-threaded and free of libm transcendentals, so the output is a pure function of the input.
#include "glyph_paint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace fuse::text::detail {
namespace {

// ---- vector math ----

struct V2 {
    double x = 0, y = 0;
    bool operator==(const V2&) const = default;
};
V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 operator*(double s, V2 a) { return {s * a.x, s * a.y}; }
double dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
double cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
double length(V2 a) { return std::sqrt(dot(a, a)); }
V2 normalize(V2 a) {
    const double l = length(a);
    return l == 0 ? V2{0, 1} : V2{a.x / l, a.y / l};
}
V2 mix(V2 a, V2 b, double t) { return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)}; }
double nonZeroSign(double v) { return v > 0 ? 1.0 : -1.0; }
double median(double a, double b, double c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

// ---- polynomial roots ----

// Real roots of a t^2 + b t + c; -1 when every t is a root.
int solveQuadratic(std::array<double, 3>& x, double a, double b, double c) {
    if (a == 0 || std::abs(b) > 1e12 * std::abs(a)) {
        if (b == 0) return c == 0 ? -1 : 0;
        x[0] = -c / b;
        return 1;
    }
    double dscr = b * b - 4 * a * c;
    if (dscr > 0) {
        dscr = std::sqrt(dscr);
        x[0] = (-b + dscr) / (2 * a);
        x[1] = (-b - dscr) / (2 * a);
        return 2;
    }
    if (dscr == 0) {
        x[0] = -b / (2 * a);
        return 1;
    }
    return 0;
}

// Roots of a t^3 + b t^2 + c t + d inside (0, 1): split at the critical points, then bracketed Newton on each
// monotonic piece. Uses only + - * / sqrt, so results are bit-identical across standard libraries.
int cubicRootsInUnit(std::array<double, 3>& out, double a, double b, double c, double d) {
    const auto f = [&](double t) { return ((a * t + b) * t + c) * t + d; };
    const auto df = [&](double t) { return (3 * a * t + 2 * b) * t + c; };
    std::array<double, 3> cp{};
    int m = solveQuadratic(cp, 3 * a, 2 * b, c);
    if (m == 2 && cp[0] > cp[1]) std::swap(cp[0], cp[1]);
    std::array<double, 4> ends{0, 1, 1, 1};
    std::size_t n = 1;
    for (int i = 0; i < m; ++i) {
        const double t = cp[static_cast<std::size_t>(i)];
        if (t > ends[n - 1] && t < 1) ends[n++] = t;
    }
    ends[n++] = 1;

    int count = 0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        double lo = ends[i], hi = ends[i + 1];
        const double flo = f(lo), fhi = f(hi);
        if (flo == 0) {
            if (lo > 0) out[static_cast<std::size_t>(count++)] = lo;
            continue;
        }
        if (fhi == 0 || (flo < 0) == (fhi < 0)) continue; // a root at hi is the next piece's lo
        const bool loNegative = flo < 0;
        double t = 0.5 * (lo + hi);
        for (int it = 0; it < 64; ++it) {
            const double ft = f(t);
            if (ft == 0) break;
            if ((ft < 0) == loNegative)
                lo = t;
            else
                hi = t;
            const double slope = df(t);
            double next = slope != 0 ? t - ft / slope : lo;
            if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
            const bool done = std::abs(next - t) <= 1e-15 || hi - lo <= 1e-15;
            t = next;
            if (done) break;
        }
        out[static_cast<std::size_t>(count++)] = t;
    }
    return count;
}

// ---- edge segments ----

enum Color : std::uint8_t { Black = 0, Red = 1, Green = 2, Yellow = 3, Blue = 4, Magenta = 5, Cyan = 6, White = 7 };

constexpr int kCubicSearchStarts = 4;
constexpr int kCubicSearchSteps = 4;

// Distance with the orthogonality tie-break (smaller dot = more orthogonal = preferred).
struct SignedDist {
    double d = -1e240;
    double dot = 1;
};
bool closer(const SignedDist& a, const SignedDist& b) {
    const double fa = std::abs(a.d), fb = std::abs(b.d);
    return fa < fb || (fa == fb && a.dot < b.dot);
}

struct Edge {
    int order = 1; // 1 linear, 2 quadratic, 3 cubic
    std::array<V2, 4> p{};
    std::uint8_t color = White;

    // Cached by prepare() for the distance queries.
    V2 lo, hi;                 // control point bounds
    V2 d0, d1;                 // tangents at the start and the end
    V2 n0, n1;                 // the same, normalised
    double inv0 = 0, inv1 = 0; // 1 / |d0|^2, 1 / |d1|^2
    V2 ab, br, as;             // polynomial terms
    double invLen = 0;         // lines: 1 / |ab|
    double ca = 0, cb = 0, cc = 0; // quadratics: the query-independent cubic coefficients

    V2 start() const { return p[0]; }
    V2 end() const { return p[static_cast<std::size_t>(order)]; }

    V2 point(double t) const {
        if (order == 1) return mix(p[0], p[1], t);
        if (order == 2) return mix(mix(p[0], p[1], t), mix(p[1], p[2], t), t);
        const V2 p12 = mix(p[1], p[2], t);
        return mix(mix(mix(p[0], p[1], t), p12, t), mix(p12, mix(p[2], p[3], t), t), t);
    }

    V2 direction(double t) const {
        if (order == 1) return p[1] - p[0];
        if (order == 2) {
            const V2 d = mix(p[1] - p[0], p[2] - p[1], t);
            return d == V2{} ? p[2] - p[0] : d;
        }
        const V2 d = mix(mix(p[1] - p[0], p[2] - p[1], t), mix(p[2] - p[1], p[3] - p[2], t), t);
        if (d == V2{}) {
            if (t == 0) return p[2] - p[0];
            if (t == 1) return p[3] - p[1];
        }
        return d;
    }

    void prepare() {
        lo = hi = p[0];
        for (int i = 1; i <= order; ++i) {
            const V2 q = p[static_cast<std::size_t>(i)];
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y)};
        }
        d0 = direction(0);
        d1 = direction(1);
        n0 = normalize(d0);
        n1 = normalize(d1);
        inv0 = dot(d0, d0) > 0 ? 1 / dot(d0, d0) : 0;
        inv1 = dot(d1, d1) > 0 ? 1 / dot(d1, d1) : 0;
        ab = p[1] - p[0];
        br = p[2] - p[1] - ab;
        as = order == 3 ? (p[3] - p[2]) - (p[2] - p[1]) - br : V2{};
        invLen = order == 1 ? 1 / length(ab) : 0;
        ca = dot(br, br);
        cb = 3 * dot(ab, br);
        cc = 2 * dot(ab, ab);
    }

    void reverse() { std::reverse(p.begin(), p.begin() + order + 1); }

    SignedDist endpointResult(double minDistance, double param, V2 q) const {
        if (param >= 0 && param <= 1) return {minDistance, 0};
        if (param < 0.5) return {minDistance, std::abs(dot(n0, normalize(p[0] - q)))};
        return {minDistance, std::abs(dot(n1, normalize(end() - q)))};
    }

    SignedDist distance(V2 q, double& param) const {
        if (order == 1) {
            const V2 aq = q - p[0];
            param = dot(aq, ab) * inv0;
            if (param > 0 && param < 1) {
                const double ortho = cross(aq, ab) * invLen;
                const V2 eq = (param > 0.5 ? p[1] : p[0]) - q;
                if (ortho * ortho < dot(eq, eq)) return {ortho, 0};
            }
            return lineEndpointDistance(q, param);
        }
        return curveDistance(q, param);
    }

    SignedDist lineEndpointDistance(V2 q, double param) const {
        const V2 eq = (param > 0.5 ? p[1] : p[0]) - q;
        return {nonZeroSign(cross(q - p[0], ab)) * length(eq), std::abs(dot(n0, normalize(eq)))};
    }

    SignedDist curveDistance(V2 q, double& param) const {
        // Squared distances until the end; endpoints first.
        const V2 qa = p[0] - q;
        double best = dot(qa, qa);
        double sign = nonZeroSign(cross(d0, qa));
        param = -dot(qa, d0) * inv0;
        {
            const V2 eq = end() - q;
            const double d2 = dot(eq, eq);
            if (d2 < best) {
                best = d2;
                sign = nonZeroSign(cross(d1, eq));
                param = 1 - dot(eq, d1) * inv1;
            }
        }

        if (order == 2) {
            // Stationary points of |B(t) - q|^2: a cubic in t.
            std::array<double, 3> t{};
            const int n = cubicRootsInUnit(t, ca, cb, cc + dot(qa, br), dot(qa, ab));
            for (int i = 0; i < n; ++i) {
                const double ti = t[static_cast<std::size_t>(i)];
                const V2 qe = qa + (2 * ti) * ab + (ti * ti) * br;
                const double d2 = dot(qe, qe);
                if (d2 <= best) {
                    best = d2;
                    sign = nonZeroSign(cross(ab + ti * br, qe));
                    param = ti;
                }
            }
            return endpointResult(sign * std::sqrt(best), param, q);
        }

        // Cubic: Newton iterations from evenly spaced starts (B(t) - q = qa + t (3ab + t (3br + t as))).
        const V2 ab3 = 3 * ab, br3 = 3 * br, br6 = 6 * br, as3 = 3 * as, as6 = 6 * as;
        const auto offset = [&](double t) { return qa + t * (ab3 + t * (br3 + t * as)); };
        const auto tangent = [&](double t) { return ab3 + t * (br6 + t * as3); };
        for (int i = 0; i <= kCubicSearchStarts; ++i) {
            double t = static_cast<double>(i) / kCubicSearchStarts;
            V2 qe = offset(t);
            V2 dd1 = tangent(t);
            V2 dd2 = br6 + t * as6;
            double improved = t - dot(qe, dd1) / (dot(dd1, dd1) + dot(qe, dd2));
            if (improved > 0 && improved < 1) {
                int remaining = kCubicSearchSteps;
                do {
                    t = improved;
                    qe = offset(t);
                    dd1 = tangent(t);
                    if (!--remaining) break;
                    dd2 = br6 + t * as6;
                    improved = t - dot(qe, dd1) / (dot(dd1, dd1) + dot(qe, dd2));
                } while (improved > 0 && improved < 1);
                const double d2 = dot(qe, qe);
                if (d2 < best) {
                    best = d2;
                    sign = nonZeroSign(cross(dd1, qe));
                    param = t;
                }
            }
        }
        return endpointResult(sign * std::sqrt(best), param, q);
    }

    // Replaces the distance by the distance to the tangent line extended beyond the nearer endpoint when closer.
    void toPseudoDistance(SignedDist& sd, V2 q, double param) const {
        if (param < 0) {
            const V2 aq = q - p[0];
            if (dot(aq, n0) < 0) {
                const double pd = cross(aq, n0);
                if (std::abs(pd) <= std::abs(sd.d)) sd = {pd, 0};
            }
        } else if (param > 1) {
            const V2 bq = q - end();
            if (dot(bq, n1) > 0) {
                const double pd = cross(bq, n1);
                if (std::abs(pd) <= std::abs(sd.d)) sd = {pd, 0};
            }
        }
    }
};

// de Casteljau split at t.
void splitEdge(const Edge& e, double t, Edge& a, Edge& b) {
    std::array<V2, 4> q = e.p;
    const auto n = static_cast<std::size_t>(e.order);
    a = e;
    b = e;
    for (std::size_t k = 1; k <= n; ++k) {
        for (std::size_t i = 0; i + k <= n; ++i) q[i] = mix(q[i], q[i + 1], t);
        a.p[k] = q[0];
        b.p[n - k] = q[n - k];
    }
}

struct Contour {
    std::vector<Edge> edges;
    int winding = 0; // +1: distances positive inside, -1: hole orientation
    V2 lo, hi;       // bounds of the edges' control points
    std::uint8_t channels = 0;

    void prepare() {
        lo = hi = edges.front().p[0];
        channels = 0;
        for (Edge& e : edges) {
            e.prepare();
            lo = {std::min(lo.x, e.lo.x), std::min(lo.y, e.lo.y)};
            hi = {std::max(hi.x, e.hi.x), std::max(hi.y, e.hi.y)};
            channels = static_cast<std::uint8_t>(channels | e.color);
        }
    }
};

// Squared distance from q to a box (0 inside).
double boxDistance2(V2 lo, V2 hi, V2 q) {
    const double bx = std::max(std::max(lo.x - q.x, q.x - hi.x), 0.0);
    const double by = std::max(std::max(lo.y - q.y, q.y - hi.y), 0.0);
    return bx * bx + by * by;
}

// ---- outline import ----

bool degenerate(const Edge& e) {
    for (int i = 1; i <= e.order; ++i)
        if (!(e.p[static_cast<std::size_t>(i)] == e.p[0])) return false;
    return true;
}

std::vector<Contour> buildContours(std::span<const OutlineCmd> path, const Affine& m) {
    std::vector<Contour> out;
    const auto map = [&m](float fx, float fy) {
        const double x = static_cast<double>(fx), y = static_cast<double>(fy);
        return V2{m.xx * x + m.xy * y + m.dx, m.yx * x + m.yy * y + m.dy};
    };
    V2 start, cur;
    bool open = false;
    const auto push = [&out](Edge e) {
        if (degenerate(e)) return;
        out.back().edges.push_back(e);
    };
    const auto closeContour = [&] {
        if (open && !(cur == start)) {
            Edge e;
            e.p[0] = cur;
            e.p[1] = start;
            push(e);
        }
        open = false;
    };
    for (const OutlineCmd& c : path) {
        if (c.kind == OutlineCmd::Move) {
            closeContour();
            out.emplace_back();
            start = cur = map(c.x, c.y);
            open = true;
            continue;
        }
        if (!open) { // segment without a preceding move: start at the current point
            out.emplace_back();
            start = cur;
            open = true;
        }
        const V2 to = map(c.x, c.y);
        Edge e;
        e.p[0] = cur;
        e.p[1] = to;
        if (c.kind == OutlineCmd::Quad) {
            const V2 c0 = map(c.cx, c.cy);
            if (!(c0 == cur || c0 == to)) {
                e.order = 2;
                e.p[1] = c0;
                e.p[2] = to;
            }
        } else if (c.kind == OutlineCmd::Cubic) {
            const V2 c0 = map(c.cx, c.cy), c1 = map(c.cx1, c.cy1);
            if (!((c0 == cur || c0 == to) && (c1 == cur || c1 == to))) {
                e.order = 3;
                e.p[1] = c0;
                e.p[2] = c1;
                e.p[3] = to;
            }
        }
        push(e);
        cur = to;
    }
    closeContour();
    std::erase_if(out, [](const Contour& c) { return c.edges.empty(); });
    return out;
}

// Twice the negated signed area (shoelace over sampled points): > 0 when distances come out positive inside.
double orientationArea(const Contour& c) {
    double total = 0;
    const V2 first = c.edges.front().start();
    V2 prev = first;
    for (const Edge& e : c.edges) {
        const int samples = e.order == 1 ? 1 : 8;
        for (int s = 1; s <= samples; ++s) {
            const V2 q = s == samples ? e.end() : e.point(static_cast<double>(s) / samples);
            total += (q.x - prev.x) * (prev.y + q.y);
            prev = q;
        }
    }
    total += (first.x - prev.x) * (prev.y + first.y);
    return total;
}

// ---- edge colouring (msdfgen "simple", angle threshold 3 rad, seed 0) ----

void switchColor(std::uint8_t& color, std::uint64_t& seed) {
    const int shifted = color << (1 + static_cast<int>(seed & 1));
    color = static_cast<std::uint8_t>((shifted | shifted >> 3) & White);
    seed >>= 1;
}

void switchColor(std::uint8_t& color, std::uint64_t& seed, std::uint8_t banned) {
    const auto combined = static_cast<std::uint8_t>(color & banned);
    if (combined == Red || combined == Green || combined == Blue)
        color = static_cast<std::uint8_t>(combined ^ White);
    else
        switchColor(color, seed);
}

bool isCorner(V2 a, V2 b, double crossThreshold) {
    return dot(a, b) <= 0 || std::abs(cross(a, b)) > crossThreshold;
}

int symmetricalTrichotomy(int position, int n) {
    return static_cast<int>(3 + 2.875 * position / (n - 1) - 1.4375 + 0.5) - 3;
}

void colorEdges(std::vector<Contour>& contours) {
    const double crossThreshold = 0.14112000805986721; // sin(3 rad), a literal so no libm is involved
    std::uint64_t seed = 0;
    std::uint8_t color = Cyan;
    std::vector<int> corners;
    for (Contour& contour : contours) {
        std::vector<Edge>& edges = contour.edges;
        corners.clear();
        V2 prevDir = edges.back().direction(1);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            if (isCorner(normalize(prevDir), normalize(edges[i].direction(0)), crossThreshold))
                corners.push_back(static_cast<int>(i));
            prevDir = edges[i].direction(1);
        }

        if (corners.empty()) { // smooth contour
            switchColor(color, seed);
            for (Edge& e : edges) e.color = color;
        } else if (corners.size() == 1) { // teardrop
            std::array<std::uint8_t, 3> colors{};
            switchColor(color, seed);
            colors[0] = color;
            colors[1] = White;
            switchColor(color, seed);
            colors[2] = color;
            const int corner = corners[0];
            const int m = static_cast<int>(edges.size());
            if (m >= 3) {
                for (int i = 0; i < m; ++i)
                    edges[static_cast<std::size_t>((corner + i) % m)].color =
                        colors[static_cast<std::size_t>(1 + symmetricalTrichotomy(i, m))];
            } else {
                // Fewer than three edges for three colours: split each in thirds.
                std::array<Edge, 6> parts{};
                const auto thirds = [&parts](const Edge& e, std::size_t at) {
                    Edge rest;
                    splitEdge(e, 1.0 / 3, parts[at], rest);
                    splitEdge(rest, 0.5, parts[at + 1], parts[at + 2]);
                };
                if (m == 2) {
                    thirds(edges[0], corner == 0 ? 0 : 3);
                    thirds(edges[1], corner == 0 ? 3 : 0);
                    parts[0].color = parts[1].color = colors[0];
                    parts[2].color = parts[3].color = colors[1];
                    parts[4].color = parts[5].color = colors[2];
                    edges.assign(parts.begin(), parts.end());
                } else {
                    thirds(edges[0], 0);
                    for (std::size_t i = 0; i < 3; ++i) parts[i].color = colors[i];
                    edges.assign(parts.begin(), parts.begin() + 3);
                }
            }
        } else { // several corners: switch colour at each, never repeating the first colour on the last stretch
            const int cornerCount = static_cast<int>(corners.size());
            const int m = static_cast<int>(edges.size());
            const int startIndex = corners[0];
            int spline = 0;
            switchColor(color, seed);
            const std::uint8_t initial = color;
            for (int i = 0; i < m; ++i) {
                const int index = (startIndex + i) % m;
                if (spline + 1 < cornerCount && corners[static_cast<std::size_t>(spline + 1)] == index) {
                    ++spline;
                    switchColor(color, seed, spline == cornerCount - 1 ? initial : std::uint8_t{Black});
                }
                edges[static_cast<std::size_t>(index)].color = color;
            }
        }
    }
}

// ---- distance selection ----

struct ChannelSel {
    SignedDist sd;
    const Edge* edge = nullptr;
    double param = 0;
};

// Closest edge per colour channel plus the closest edge overall (true distance / single-channel pseudo-distance).
struct Selector {
    std::array<ChannelSel, 3> ch{};
    ChannelSel any;

    void add(const Edge& e, V2 q) {
        // Skip edges whose bounds are farther than every distance they could replace.
        const double bound = worst(e.color);
        if (boxDistance2(e.lo, e.hi, q) > bound * bound) return;

        double param = 0;
        const SignedDist sd = e.distance(q, param);
        if (closer(sd, any.sd)) any = {sd, &e, param};
        for (std::size_t i = 0; i < 3; ++i)
            if ((e.color & (1u << i)) && closer(sd, ch[i].sd)) ch[i] = {sd, &e, param};
    }

    // Largest current distance among the given channels (an edge or contour farther than this changes nothing).
    double worst(std::uint8_t channels) const {
        const double r = (channels & Red) ? std::abs(ch[0].sd.d) : 0.0;
        const double g = (channels & Green) ? std::abs(ch[1].sd.d) : 0.0;
        const double b = (channels & Blue) ? std::abs(ch[2].sd.d) : 0.0;
        return std::max(std::max(r, g), b);
    }

    void merge(const Selector& o) {
        for (std::size_t i = 0; i < 3; ++i)
            if (closer(o.ch[i].sd, ch[i].sd)) ch[i] = o.ch[i];
        if (closer(o.any.sd, any.sd)) any = o.any;
    }
};

struct MultiDist {
    std::array<double, 3> c{}; // per-channel pseudo-distances
    double a = 0;              // true distance
    double p = 0;              // single-channel pseudo-distance
    double key(bool single) const { return single ? p : median(c[0], c[1], c[2]); }
};

double pseudo(const ChannelSel& s, V2 q) {
    SignedDist sd = s.sd;
    if (s.edge) s.edge->toPseudoDistance(sd, q, s.param);
    return sd.d;
}

MultiDist resolve(const Selector& s, V2 q) {
    MultiDist r;
    for (std::size_t i = 0; i < 3; ++i) r.c[i] = pseudo(s.ch[i], q);
    r.a = s.any.sd.d;
    r.p = pseudo(s.any, q);
    return r;
}

// Distance evaluation over all contours, with per-contour scratch and warm-start hints.
class DistanceFinder {
public:
    explicit DistanceFinder(const std::vector<Contour>& contours)
        : contours_(contours), sels_(contours.size()), dists_(contours.size()), hints_(contours.size()) {}

    // Combines per-contour distances so overlapping contours and holes resolve like the non-zero fill. `single`
    // makes the single-channel pseudo-distance drive the combining (for the error-correction distance check).
    MultiDist distance(V2 q, bool single) {
        const std::size_t n = contours_.size();
        for (std::size_t i = 0; i < n; ++i) scanContour(i, q);
        if (n == 1) return resolve(sels_[0], q);

        Selector shape, inner, outer;
        for (std::size_t i = 0; i < n; ++i) {
            dists_[i] = resolve(sels_[i], q);
            const double ed = dists_[i].key(single);
            shape.merge(sels_[i]);
            if (contours_[i].winding > 0 && ed >= 0) inner.merge(sels_[i]);
            if (contours_[i].winding < 0 && ed <= 0) outer.merge(sels_[i]);
        }
        const MultiDist shapeD = resolve(shape, q), innerD = resolve(inner, q), outerD = resolve(outer, q);
        const double innerM = innerD.key(single), outerM = outerD.key(single);

        MultiDist best;
        int winding = 0;
        if (innerM >= 0 && std::abs(innerM) <= std::abs(outerM)) {
            best = innerD;
            winding = 1;
            for (std::size_t i = 0; i < n; ++i)
                if (contours_[i].winding > 0) {
                    const double cd = dists_[i].key(single);
                    if (std::abs(cd) < std::abs(outerM) && cd > best.key(single)) best = dists_[i];
                }
        } else if (outerM <= 0 && std::abs(outerM) < std::abs(innerM)) {
            best = outerD;
            winding = -1;
            for (std::size_t i = 0; i < n; ++i)
                if (contours_[i].winding < 0) {
                    const double cd = dists_[i].key(single);
                    if (std::abs(cd) < std::abs(innerM) && cd < best.key(single)) best = dists_[i];
                }
        } else {
            return shapeD;
        }
        for (std::size_t i = 0; i < n; ++i)
            if (contours_[i].winding != winding) {
                const double cd = dists_[i].key(single), bd = best.key(single);
                if (cd * bd >= 0 && std::abs(cd) < std::abs(bd)) best = dists_[i];
            }
        if (best.key(single) == shapeD.key(single)) best = shapeD;
        return best;
    }

private:
    // Evaluates the edges that won at the previous query first so the bounds test culls most of the rest.
    void scanContour(std::size_t ci, V2 q) {
        const std::vector<Edge>& edges = contours_[ci].edges;
        Selector& s = sels_[ci];
        std::array<std::size_t, 3>& hint = hints_[ci];
        s = Selector{};
        for (std::size_t k = 0; k < hint.size(); ++k)
            if (hint[k] < edges.size() && std::find(hint.begin(), hint.begin() + static_cast<std::ptrdiff_t>(k),
                                                    hint[k]) == hint.begin() + static_cast<std::ptrdiff_t>(k))
                s.add(edges[hint[k]], q);
        const double bound = s.worst(contours_[ci].channels);
        if (boxDistance2(contours_[ci].lo, contours_[ci].hi, q) <= bound * bound)
            for (std::size_t e = 0; e < edges.size(); ++e)
                if (std::find(hint.begin(), hint.end(), e) == hint.end()) s.add(edges[e], q);
        const auto index = [&edges](const ChannelSel& c) {
            return c.edge ? static_cast<std::size_t>(c.edge - edges.data()) : edges.size();
        };
        hint = {index(s.ch[0]), index(s.ch[1]), index(s.ch[2])};
    }

    const std::vector<Contour>& contours_;
    std::vector<Selector> sels_;
    std::vector<MultiDist> dists_;
    std::vector<std::array<std::size_t, 3>> hints_;
};

// ---- non-zero fill per texel (scanline crossings of y-monotonic pieces) ----

struct Piece {
    const Edge* edge;
    double t0, t1, y0, y1;
};

std::vector<std::uint8_t> fillMask(const std::vector<Contour>& contours, int w, int h) {
    std::vector<Piece> pieces;
    for (const Contour& c : contours)
        for (const Edge& e : c.edges) {
            std::array<double, 4> ts{0, 1, 1, 1};
            std::size_t n = 1;
            std::array<double, 3> roots{};
            int found = 0;
            if (e.order == 2) {
                const double a = e.p[1].y - e.p[0].y, b = e.p[2].y - e.p[1].y;
                found = solveQuadratic(roots, 0, a - b, -a) > 0 ? 1 : 0; // y'(t) = 0 at a / (a - b)
            } else if (e.order == 3) {
                const double a = e.p[1].y - e.p[0].y, b = e.p[2].y - e.p[1].y, c3 = e.p[3].y - e.p[2].y;
                found = std::max(0, solveQuadratic(roots, a - 2 * b + c3, 2 * (b - a), a));
            }
            sortSmall(roots.data(), std::size_t(found));
            for (int i = 0; i < found; ++i) {
                const double t = roots[static_cast<std::size_t>(i)];
                if (t > ts[n - 1] && t < 1) ts[n++] = t;
            }
            ts[n++] = 1;
            for (std::size_t i = 0; i + 1 < n; ++i) {
                const double y0 = e.point(ts[i]).y, y1 = e.point(ts[i + 1]).y;
                if (y0 != y1) pieces.push_back({&e, ts[i], ts[i + 1], y0, y1});
            }
        }

    std::vector<std::uint8_t> fill(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    std::vector<std::pair<double, int>> xs;
    for (int y = 0; y < h; ++y) {
        const double sy = y + 0.5;
        xs.clear();
        for (const Piece& pc : pieces) {
            if (!(sy >= std::min(pc.y0, pc.y1) && sy < std::max(pc.y0, pc.y1))) continue;
            double ta = pc.t0, tb = pc.t1;
            const bool rising = pc.y1 > pc.y0;
            for (int it = 0; it < 60; ++it) { // y is monotonic on the piece
                const double tm = 0.5 * (ta + tb);
                if ((pc.edge->point(tm).y < sy) == rising)
                    ta = tm;
                else
                    tb = tm;
            }
            xs.emplace_back(pc.edge->point(0.5 * (ta + tb)).x, rising ? 1 : -1);
        }
        std::sort(xs.begin(), xs.end());
        int winding = 0;
        std::size_t k = 0;
        for (int x = 0; x < w; ++x) {
            const double sx = x + 0.5;
            while (k < xs.size() && xs[k].first < sx) winding += xs[k++].second;
            fill[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                winding != 0 ? 1 : 0;
        }
    }
    return fill;
}

// ---- error correction (msdfgen's artifact detection with edge / corner protection and distance check) ----

constexpr std::uint8_t kError = 1, kProtected = 2, kSuspect = 4;
constexpr int kCandidate = 1, kArtifact = 2;
constexpr double kArtifactTEpsilon = 0.01;
constexpr double kProtectionRadiusTolerance = 1.001;
constexpr double kMinDeviationRatio = 10.0 / 9.0;
constexpr double kMinImproveRatio = 10.0 / 9.0;

// Normalised distances (d / range + 0.5), RGBA per texel.
struct Field {
    std::vector<double> v;
    int w = 0, h = 0;
    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    }
    double* at(int x, int y) { return v.data() + index(x, y) * 4; }
    const double* at(int x, int y) const { return v.data() + index(x, y) * 4; }

    // Bilinear sample at a pixel-space position (texel centres at +0.5), clamped to the edges.
    std::array<double, 3> sample(V2 pos) const {
        const double px = pos.x - 0.5, py = pos.y - 0.5;
        const double fx = std::floor(px), fy = std::floor(py);
        const double tx = px - fx, ty = py - fy;
        const int l = std::clamp(static_cast<int>(fx), 0, w - 1), r = std::clamp(static_cast<int>(fx) + 1, 0, w - 1);
        const int b = std::clamp(static_cast<int>(fy), 0, h - 1), t = std::clamp(static_cast<int>(fy) + 1, 0, h - 1);
        std::array<double, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            const double lo = at(l, b)[i] + tx * (at(r, b)[i] - at(l, b)[i]);
            const double hi = at(l, t)[i] + tx * (at(r, t)[i] - at(l, t)[i]);
            out[i] = lo + ty * (hi - lo);
        }
        return out;
    }
};

double med3(const double* c) { return median(c[0], c[1], c[2]); }

double interpolatedMedian(const double* a, const double* b, double t) {
    return median(a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2]));
}
double interpolatedMedian(const double* a, const double* l, const double* q, double t) {
    return median(t * (t * q[0] + l[0]) + a[0], t * (t * q[1] + l[1]) + a[1], t * (t * q[2] + l[2]) + a[2]);
}

// Context for checking artifact candidates against the exact shape distance.
struct ShapeCheck {
    const Field* field = nullptr;
    DistanceFinder* finder = nullptr;
    double rangePx = 1;
    V2 texel;                   // centre of the texel under test
    const double* msd = nullptr; // its channels
};

// Decides whether a median interpolated between two texels is an artifact.
struct Classifier {
    double span;
    bool isProtected;
    const ShapeCheck* check; // null: decide on the value ranges alone
    V2 dir;                  // towards the neighbour (texels)
    bool* suspect;           // set when any candidate is seen

    int rangeTest(double at, double bt, double xt, double am, double bm, double xm) const {
        // Protected texels only count inversions; others any median outside its endpoints.
        if ((am > 0.5 && bm > 0.5 && xm <= 0.5) || (am < 0.5 && bm < 0.5 && xm >= 0.5) ||
            (!isProtected && median(am, bm, xm) != xm)) {
            const double axSpan = (xt - at) * span, bxSpan = (bt - xt) * span;
            if (!(xm >= am - axSpan && xm <= am + axSpan && xm >= bm - bxSpan && xm <= bm + bxSpan))
                return kCandidate | kArtifact;
            return kCandidate;
        }
        return 0;
    }

    bool evaluate(double t, int flags) const {
        if (flags & kCandidate) *suspect = true;
        if (!check) return (flags & kArtifact) != 0;
        if (!(flags & kCandidate)) return false;
        if (flags & kArtifact) return true;
        // Is the exact distance there matched better once this texel is flattened to its median?
        const V2 tv = t * dir;
        const V2 pos = check->texel + tv;
        const std::array<double, 3> before = check->field->sample(pos);
        const double weight = (1 - std::abs(tv.x)) * (1 - std::abs(tv.y));
        const double m = med3(check->msd);
        std::array<double, 3> after{};
        for (std::size_t i = 0; i < 3; ++i) after[i] = before[i] + weight * (m - check->msd[i]);
        const double oldD = med3(before.data()), newD = med3(after.data());
        const double ref = check->finder->distance(pos, true).p / check->rangePx + 0.5;
        return kMinImproveRatio * std::abs(newD - ref) < std::abs(oldD - ref);
    }
};

bool linearArtifactAt(const Classifier& cls, double am, double bm, const double* a, const double* b, double dA,
                      double dB) {
    // Where the two channels cross.
    const double t = dA / (dA - dB);
    if (t > kArtifactTEpsilon && t < 1 - kArtifactTEpsilon) {
        const double xm = interpolatedMedian(a, b, t);
        return cls.evaluate(t, cls.rangeTest(0, 1, t, am, bm, xm));
    }
    return false;
}

bool hasLinearArtifact(const Classifier& cls, double am, const double* a, const double* b) {
    const double bm = med3(b);
    // Of the pair, only the texel farther from the edge is reported.
    return std::abs(am - 0.5) >= std::abs(bm - 0.5) &&
           (linearArtifactAt(cls, am, bm, a, b, a[1] - a[0], b[1] - b[0]) ||
            linearArtifactAt(cls, am, bm, a, b, a[2] - a[1], b[2] - b[1]) ||
            linearArtifactAt(cls, am, bm, a, b, a[0] - a[2], b[0] - b[2]));
}

bool diagonalArtifactAt(const Classifier& cls, double am, double dm, const double* a, const double* l, const double* q,
                        double dA, double dBC, double dD, double tEx0, double tEx1) {
    std::array<double, 3> t{};
    const int n = solveQuadratic(t, dD - dBC + dA, dBC - dA - dA, dA);
    for (int i = 0; i < n; ++i) {
        const double ti = t[static_cast<std::size_t>(i)];
        if (!(ti > kArtifactTEpsilon && ti < 1 - kArtifactTEpsilon)) continue;
        const double xm = interpolatedMedian(a, l, q, ti);
        int flags = cls.rangeTest(0, 1, ti, am, dm, xm);
        // Also test against the medians at the channels' local extremes.
        for (const double tEx : {tEx0, tEx1}) {
            if (!(tEx > 0 && tEx < 1)) continue;
            std::array<double, 2> tEnd{0, 1};
            std::array<double, 2> em{am, dm};
            const std::size_t side = tEx > ti ? 1 : 0;
            tEnd[side] = tEx;
            em[side] = interpolatedMedian(a, l, q, tEx);
            flags |= cls.rangeTest(tEnd[0], tEnd[1], ti, em[0], em[1], xm);
        }
        if (cls.evaluate(ti, flags)) return true;
    }
    return false;
}

// a: this texel, b / c: the side neighbours, d: the diagonal neighbour (bilinear along the diagonal).
bool hasDiagonalArtifact(const Classifier& cls, double am, const double* a, const double* b, const double* c,
                         const double* d) {
    const double dm = med3(d);
    if (std::abs(am - 0.5) < std::abs(dm - 0.5)) return false;
    // Channels i and j can only cross if their difference changes sign somewhere among the four texels
    // (the diagonal interpolation is a non-negative blend of them).
    const auto mayCross = [&](std::size_t i, std::size_t j) {
        const double da = a[j] - a[i], db = b[j] - b[i], dc = c[j] - c[i], dd = d[j] - d[i];
        return !((da > 0 && db > 0 && dc > 0 && dd > 0) || (da < 0 && db < 0 && dc < 0 && dd < 0));
    };
    const bool c01 = mayCross(0, 1), c12 = mayCross(1, 2), c20 = mayCross(2, 0);
    if (!c01 && !c12 && !c20) return false;
    std::array<double, 3> l{}, q{}, tEx{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double abc = a[i] - b[i] - c[i];
        l[i] = -a[i] - abc;
        q[i] = d[i] + abc;
        tEx[i] = -0.5 * l[i] / q[i];
    }
    return (c01 && diagonalArtifactAt(cls, am, dm, a, l.data(), q.data(), a[1] - a[0], b[1] - b[0] + c[1] - c[0],
                                      d[1] - d[0], tEx[0], tEx[1])) ||
           (c12 && diagonalArtifactAt(cls, am, dm, a, l.data(), q.data(), a[2] - a[1], b[2] - b[1] + c[2] - c[1],
                                      d[2] - d[1], tEx[1], tEx[2])) ||
           (c20 && diagonalArtifactAt(cls, am, dm, a, l.data(), q.data(), a[0] - a[2], b[0] - b[2] + c[0] - c[2],
                                      d[0] - d[2], tEx[2], tEx[0]));
}

// Flags texels whose interpolation with a neighbour produces an artifact. With `check`, only texels not yet
// flagged but suspect are tested and candidates are confirmed against the exact distance.
void findErrors(const Field& f, std::vector<std::uint8_t>& stencil, double rangePx, ShapeCheck* check) {
    const double hSpan = kMinDeviationRatio / rangePx;
    const double dSpan = kMinDeviationRatio * std::numbers::sqrt2 / rangePx;
    // Channel order per texel (8: some channels equal). Interpolating texels that all share one strict order
    // never makes two channels cross, so such neighbourhoods cannot hold an artifact.
    std::vector<std::uint8_t> order(stencil.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        const double* c = f.v.data() + i * 4;
        order[i] = (c[0] == c[1] || c[1] == c[2] || c[2] == c[0])
                       ? std::uint8_t{8}
                       : static_cast<std::uint8_t>((c[1] > c[0] ? 1 : 0) | (c[2] > c[1] ? 2 : 0) | (c[0] > c[2] ? 4 : 0));
    }
    const auto uniform = [&](int x, int y) {
        const std::uint8_t o = order[f.index(x, y)];
        if (o == 8) return false;
        for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, f.h - 1); ++ny)
            for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, f.w - 1); ++nx)
                if (order[f.index(nx, ny)] != o) return false;
        return true;
    };
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            std::uint8_t& st = stencil[f.index(x, y)];
            if (check && ((st & kError) || !(st & kSuspect))) continue;
            if (uniform(x, y)) continue;
            const double* c = f.at(x, y);
            const double cm = med3(c);
            const bool prot = (st & kProtected) != 0;
            if (check) {
                check->texel = {x + 0.5, y + 0.5};
                check->msd = c;
            }
            bool suspect = false;
            const auto cls = [&](double dx, double dy, double span) {
                return Classifier{span, prot, check, {dx, dy}, &suspect};
            };
            const double* l = x > 0 ? f.at(x - 1, y) : nullptr;
            const double* b = y > 0 ? f.at(x, y - 1) : nullptr;
            const double* r = x < f.w - 1 ? f.at(x + 1, y) : nullptr;
            const double* t = y < f.h - 1 ? f.at(x, y + 1) : nullptr;
            const bool error =
                (l && hasLinearArtifact(cls(-1, 0, hSpan), cm, c, l)) ||
                (b && hasLinearArtifact(cls(0, -1, hSpan), cm, c, b)) ||
                (r && hasLinearArtifact(cls(1, 0, hSpan), cm, c, r)) ||
                (t && hasLinearArtifact(cls(0, 1, hSpan), cm, c, t)) ||
                (l && b && hasDiagonalArtifact(cls(-1, -1, dSpan), cm, c, l, b, f.at(x - 1, y - 1))) ||
                (r && b && hasDiagonalArtifact(cls(1, -1, dSpan), cm, c, r, b, f.at(x + 1, y - 1))) ||
                (l && t && hasDiagonalArtifact(cls(-1, 1, dSpan), cm, c, l, t, f.at(x - 1, y + 1))) ||
                (r && t && hasDiagonalArtifact(cls(1, 1, dSpan), cm, c, r, t, f.at(x + 1, y + 1)));
            if (error) st |= kError;
            if (suspect) st |= kSuspect;
        }
}

// Protects the four texels around every corner whose edges share at most one channel.
void protectCorners(const std::vector<Contour>& contours, std::vector<std::uint8_t>& stencil, const Field& f) {
    for (const Contour& c : contours) {
        const Edge* prev = &c.edges.back();
        for (const Edge& e : c.edges) {
            const int common = prev->color & e.color;
            prev = &e;
            if (common & (common - 1)) continue;
            const V2 p = e.start();
            const double fl = std::floor(p.x - 0.5), fb = std::floor(p.y - 0.5);
            if (fl < -1 || fb < -1 || fl >= f.w || fb >= f.h) continue;
            const int l = static_cast<int>(fl), b = static_cast<int>(fb);
            for (const int ty : {b, b + 1})
                for (const int tx : {l, l + 1})
                    if (tx >= 0 && ty >= 0 && tx < f.w && ty < f.h) stencil[f.index(tx, ty)] |= kProtected;
        }
    }
}

// True when channel `ch` crosses the edge between a and b while being the median there.
bool edgeBetween(const double* a, const double* b, std::size_t ch) {
    const double t = (a[ch] - 0.5) / (a[ch] - b[ch]);
    if (t > 0 && t < 1) {
        const std::array<double, 3> c{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])};
        return median(c[0], c[1], c[2]) == c[ch];
    }
    return false;
}

// Protects texel pairs straddling an edge whose edge-defining channels differ from the median.
void protectEdges(const Field& f, std::vector<std::uint8_t>& stencil, double rangePx) {
    const auto pair = [&](int ax, int ay, int bx, int by, double radius) {
        const double* a = f.at(ax, ay);
        const double* b = f.at(bx, by);
        const double am = med3(a), bm = med3(b);
        if (std::abs(am - 0.5) + std::abs(bm - 0.5) >= radius) return;
        std::array<bool, 3> mask{};
        for (std::size_t i = 0; i < 3; ++i) mask[i] = edgeBetween(a, b, i);
        const auto protect = [&](const double* msd, double m, std::uint8_t& st) {
            for (std::size_t i = 0; i < 3; ++i)
                if (mask[i] && msd[i] != m) st |= kProtected;
        };
        protect(a, am, stencil[f.index(ax, ay)]);
        protect(b, bm, stencil[f.index(bx, by)]);
    };
    const double radius = kProtectionRadiusTolerance / rangePx;
    const double diagRadius = kProtectionRadiusTolerance * std::numbers::sqrt2 / rangePx;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x + 1 < f.w; ++x) pair(x, y, x + 1, y, radius);
    for (int y = 0; y + 1 < f.h; ++y)
        for (int x = 0; x < f.w; ++x) pair(x, y, x, y + 1, radius);
    for (int y = 0; y + 1 < f.h; ++y)
        for (int x = 0; x + 1 < f.w; ++x) {
            pair(x, y, x + 1, y + 1, diagRadius);
            pair(x + 1, y, x, y + 1, diagRadius);
        }
}

void errorCorrection(Field& f, const std::vector<Contour>& contours, DistanceFinder& finder, double rangePx) {
    std::vector<std::uint8_t> stencil(f.v.size() / 4, 0);
    protectCorners(contours, stencil, f);
    protectEdges(f, stencil, rangePx);
    findErrors(f, stencil, rangePx, nullptr);
    // Remaining texels: only inversions confirmed by the exact distance.
    for (std::uint8_t& s : stencil) s |= kProtected;
    ShapeCheck check{&f, &finder, rangePx, {}, nullptr};
    findErrors(f, stencil, rangePx, &check);
    for (std::size_t i = 0; i < stencil.size(); ++i)
        if (stencil[i] & kError) {
            double* px = f.v.data() + i * 4;
            px[0] = px[1] = px[2] = med3(px);
        }
}

std::uint8_t encode(double v) { // v: normalised distance, 0.5 on the edge
    const double s = v * 255.0;
    if (!(s > 0)) return 0; // also NaN
    if (s >= 255) return 255;
    return static_cast<std::uint8_t>(std::floor(s + 0.5));
}

} // namespace

void generateMsdf(std::span<const OutlineCmd> path, const Affine& toPixels, float range, std::uint8_t* rgba, int w,
                  int h, int stride) {
    if (!rgba || w <= 0 || h <= 0) return;
    const double rangePx = range > 0.f ? static_cast<double>(range) : 1.0;

    std::vector<Contour> contours = buildContours(path, toPixels);
    if (contours.empty()) {
        for (int y = 0; y < h; ++y)
            std::fill_n(rgba + static_cast<std::ptrdiff_t>(y) * stride, static_cast<std::size_t>(w) * 4, std::uint8_t{0});
        return;
    }

    // Orient so the dominant (outer) contours give positive distances inside: covers the y flip of toPixels and
    // both the TrueType and the CFF convention.
    double total = 0;
    std::vector<double> areas(contours.size());
    for (std::size_t i = 0; i < contours.size(); ++i) total += areas[i] = orientationArea(contours[i]);
    for (std::size_t i = 0; i < contours.size(); ++i) {
        if (total < 0) {
            areas[i] = -areas[i];
            std::reverse(contours[i].edges.begin(), contours[i].edges.end());
            for (Edge& e : contours[i].edges) e.reverse();
        }
        contours[i].winding = areas[i] > 0 ? 1 : areas[i] < 0 ? -1 : 0;
    }
    colorEdges(contours);
    for (Contour& c : contours) c.prepare();

    Field f{std::vector<double>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4), w, h};
    DistanceFinder finder(contours);
    const double limit = 1e6; // keeps channels no edge feeds finite
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const MultiDist d = finder.distance(V2{x + 0.5, y + 0.5}, false);
            double* px = f.at(x, y);
            for (std::size_t i = 0; i < 3; ++i) px[i] = std::clamp(d.c[i], -limit, limit) / rangePx + 0.5;
            px[3] = std::clamp(d.a, -limit, limit) / rangePx + 0.5;
        }

    // Sign correction: the median and the true distance must agree with the non-zero fill.
    const std::vector<std::uint8_t> fill = fillMask(contours, w, h);
    std::vector<signed char> match(fill.size(), 0);
    for (std::size_t i = 0; i < fill.size(); ++i) {
        double* px = f.v.data() + i * 4;
        const bool inside = fill[i] != 0;
        const double sd = med3(px);
        if (sd != 0.5) {
            match[i] = (sd > 0.5) == inside ? 1 : -1;
            if (match[i] < 0)
                for (std::size_t c = 0; c < 3; ++c) px[c] = 1 - px[c];
        }
        if (px[3] != 0.5 && (px[3] > 0.5) != inside) px[3] = 1 - px[3];
    }
    // Texels exactly on the edge follow their neighbours.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = f.index(x, y);
            if (match[i] != 0) continue;
            int neighbours = 0;
            if (x > 0) neighbours += match[i - 1];
            if (x < w - 1) neighbours += match[i + 1];
            if (y > 0) neighbours += match[i - static_cast<std::size_t>(w)];
            if (y < h - 1) neighbours += match[i + static_cast<std::size_t>(w)];
            if (neighbours < 0)
                for (std::size_t c = 0; c < 3; ++c) f.v[i * 4 + c] = 1 - f.v[i * 4 + c];
        }

    errorCorrection(f, contours, finder, rangePx);

    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = rgba + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const double* px = f.at(x, y);
            for (std::size_t c = 0; c < 4; ++c) row[static_cast<std::size_t>(x) * 4 + c] = encode(px[c]);
        }
    }
}

} // namespace fuse::text::detail
