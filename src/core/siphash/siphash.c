#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <cwist/core/siphash/siphash.h>

/**
 * @file siphash.c
 * @brief SipHash implementation plus CWIST-specific entropy expansion for hash seeds.
 */

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

/**
 * @brief Compute SipHash-2-4 over a byte range using a 16-byte secret key.
 * @param src Input bytes to hash.
 * @param len Number of bytes in @p src.
 * @param key 16-byte SipHash key.
 * @return 64-bit keyed hash value.
 */
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

static const uint8_t CHOE_ORTHO_PRIMARY[4][4] = {
    {0, 1, 2, 3},
    {1, 0, 3, 2},
    {2, 3, 0, 1},
    {3, 2, 1, 0}
};

static const uint8_t CHOE_ORTHO_SECONDARY[4][4] = {
    {0, 1, 2, 3},
    {2, 3, 0, 1},
    {3, 2, 1, 0},
    {1, 0, 3, 2}
};

/**
 * @brief Rotate a 64-bit value left by the requested amount.
 * @param v Input value to rotate.
 * @param r Rotation count in bits.
 * @return Rotated value.
 */
static inline uint64_t rotl64(uint64_t v, unsigned int r) {
    return (v << r) | (v >> (64U - r));
}

/**
 * @brief Fill a buffer with entropy from /dev/urandom or a timing-based fallback.
 * @param buf Destination buffer to fill.
 * @param len Number of bytes to generate.
 */
static void cwist_entropy_fill(uint8_t *buf, size_t len) {
    size_t filled = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (filled < len) {
            ssize_t n = read(fd, buf + filled, len - filled);
            if (n <= 0) break;
            filled += (size_t)n;
        }
        close(fd);
    }
    if (filled < len) {
        struct timespec ts = {0};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t fallbacks[2] = {
            (uint64_t)ts.tv_nsec ^ (uint64_t)getpid(),
            (uint64_t)ts.tv_sec ^ (uint64_t)getppid()
        };
        srand((unsigned int)(fallbacks[0] ^ fallbacks[1]));
        size_t idx = 0;
        while (filled + idx < len) {
            uint8_t val = (uint8_t)(fallbacks[idx % 2] >> ((idx * 13) & 63));
            val ^= (uint8_t)(rand() & 0xff);
            buf[filled + idx] = val;
            idx++;
        }
    }
}

/**
 * @brief Mix two 2-bit cell coordinates into one Latin-square derived nibble.
 * @param a First input byte.
 * @param b Second input byte.
 * @return Mixed 4-bit value encoded in the low nibble of the byte.
 */
static uint8_t gusuryak_cell(uint8_t a, uint8_t b) {
    uint8_t row = a & 0x3;
    uint8_t col = b & 0x3;
    uint8_t primary = CHOE_ORTHO_PRIMARY[row][col];
    uint8_t secondary = CHOE_ORTHO_SECONDARY[col][row];
    return (uint8_t)((primary << 2) | secondary);
}

/**
 * @brief Expand 16 raw entropy bytes through the Gusuryak-inspired mixing stage.
 * @param in Raw entropy bytes.
 * @param out Mixed 16-byte seed material.
 */
static void gusuryak_mix(const uint8_t in[16], uint8_t out[16]) {
    /* Following Choe Seok-jeong (Gusuryak), treat 16 bytes as a 4x4 Latin board. */
    uint64_t hi = 0x9e3779b185ebca87ULL;
    uint64_t lo = 0xc2b2ae3d27d4eb4fULL;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t col = 0; col < 4; ++col) {
            size_t idx = row * 4 + col;
            uint8_t base = in[idx];
            uint8_t partner = in[((col + 1) % 4) + ((row + 1) % 4) * 4];
            uint8_t orth = gusuryak_cell(base, partner);
            uint64_t delta = ((uint64_t)base << 32) |
                             ((uint64_t)orth << 24) |
                             ((uint64_t)row << 12) |
                             ((uint64_t)col << 4) |
                             (uint64_t)gusuryak_cell(partner, base);
            hi ^= rotl64(delta ^ hi, (unsigned int)((row * 13 + col * 7) & 63));
            lo += rotl64(delta + lo, (unsigned int)((row * 11 + col * 5) & 63));
        }
    }
    hi ^= rotl64(lo, 17);
    lo ^= rotl64(hi, 41);
    memcpy(out, &hi, 8);
    memcpy(out + 8, &lo, 8);
}

/**
 * @brief Generate a non-trivial 16-byte key suitable for SipHash table seeding.
 * @param key Output buffer that receives the generated seed.
 */
void cwist_generate_hash_seed(uint8_t key[16]) {
    uint8_t raw[16];
    cwist_entropy_fill(raw, sizeof(raw));
    gusuryak_mix(raw, key);
    if (key[0] == 0 && key[8] == 0) {
        /* keep RFC compatibility but avoid all-zero keys */
        uint64_t tweak = 0xA54FF53A5F1D36F1ULL;
        for (size_t i = 0; i < 16; ++i) {
            key[i] ^= (uint8_t)(tweak >> ((i % 8) * 8));
        }
    }
}
