#ifndef __CWIST_WS_UTILS_H__
#define __CWIST_WS_UTILS_H__

/**
 * @file ws_utils.h
 * @brief Internal cryptographic helpers used during the WebSocket handshake.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Compute the SHA-1 digest for a byte range.
 * @param data Input buffer to hash.
 * @param len Number of bytes in @p data.
 * @param hash Output buffer that receives the 20-byte SHA-1 digest.
 */
void sha1(const uint8_t *data, size_t len, uint8_t *hash);
/**
 * @brief Base64-encode a byte buffer for handshake header generation.
 * @param data Input buffer to encode.
 * @param input_length Number of bytes in @p data.
 * @param output_length Optional output parameter for the encoded length.
 * @return Heap-allocated encoded string, or NULL when allocation fails.
 */
char *base64_encode(const uint8_t *data, size_t input_length, size_t *output_length);

#endif
