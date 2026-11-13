// FUSE UI GPU backend: clip-path coverage shared by ui.draw_list and ui.backdrop. Needs edgeWord(uint) (a float
// word of the ui_images buffer) declared before inclusion.
// DrawOp::ClipPath coverage: same 4x4 winding loop as pathFillCoverage() in Core/src/draw_list.cpp
float windCoverage(uint base, uint n, bool evenOdd, float px, float py) {
    int wind[16];
    for (int k = 0; k < 16; ++k) wind[k] = 0;
    for (uint i = 0u; i < n; ++i) {
        const uint o = base + i * 5u;
        const float ax = edgeWord(o), ay = edgeWord(o + 1u), bx = edgeWord(o + 2u), by = edgeWord(o + 3u);
        if (ay == by) continue;
        precise float vx = bx - ax;
        precise float vy = by - ay;
        const int dir = by > ay ? 1 : -1;
        const float lo = min(ay, by), hi = max(ay, by);
        for (int j = 0; j < 4; ++j) {
            precise float sy = py - 0.5 + (float(j) + 0.5) * 0.25;
            if (sy < lo || sy >= hi) continue;
            precise float xi = ax + (sy - ay) * vx / vy;
            for (int k = 0; k < 4; ++k) {
                precise float sx = px - 0.5 + (float(k) + 0.5) * 0.25;
                if (xi > sx) wind[j * 4 + k] += dir;
            }
        }
    }
    int inCount = 0;
    for (int k = 0; k < 16; ++k) inCount += evenOdd ? (wind[k] & 1) : (wind[k] != 0 ? 1 : 0);
    return float(inCount) / 16.0;
}

// a ClipPath entry: a plain path, or layered clip data (header edge with fillOnly < 0), as clipEntryCoverage()
float clipPathCoverage(uint base, uint n, bool evenOdd, float px, float py) {
    if (n == 0u || edgeWord(base + 4u) >= 0.0) return windCoverage(base, n, evenOdd, px, py);
    precise float m = 0.0;
    for (uint i = 0u; i < n;) {
        const uint h = base + i * 5u;
        const int g = int(-1.0 - edgeWord(h + 4u));
        const uint cnt = uint(edgeWord(h + 2u));
        uint at = i + 1u + (g != 0 ? 2u : 0u);
        const uint tw = g == 3 ? uint(edgeWord(h + 12u)) : 0u, th = g == 3 ? uint(edgeWord(h + 13u)) : 0u;
        const uint data = at;
        if (g == 3) at += (tw * th + 3u) / 4u;
        if (at + cnt > n) break;
        const float cov = windCoverage(base + at * 5u, cnt, edgeWord(h + 1u) != 0.0, px, py);
        if (cov > 0.0) {
            precise float v = edgeWord(h);
            precise float a = edgeWord(h + 3u);
            if (g == 3) { // mask texture: bilinear, clamped (as clipEntryCoverage)
                const uint p0 = h + 5u, p1 = h + 10u;
                precise float ux = edgeWord(p0) * px + edgeWord(p0 + 2u) * py + edgeWord(p1);
                precise float uy = edgeWord(p0 + 1u) * px + edgeWord(p0 + 3u) * py + edgeWord(p1 + 1u);
                precise float fx = ux - 0.5;
                precise float fy = uy - 0.5;
                const float x0 = floor(fx), y0 = floor(fy);
                precise float tx = fx - x0;
                precise float ty = fy - y0;
                const int ix = int(x0), iy = int(y0);
                float t4[4];
                for (int q = 0; q < 4; ++q) {
                    const int xx = clamp(ix + (q & 1), 0, int(tw) - 1), yy = clamp(iy + (q >> 1), 0, int(th) - 1);
                    const uint k = uint(yy) * tw + uint(xx);
                    const uint w = base + (data + k / 4u) * 5u + (k % 4u);
                    t4[q] = edgeWord(w);
                }
                precise float top = t4[0] + (t4[1] - t4[0]) * tx;
                precise float bot = t4[2] + (t4[3] - t4[2]) * tx;
                v = top + (bot - top) * ty;
                a = v;
            } else if (g != 0) {
                const uint p0 = h + 5u, p1 = h + 10u;
                precise float ux = edgeWord(p0) * px + edgeWord(p0 + 2u) * py + edgeWord(p1);
                precise float uy = edgeWord(p0 + 1u) * px + edgeWord(p0 + 3u) * py + edgeWord(p1 + 1u);
                const float t = clamp(g == 1 ? ux : sqrt(ux * ux + uy * uy), 0.0, 1.0);
                v = v + (edgeWord(p1 + 2u) - v) * t;
                a = a + (edgeWord(p1 + 3u) - a) * t;
            }
            m = v * cov + m * (1.0 - a * cov);
        }
        i = at + cnt;
    }
    return clamp(m, 0.0, 1.0);
}

