#ifndef __CWIST_SIPHASH_H__
#define __CWIST_SIPHASH_H__

#include <stdint.h>
#include <stddef.h>

/**
 * siphash24: Computes a 64-bit hash using the SipHash-2-4 algorithm.
 * @src: Pointer to the input data.
 * @len: Length of the input data in bytes.
 * @key: A 16-byte secret key (used as two 64-bit integers).
 * * Returns the 64-bit hash value.
 */
uint64_t siphash24(const void *src, size_t len, const uint8_t key[16]);
/**
 * @brief Generates a random SipHash seed.
 * @code
 * uint8_t key[16];
 * cwist_generate_hash_seed(key);
 * uint64_t hash = siphash24(src, len, key);
 * @endcode
 */
void cwist_generate_hash_seed(uint8_t key[16]);
#endif
