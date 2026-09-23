/* FUSE Relight test-app kit (RL-0.4): byte-level helpers shared by every app.
 * SHA-256, base64, CRC-32/Adler-32 and an uncompressed (stored-deflate) RGBA8 PNG writer.
 * Pure C99, no third-party code, deterministic output (no timestamps, no compression level
 * choices), so two runs that render the same pixels write byte-identical files. */
#ifndef RL_DUMP_H
#define RL_DUMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-256 of `size` bytes as 64 lowercase hex characters plus NUL. */
void rl_sha256_hex(const void* data, size_t size, char out_hex[65]);

/* Base64 (RFC 4648, with padding). Returns the number of characters written (excluding NUL).
 * `out` must hold rl_base64_size(size) bytes. */
size_t rl_base64_size(size_t size);
size_t rl_base64_encode(const void* data, size_t size, char* out);

uint32_t rl_crc32(uint32_t crc, const void* data, size_t size);
uint32_t rl_adler32(uint32_t adler, const void* data, size_t size);

/* Writes `size` bytes to `path`. Returns 0 on success. */
int rl_write_file(const char* path, const void* data, size_t size);

/* Writes a width x height RGBA8 image (rows top to bottom, 4 bytes per pixel) as PNG
 * (colour type 6, bit depth 8, filter 0 on every row, stored deflate blocks). Returns 0 on success. */
int rl_write_png_rgba8(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif
