/* FUSE Relight test-app kit (RL-0.4): see rl_dump.h. */
#include "rl_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------ */
static const uint32_t rl_sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define RL_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void rl_sha256_block(uint32_t h[8], const uint8_t* p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = RL_ROTR(w[i - 15], 7) ^ RL_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = RL_ROTR(w[i - 2], 17) ^ RL_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; ++i) {
        uint32_t s1 = RL_ROTR(e, 6) ^ RL_ROTR(e, 11) ^ RL_ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + rl_sha_k[i] + w[i];
        uint32_t s0 = RL_ROTR(a, 2) ^ RL_ROTR(a, 13) ^ RL_ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void rl_sha256_hex(const void* data, size_t size, char out_hex[65])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const uint8_t* p = (const uint8_t*)data;
    uint8_t tail[128];
    size_t full = size / 64, rem = size % 64, tail_len, i;
    uint64_t bits = (uint64_t)size * 8u;
    for (i = 0; i < full; ++i)
        rl_sha256_block(h, p + 64 * i);
    memset(tail, 0, sizeof tail);
    if (rem)
        memcpy(tail, p + 64 * full, rem);
    tail[rem] = 0x80;
    tail_len = (rem + 1 + 8 <= 64) ? 64 : 128;
    for (i = 0; i < 8; ++i)
        tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    rl_sha256_block(h, tail);
    if (tail_len == 128)
        rl_sha256_block(h, tail + 64);
    for (i = 0; i < 32; ++i) {
        uint8_t byte = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        out_hex[2 * i] = hex[byte >> 4];
        out_hex[2 * i + 1] = hex[byte & 15];
    }
    out_hex[64] = 0;
}

/* ---- base64 --------------------------------------------------------------------------------- */
size_t rl_base64_size(size_t size) { return ((size + 2) / 3) * 4 + 1; }

size_t rl_base64_encode(const void* data, size_t size, char* out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t* p = (const uint8_t*)data;
    size_t i, o = 0;
    for (i = 0; i + 2 < size; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63]; out[o++] = tbl[v & 63];
    }
    if (size - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63]; out[o++] = '='; out[o++] = '=';
    } else if (size - i == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63]; out[o++] = tbl[(v >> 6) & 63]; out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* ---- checksums ------------------------------------------------------------------------------ */
uint32_t rl_crc32(uint32_t crc, const void* data, size_t size)
{
    static uint32_t table[256];
    static int init = 0;
    const uint8_t* p = (const uint8_t*)data;
    size_t i;
    if (!init) {
        uint32_t n, k;
        for (n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        init = 1;
    }
    crc = ~crc;
    for (i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

uint32_t rl_adler32(uint32_t adler, const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t a = adler & 0xffff, b = adler >> 16;
    size_t i;
    for (i = 0; i < size; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ---- files ---------------------------------------------------------------------------------- */
int rl_write_file(const char* path, const void* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;
    if (size && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void rl_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int rl_png_chunk(FILE* f, const char type[4], const uint8_t* data, uint32_t len)
{
    uint8_t hdr[8], crcb[4];
    uint32_t crc;
    rl_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    crc = rl_crc32(0, hdr + 4, 4);
    if (len)
        crc = rl_crc32(crc, data, len);
    rl_be32(crcb, crc);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    return fwrite(crcb, 1, 4, f) == 4 ? 0 : -1;
}

int rl_write_png_rgba8(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    size_t row = (size_t)width * 4u + 1u, raw_size = row * height, nblocks, zsize, i, o;
    uint8_t ihdr[13], *raw, *z;
    uint32_t adler;
    FILE* f;
    int rc = 0;

    raw = (uint8_t*)malloc(raw_size ? raw_size : 1);
    if (!raw) return -1;
    for (i = 0; i < height; ++i) {
        raw[i * row] = 0;
        memcpy(raw + i * row + 1, rgba + i * (size_t)width * 4u, (size_t)width * 4u);
    }
    nblocks = raw_size ? (raw_size + 65534) / 65535 : 1;
    zsize = 2 + nblocks * 5 + raw_size + 4;
    z = (uint8_t*)malloc(zsize);
    if (!z) { free(raw); return -1; }
    o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    for (i = 0; i < nblocks; ++i) {
        size_t off = i * 65535, len = raw_size - off > 65535 ? 65535 : raw_size - off;
        z[o++] = (uint8_t)(i + 1 == nblocks ? 1 : 0);
        z[o++] = (uint8_t)(len & 0xff); z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)(~len & 0xff); z[o++] = (uint8_t)((~len >> 8) & 0xff);
        memcpy(z + o, raw + off, len);
        o += len;
    }
    adler = rl_adler32(1, raw, raw_size);
    rl_be32(z + o, adler);
    o += 4;

    rl_be32(ihdr, width);
    rl_be32(ihdr + 4, height);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

    f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    if (fwrite(sig, 1, 8, f) != 8) rc = -1;
    if (!rc) rc = rl_png_chunk(f, "IHDR", ihdr, 13);
    if (!rc) rc = rl_png_chunk(f, "IDAT", z, (uint32_t)o);
    if (!rc) rc = rl_png_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) rc = -1;
    free(raw);
    free(z);
    return rc;
}
