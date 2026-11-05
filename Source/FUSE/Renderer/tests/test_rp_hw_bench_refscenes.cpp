// Renderer plan §8 reference scenes (Sponza, Bistro): the test-time side of the fetch harness
// (docs/unification/RENDERER-HW-RUN.md, "Reference scenes"). The test never downloads anything.
//
//   manifest   tests/perf/reference_scenes.manifest: name | url | archive | sha256 pin | marker file | licence
//   fetch      cmake -DDEST=<asset root> [-DSCENE=...] -P Source/FUSE/Renderer/cmake/rp_hw_fetch_reference_scenes.cmake
//   root       FUSE_REFSCENE_ROOT (environment) or the configure-time FUSE_RP_HW_ASSET_ROOT
//
// --scene <name>: 77 (skip) when the archive is absent, or when the manifest has no sha256 pin yet (the hash of a
// present archive is printed so it can be pinned); 1 on a sha256 mismatch or a missing marker after extraction; else
// the OBJ statistics of the extracted scene (vertices, triangles after fan triangulation, materials, objects), 0.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kSkip = 77;

// --- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------------
class Sha256 {
public:
    void update(const unsigned char* p, size_t n) {
        m_bits += static_cast<uint64_t>(n) * 8u;
        while (n > 0) {
            const size_t take = std::min(n, size_t{64} - m_len);
            std::memcpy(m_buf + m_len, p, take);
            m_len += take;
            p += take;
            n -= take;
            if (m_len == 64) {
                block(m_buf);
                m_len = 0;
            }
        }
    }
    std::string hex() {
        const uint64_t bits = m_bits;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while (m_len != 56) {
            update(&zero, 1);
        }
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        }
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) {
            std::snprintf(out + i * 8, 9, "%08x", m_h[i]);
        }
        return std::string(out, 64);
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* b) {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t{b[i * 4]} << 24) | (uint32_t{b[i * 4 + 1]} << 16) | (uint32_t{b[i * 4 + 2]} << 8) | uint32_t{b[i * 4 + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = m_h[0], bb = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        m_h[0] += a;
        m_h[1] += bb;
        m_h[2] += c;
        m_h[3] += d;
        m_h[4] += e;
        m_h[5] += f;
        m_h[6] += g;
        m_h[7] += h;
    }
    uint32_t m_h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char m_buf[64] = {};
    size_t m_len = 0;
    uint64_t m_bits = 0;
};

bool fileExists(const std::string& p) { return static_cast<bool>(std::ifstream(p, std::ios::binary)); }

bool sha256File(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    Sha256 h;
    std::vector<unsigned char> buf(1u << 20);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        h.update(buf.data(), static_cast<size_t>(f.gcount()));
    }
    out = h.hex();
    return true;
}

bool selfTest() {
    Sha256 h;
    const char* abc = "abc";
    h.update(reinterpret_cast<const unsigned char*>(abc), 3);
    return h.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

struct Entry {
    std::string name, url, archive, sha256, marker, licence;
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string scene = "sponza";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--scene") == 0) {
            scene = argv[i + 1];
        }
    }
    if (!selfTest()) {
        std::fprintf(stderr, "FAIL: SHA-256 self-test\n");
        return 1;
    }
    std::ifstream mf(FUSE_RP_HW_MANIFEST);
    if (!mf) {
        std::fprintf(stderr, "FAIL: manifest %s missing\n", FUSE_RP_HW_MANIFEST);
        return 1;
    }
    Entry e{};
    for (std::string line; std::getline(mf, line);) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::vector<std::string> f = split(line, '|');
        if (f.size() >= 6 && f[0] == scene) {
            e = Entry{f[0], f[1], f[2], f[3], f[4], f[5]};
        }
    }
    if (e.name.empty()) {
        std::fprintf(stderr, "FAIL: scene %s not in the manifest\n", scene.c_str());
        return 1;
    }
    const char* envRoot = std::getenv("FUSE_REFSCENE_ROOT");
    const std::string root = envRoot != nullptr && envRoot[0] != '\0' ? envRoot : FUSE_RP_HW_ASSET_ROOT;
    const std::string archive = root + "/" + e.archive;
    const std::string marker = root + "/" + e.name + "/" + e.marker;
    std::printf("%s: %s (%s)\n  archive %s\n", e.name.c_str(), e.url.c_str(), e.licence.c_str(), archive.c_str());
    if (!fileExists(archive)) {
        std::printf("SKIP: asset absent. Fetch (downloads ~%s; needs the user's permission):\n"
                    "  cmake -DDEST=\"%s\" -DSCENE=%s -P Source/FUSE/Renderer/cmake/rp_hw_fetch_reference_scenes.cmake\n",
                    e.name == "bistro" ? "1 GB" : "100 MB", root.c_str(), e.name.c_str());
        return kSkip;
    }
    std::string have;
    if (!sha256File(archive, have)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", archive.c_str());
        return 1;
    }
    if (e.sha256.empty()) {
        std::printf("SKIP: the manifest has no sha256 pin for %s; the present archive hashes to %s\n"
                    "  (review it, then commit that value to tests/perf/reference_scenes.manifest)\n",
                    e.name.c_str(), have.c_str());
        return kSkip;
    }
    if (have != e.sha256) {
        std::fprintf(stderr, "FAIL: sha256 mismatch: %s (pinned %s)\n", have.c_str(), e.sha256.c_str());
        return 1;
    }
    std::ifstream obj(marker);
    if (!obj) {
        std::fprintf(stderr, "FAIL: %s missing (run the fetch script to extract)\n", marker.c_str());
        return 1;
    }
    unsigned long long vertices = 0, triangles = 0, materials = 0, objects = 0;
    for (std::string line; std::getline(obj, line);) {
        if (line.rfind("v ", 0) == 0) {
            ++vertices;
        } else if (line.rfind("f ", 0) == 0) {
            std::istringstream ss(line.substr(2));
            unsigned long long n = 0;
            for (std::string tok; ss >> tok;) {
                ++n;
            }
            triangles += n >= 3 ? n - 2 : 0;
        } else if (line.rfind("usemtl ", 0) == 0) {
            ++materials;
        } else if (line.rfind("o ", 0) == 0 || line.rfind("g ", 0) == 0) {
            ++objects;
        }
    }
    std::printf("%s: sha256 ok; %s: %llu vertices, %llu triangles, %llu material switches, %llu objects / groups\n", e.name.c_str(),
                e.marker.c_str(), vertices, triangles, materials, objects);
    if (triangles == 0) {
        std::fprintf(stderr, "FAIL: no triangles parsed\n");
        return 1;
    }
    return 0;
}
