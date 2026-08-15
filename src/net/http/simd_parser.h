#ifndef __CWIST_SIMD_PARSER_H__
#define __CWIST_SIMD_PARSER_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(__AVX2__)
    #include <immintrin.h>
    #define CWIST_SIMD_AVX2 1
  #endif
  #if defined(__SSE4_2__)
    #include <nmmintrin.h>
    #define CWIST_SIMD_SSE42 1
  #endif
  #if defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64)
    #include <emmintrin.h>
    #define CWIST_SIMD_SSE2 1
  #endif
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
  #include <arm_neon.h>
  #define CWIST_SIMD_NEON 1
#elif (defined(__riscv) || defined(__riscv__)) && defined(__riscv_vector)
  #include <riscv_vector.h>
  #define CWIST_SIMD_RISCV_VECTOR 1
#elif (defined(__powerpc__) || defined(__ppc__) || defined(__PPC__) || defined(__PPC64__) || defined(__ppc64le__) || defined(_ARCH_PPC)) && (defined(__ALTIVEC__) || defined(__VSX__))
  #include <altivec.h>
  #undef vector
  #undef pixel
  #undef bool
  #define CWIST_SIMD_PPC_ALTIVEC 1
#elif (defined(__mips__) || defined(__mips64) || defined(__mips64__) || defined(__mips_msa)) && defined(__mips_msa)
  #include <msa.h>
  #define CWIST_SIMD_MIPS_MSA 1
#endif

/**
 * @brief Search for a character in a buffer using SIMD with SWAR and scalar fallback.
 * @param buf Pointer to the buffer to search.
 * @param len Length of the buffer in bytes.
 * @param c Target character.
 * @return Pointer to the first match, or NULL if not found.
 */
static inline const char *cwist_simd_find_char(const char *buf, size_t len, char c) {
    if (!buf || len == 0) return NULL;

    size_t i = 0;

#if defined(CWIST_SIMD_AVX2)
    __m256i target = _mm256_set1_epi8(c);
    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(buf + i));
        __m256i cmp = _mm256_cmpeq_epi8(chunk, target);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(cmp);
        if (mask != 0) {
            return buf + i + __builtin_ctz(mask);
        }
        i += 32;
    }
#elif defined(CWIST_SIMD_SSE2)
    __m128i target = _mm_set1_epi8(c);
    while (i + 16 <= len) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(const void *)(buf + i));
        __m128i cmp = _mm_cmpeq_epi8(chunk, target);
        uint32_t mask = (uint32_t)_mm_movemask_epi8(cmp);
        if (mask != 0) {
            return buf + i + __builtin_ctz(mask);
        }
        i += 16;
    }
#elif defined(CWIST_SIMD_NEON)
    uint8x16_t target = vdupq_n_u8((uint8_t)c);
#if defined(__aarch64__)
    while (i + 32 <= len) {
        uint8x16_t chunk0 = vld1q_u8((const uint8_t *)(buf + i));
        uint8x16_t chunk1 = vld1q_u8((const uint8_t *)(buf + i + 16));
        uint8x16_t cmp0 = vceqq_u8(chunk0, target);
        uint8x16_t cmp1 = vceqq_u8(chunk1, target);
        uint8x16_t combined = vorrq_u8(cmp0, cmp1);
        if (vmaxvq_u8(combined) != 0) {
            for (size_t k = 0; k < 32; k++) {
                if (buf[i + k] == c) return buf + i + k;
            }
        }
        i += 32;
    }
#endif
    while (i + 16 <= len) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)(buf + i));
        uint8x16_t cmp = vceqq_u8(chunk, target);
#if defined(__aarch64__)
        if (vmaxvq_u8(cmp) != 0) {
            for (size_t k = 0; k < 16; k++) {
                if (buf[i + k] == c) return buf + i + k;
            }
        }
#else
        uint64x2_t cmp64 = vreinterpretq_u64_u8(cmp);
        if (vgetq_lane_u64(cmp64, 0) != 0 || vgetq_lane_u64(cmp64, 1) != 0) {
            for (size_t k = 0; k < 16; k++) {
                if (buf[i + k] == c) return buf + i + k;
            }
        }
#endif
        i += 16;
    }
#elif defined(CWIST_SIMD_RISCV_VECTOR)
    size_t vl;
    while (i < len) {
        vl = __riscv_vsetvl_e8m1(len - i);
        vuint8m1_t chunk = __riscv_vle8_v_u8m1((const uint8_t *)(buf + i), vl);
        vbool8_t mask = __riscv_vmseq_vx_u8m1_b8(chunk, (uint8_t)c, vl);
        long first = __riscv_vfirst_m_b8(mask, vl);
        if (first >= 0) {
            return buf + i + first;
        }
        i += vl;
    }
#elif defined(CWIST_SIMD_PPC_ALTIVEC)
    __vector unsigned char target = vec_splats((unsigned char)c);
    while (i + 16 <= len) {
        __vector unsigned char chunk;
        memcpy(&chunk, buf + i, 16);
        if (vec_any_eq(chunk, target)) {
            for (size_t k = 0; k < 16; k++) {
                if (buf[i + k] == c) return buf + i + k;
            }
        }
        i += 16;
    }
#elif defined(CWIST_SIMD_MIPS_MSA)
    v16u8 target = (v16u8)__builtin_msa_fill_b((unsigned char)c);
    while (i + 16 <= len) {
        v16u8 chunk;
        memcpy(&chunk, buf + i, 16);
        v16u8 cmp = (v16u8)__builtin_msa_ceq_b((v16i8)chunk, (v16i8)target);
        if (__builtin_msa_bnz_b(cmp)) {
            for (size_t k = 0; k < 16; k++) {
                if (buf[i + k] == c) return buf + i + k;
            }
        }
        i += 16;
    }
#endif

    /* 64-bit SWAR (SIMD Within A Register) fast loop for 64-bit architectures */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    uint64_t target64 = ((uint8_t)c) * 0x0101010101010101ULL;
    while (i + 8 <= len) {
        uint64_t chunk64;
        memcpy(&chunk64, buf + i, sizeof(chunk64));
        uint64_t xor_val = chunk64 ^ target64;
        uint64_t has_zero = (xor_val - 0x0101010101010101ULL) & ~xor_val & 0x8080808080808080ULL;
        if (has_zero != 0) {
            for (size_t k = 0; k < 8; k++) {
                if (buf[i + k] == c) return buf + i + k;
            }
        }
        i += 8;
    }
#endif

    /* Scalar fallback for remaining bytes */
    if (i < len) {
        return (const char *)memchr(buf + i, (int)(unsigned char)c, len - i);
    }
    return NULL;
}

/**
 * @brief Search for CRLF ("\r\n") in a buffer using SIMD with scalar fallback.
 * @param buf Pointer to the buffer.
 * @param len Length of the buffer.
 * @return Pointer to '\r' in the first "\r\n", or NULL if not found.
 */
static inline const char *cwist_simd_find_crlf(const char *buf, size_t len) {
    if (!buf || len < 2) return NULL;

    const char *curr = buf;
    size_t remaining = len;

    while (remaining >= 2) {
        const char *cr = cwist_simd_find_char(curr, remaining, '\r');
        if (!cr) return NULL;
        if (cr + 1 < buf + len && cr[1] == '\n') {
            return cr;
        }
        size_t consumed = (size_t)(cr + 1 - curr);
        curr += consumed;
        remaining -= consumed;
    }
    return NULL;
}

/**
 * @brief Search for double CRLF ("\r\n\r\n") marking the end of HTTP headers.
 * @param buf Pointer to the buffer.
 * @param len Length of the buffer.
 * @return Pointer to the first '\r' of "\r\n\r\n", or NULL if not found.
 */
static inline const char *cwist_simd_find_crlfcrlf(const char *buf, size_t len) {
    if (!buf || len < 4) return NULL;

    const char *curr = buf;
    size_t remaining = len;

    while (remaining >= 4) {
        const char *cr = cwist_simd_find_char(curr, remaining, '\r');
        if (!cr) return NULL;
        if (cr + 3 < buf + len && cr[1] == '\n' && cr[2] == '\r' && cr[3] == '\n') {
            return cr;
        }
        size_t consumed = (size_t)(cr + 1 - curr);
        curr += consumed;
        remaining -= consumed;
    }
    return NULL;
}

#endif /* __CWIST_SIMD_PARSER_H__ */
