/* FUSE Relight RL-6.2: the ABI gate in C (both public headers compile as C99/C11 and the layouts seen from C match the
 * Remix API 0.6.5 reference, remixapi_layout_ref.inc). Called by test_api_layout.cpp. */
#include <fuse/relight/api/fuse_relight_api.h>
#include <fuse/relight/api/remixapi_compat.h>

#include <stddef.h>
#include <stdio.h>

int rl_api_layout_check_c(void) {
    int failures = 0;
    const int is64 = sizeof(void*) == 8;
#define RL_STRUCT(T, s64, a64, s32, a32)                                                                              \
    if (sizeof(T) != (size_t)(is64 ? (s64) : (s32))) {                                                              \
        printf("FAIL (C): sizeof(%s) = %u, reference %u\n", #T, (unsigned)sizeof(T), (unsigned)(is64 ? (s64) : (s32))); \
        ++failures;                                                                                                  \
    }
#define RL_FIELD(T, f, o64, o32)                                                                                      \
    if (offsetof(T, f) != (size_t)(is64 ? (o64) : (o32))) {                                                          \
        printf("FAIL (C): offsetof(%s, %s) = %u, reference %u\n", #T, #f, (unsigned)offsetof(T, f),                 \
               (unsigned)(is64 ? (o64) : (o32)));                                                                    \
        ++failures;                                                                                                  \
    }
#define RL_ENUM(name, value)                                                                                          \
    if ((unsigned)(name) != (unsigned)(value)) {                                                                     \
        printf("FAIL (C): %s = 0x%X, reference 0x%X\n", #name, (unsigned)(name), (unsigned)(value));                \
        ++failures;                                                                                                  \
    }
#include "remixapi_layout_ref.inc"
#undef RL_STRUCT
#undef RL_FIELD
#undef RL_ENUM
    return failures;
}
