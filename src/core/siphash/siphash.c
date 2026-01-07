#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <cwist/core/siphash/siphash.h>

/* Left-rotate a 64-bit integer by 'b' bits */
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

/**
 * SipRound: the core mix function of SipHash.
 * it performs ARX (Addition-Rotation-XOR) operations to scramble the internal state.
 */
static void sipround(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3) {
    *v0 += *v1; *v1 = ROTL(*v1, 13); *v1 ^= *v0; *v0 = ROTL(*v0, 32);
    *v2 += *v3; *v3 = ROTL(*v3, 16); *v3 ^= *v2;
    *v0 += *v3; *v3 = ROTL(*v3, 21); *v3 ^= *v0;
    *v2 += *v1; *v1 = ROTL(*v1, 17); *v1 ^= *v2; *v2 = ROTL(*v2, 32);
}

uint64_t siphash24(const void *src, size_t len, const uint8_t key[16]) {
    const uint8_t *m = (const uint8_t *)src;
    uint64_t k0, k1;
    
    /* Load the 16-byte key into two 64-bit integers */
    memcpy(&k0, key, 8);
    memcpy(&k1, key + 8, 8);

    /* 1. Initialization: Mix secret keys with fixed magic constants */
    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    uint64_t b = ((uint64_t)len) << 56;
    const uint8_t *end = m + (len - (len % 8));
    uint64_t mi;

    /* 2. Compression: Process the input in 8-byte blocks */
    for (; m < end; m += 8) {
        memcpy(&mi, m, 8);
        v3 ^= mi;
        sipround(&v0, &v1, &v2, &v3); /* Round 1 */
        sipround(&v0, &v1, &v2, &v3); /* Round 2 */
        v0 ^= mi;
    }

    /* 3. Final Block & Padding: Handle the remaining 0-7 bytes */
    uint64_t t = 0;
    switch (len % 8) {
        /* fall through */
        case 7: t |= ((uint64_t)m[6]) << 48;
        /* fall through */
        case 6: t |= ((uint64_t)m[5]) << 40;
        /* fall through */
        case 5: t |= ((uint64_t)m[4]) << 32;
        /* fall through */
        case 4: t |= ((uint64_t)m[3]) << 24;
        /* fall through */
        case 3: t |= ((uint64_t)m[2]) << 16;
        /* fall through */
        case 2: t |= ((uint64_t)m[1]) << 8;
        /* fall through */
        case 1: t |= ((uint64_t)m[0]);
    }
    
    /* Mix length and final partial block into the state */
    v3 ^= (b | t);
    sipround(&v0, &v1, &v2, &v3);
    sipround(&v0, &v1, &v2, &v3);
    v0 ^= (b | t);

    /* 4. Finalization: 4 additional rounds for security */
    v2 ^= 0xff;
    for (int i = 0; i < 4; ++i) sipround(&v0, &v1, &v2, &v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

void cwist_generate_hash_seed(uint8_t key[16]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, key, 16);
        close(fd);
    } else {
        /* Fallback to a less secure method if /dev/urandom is unavailable */
        for(int i = 0; i < 16; i++) key[i] = (uint8_t)rand();
    }
}
