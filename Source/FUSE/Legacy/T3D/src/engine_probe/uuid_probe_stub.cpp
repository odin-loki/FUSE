// UUID generate/toString/fromString for FUSE_T3D_LEGACY_ENGINE_PROBE.
// Ported from Engine/source/core/util/uuid.cpp (v1 token path) without
// UUIDEngineExport / engineTypeInfo.cpp — not full uuid.cpp link.

#include "core/util/md5.h"
#include "core/util/uuid.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char nodeID[6];
} uuid_node_t;

#undef xuuid_t

typedef struct {
    uuid_node_t node;
    unsigned short cs;
} uuid_state;

#define UUIDS_PER_TICK 1024

#define unsigned64_t unsigned long long
#define I64(C) C##LL

typedef unsigned64_t uuid_time_t;

static void format_uuid_v1(xuuid_t* uuid, unsigned short clockseq, uuid_time_t timestamp, uuid_node_t node);
static void get_current_time(uuid_time_t* timestamp);
static unsigned short true_random(void);
static void get_pseudo_node_identifier(uuid_node_t* node);
static void get_system_time(uuid_time_t* uuid_time);
static void get_random_info(unsigned char seed[16]);

static int create_token(uuid_state* st, xuuid_t* u)
{
    uuid_time_t timestamp;

    get_current_time(&timestamp);
    format_uuid_v1(u, st->cs, timestamp, st->node);

    return 1;
}

static void create_uuid_state(uuid_state* st)
{
    st->cs = true_random();
    get_pseudo_node_identifier(&st->node);
}

static int dav_parse_hexpair(const char* s)
{
    int result;
    int temp;

    result = s[0] - '0';
    if (result > 48) {
        result = (result - 39) << 4;
    } else if (result > 16) {
        result = (result - 7) << 4;
    } else {
        result = result << 4;
    }

    temp = s[1] - '0';
    if (temp > 48) {
        result |= temp - 39;
    } else if (temp > 16) {
        result |= temp - 7;
    } else {
        result |= temp;
    }

    return result;
}

static int parse_token(const char* char_token, xuuid_t* bin_token)
{
    int i;

    for (i = 0; i < 36; ++i) {
        const char c = char_token[i];
        if (!isxdigit(c) && !(c == '-' && (i == 8 || i == 13 || i == 18 || i == 23))) {
            return -1;
        }
    }
    if (char_token[36] != '\0') {
        return -1;
    }

    bin_token->time_low = (dav_parse_hexpair(&char_token[0]) << 24) |
                          (dav_parse_hexpair(&char_token[2]) << 16) |
                          (dav_parse_hexpair(&char_token[4]) << 8) | dav_parse_hexpair(&char_token[6]);

    bin_token->time_mid = (dav_parse_hexpair(&char_token[9]) << 8) | dav_parse_hexpair(&char_token[11]);

    bin_token->time_hi_and_version = (dav_parse_hexpair(&char_token[14]) << 8) |
                                     dav_parse_hexpair(&char_token[16]);

    bin_token->clock_seq_hi_and_reserved = dav_parse_hexpair(&char_token[19]);
    bin_token->clock_seq_low = dav_parse_hexpair(&char_token[21]);

    for (i = 6; i--;) {
        bin_token->node[i] = dav_parse_hexpair(&char_token[i * 2 + 24]);
    }

    return 0;
}

static void format_uuid_v1(xuuid_t* uuid, unsigned short clock_seq, uuid_time_t timestamp, uuid_node_t node)
{
    uuid->time_low = (unsigned long)(timestamp & 0xFFFFFFFF);
    uuid->time_mid = (unsigned short)((timestamp >> 32) & 0xFFFF);
    uuid->time_hi_and_version = (unsigned short)((timestamp >> 48) & 0x0FFF);
    uuid->time_hi_and_version |= (1 << 12);
    uuid->clock_seq_low = clock_seq & 0xFF;
    uuid->clock_seq_hi_and_reserved = (clock_seq & 0x3F00) >> 8;
    uuid->clock_seq_hi_and_reserved |= 0x80;
    memcpy(&uuid->node, &node, sizeof uuid->node);
}

static void get_current_time(uuid_time_t* timestamp)
{
    uuid_time_t time_now;
    static uuid_time_t time_last;
    static unsigned short uuids_this_tick;
    static int inited = 0;

    if (!inited) {
        get_system_time(&time_now);
        uuids_this_tick = UUIDS_PER_TICK;
        inited = 1;
    }

    while (1) {
        get_system_time(&time_now);

        if (time_last != time_now) {
            uuids_this_tick = 0;
            break;
        }
        if (uuids_this_tick < UUIDS_PER_TICK) {
            uuids_this_tick++;
            break;
        }
    }

    *timestamp = time_now + uuids_this_tick;
    time_last = time_now;
}

static unsigned short true_random(void)
{
    uuid_time_t time_now;

    get_system_time(&time_now);
    time_now = time_now / UUIDS_PER_TICK;
    srand((unsigned int)(((time_now >> 32) ^ time_now) & 0xffffffff));

    return static_cast<unsigned short>(rand());
}

static void get_pseudo_node_identifier(uuid_node_t* node)
{
    unsigned char seed[16];

    get_random_info(seed);
    seed[0] |= 0x80;
    memcpy(node, seed, sizeof(uuid_node_t));
}

static void get_system_time(uuid_time_t* uuid_time)
{
    struct timeval tp;

    gettimeofday(&tp, (struct timezone*)0);

    *uuid_time = (tp.tv_sec * 10000000) + (tp.tv_usec * 10) + I64(0x01B21DD213814000);
}

static void get_random_info(unsigned char seed[16])
{
    MD5_CTX c;
    struct {
        pid_t pid;
        struct timeval t;
        char hostname[257];

    } r;

    MD5Init(&c);
    r.pid = getpid();
    gettimeofday(&r.t, (struct timezone*)0);
    gethostname(r.hostname, 256);
    MD5Update(&c, (unsigned char*)&r, sizeof(r));
    MD5Final(seed, &c);
}

namespace {
bool gUUIDStateInitialized;
uuid_state gUUIDState;
} // namespace

namespace Torque
{

UUID UUID::smNull;

void UUID::generate()
{
    if (!gUUIDStateInitialized) {
        create_uuid_state(&gUUIDState);
        gUUIDStateInitialized = true;
    }

    create_token(&gUUIDState, (xuuid_t*)this);
}

String UUID::toString() const
{
    const xuuid_t* u = (xuuid_t*)this;
    char buffer[64];
    snprintf(buffer,
                  sizeof(buffer),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  u->time_low,
                  u->time_mid,
                  u->time_hi_and_version,
                  u->clock_seq_hi_and_reserved,
                  u->clock_seq_low,
                  u->node[0],
                  u->node[1],
                  u->node[2],
                  u->node[3],
                  u->node[4],
                  u->node[5]);
    return String(buffer);
}

bool UUID::fromString(const char* str)
{
    if (parse_token(str, (xuuid_t*)this) != 0) {
        dMemset(this, 0, sizeof(UUID));
        return false;
    }

    return true;
}

U32 UUID::getHash() const
{
    return (a + b + c + d + e + f[0] + f[1] + f[2] + f[3] + f[4] + f[5]);
}

} // namespace Torque
