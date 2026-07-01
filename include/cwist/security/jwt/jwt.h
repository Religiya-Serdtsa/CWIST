/**
 * @file jwt.h
 * @brief JWT (JSON Web Token) generation and verification.
 *
 * Supports HS256 (HMAC-SHA256) signed tokens only.
 * Wrapper over OpenSSL, following the cwist security module convention.
 */

#ifndef __CWIST_SECURITY_JWT_H__
#define __CWIST_SECURITY_JWT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque JWT claims object returned after successful verification.
 */
typedef struct cwist_jwt_claims cwist_jwt_claims;

/**
 * @brief Sign a JWT token with HMAC-SHA256.
 *
 * @param payload_json  JSON string for the payload (e.g. "{\"sub\":\"1\"}").
 *                      Standard claims like "exp" and "iat" are added automatically
 *                      when @p exp_seconds is > 0.  Supply them yourself if you need
 *                      fine-grained control (pass exp_seconds = 0 in that case).
 * @param secret        HMAC secret key (null-terminated).
 * @param exp_seconds   Seconds until expiry from now.  Pass 0 to skip automatic
 *                      "exp" injection (you must include it in payload_json).
 * @return Heap-allocated null-terminated JWT string, or NULL on error.
 *         Caller must free with cwist_free().
 */
char *cwist_jwt_sign(const char *payload_json, const char *secret, long exp_seconds);

/**
 * @brief Verify a JWT token and return the decoded claims.
 *
 * Validates:
 *  - Structural integrity (three Base64URL-encoded parts).
 *  - HMAC-SHA256 signature.
 *  - "exp" claim (token must not be expired) if present.
 *
 * @param token   Null-terminated JWT string ("header.payload.sig").
 * @param secret  HMAC secret key (null-terminated).
 * @return Pointer to a cwist_jwt_claims object on success, NULL on failure.
 *         Caller must free with cwist_jwt_claims_destroy().
 */
cwist_jwt_claims *cwist_jwt_verify(const char *token, const char *secret);

/**
 * @brief Retrieve a claim value by key from decoded claims.
 *
 * @param claims  Claims object returned by cwist_jwt_verify().
 * @param key     JSON key to look up (e.g. "sub").
 * @return Read-only pointer to the value string, or NULL if not found.
 *         The pointer is valid until cwist_jwt_claims_destroy() is called.
 */
const char *cwist_jwt_claims_get(const cwist_jwt_claims *claims, const char *key);

/**
 * @brief Free a claims object.
 *
 * @param claims  Object to free (may be NULL).
 */
void cwist_jwt_claims_destroy(cwist_jwt_claims *claims);

/**
 * @brief One sequenced JWT chunk produced by cwist_jwt_split_chunks().
 *
 * The data buffer is heap-allocated and includes the 8-byte sequence header
 * followed by a substring of the original token.
 */
typedef struct cwist_jwt_chunk {
    uint8_t *data;  ///< Chunk bytes (header + payload).
    size_t len;     ///< Length of @p data.
} cwist_jwt_chunk_t;

/**
 * @brief Split a JWT token into sequenced chunks.
 *
 * The returned chunks can be transmitted out of order and reassembled with
 * cwist_jwt_join_chunks().  This is useful when the token is too large for a
 * single UDP datagram, WebSocket frame, or HTTP/2 DATA frame.
 *
 * @param token JWT string to split.
 * @param chunk_payload_size Maximum token bytes per chunk (must be > 0).
 * @param out_count Receives the number of chunks.
 * @return Array of chunk objects, or NULL on error.  Caller must free with
 *         cwist_jwt_chunks_free().
 */
cwist_jwt_chunk_t *cwist_jwt_split_chunks(const char *token,
                                          uint16_t chunk_payload_size,
                                          size_t *out_count);

/**
 * @brief Free chunk objects returned by cwist_jwt_split_chunks().
 */
void cwist_jwt_chunks_free(cwist_jwt_chunk_t *chunks, size_t count);

/**
 * @brief Reassemble a JWT token from sequenced chunks.
 *
 * Duplicate chunks are discarded.  The chunks may be supplied in any order,
 * but all chunks for one token must be present.
 *
 * @param chunks Array of chunk objects.
 * @param count Number of chunks.
 * @return Heap-allocated token string, or NULL on error.  Caller must free
 *         with cwist_free().
 */
char *cwist_jwt_join_chunks(const cwist_jwt_chunk_t *chunks, size_t count);

/**
 * @brief Sign a JWT and immediately split it into sequenced chunks.
 *
 * This is a convenience helper for the login/authentication path: the caller
 * receives chunks that can be transmitted out of order and later verified with
 * cwist_jwt_verify_chunks().
 *
 * @param payload_json JSON payload for the new token.
 * @param secret HMAC secret key.
 * @param exp_seconds Lifetime in seconds (see cwist_jwt_sign()).
 * @param chunk_payload_size Maximum token bytes per chunk (must be > 0).
 * @param out_count Receives the number of chunks.
 * @return Array of chunk objects, or NULL on error.  Caller must free with
 *         cwist_jwt_chunks_free().
 */
cwist_jwt_chunk_t *cwist_jwt_sign_chunks(const char *payload_json,
                                         const char *secret,
                                         long exp_seconds,
                                         uint16_t chunk_payload_size,
                                         size_t *out_count);

/**
 * @brief Reassemble sequenced JWT chunks and verify the resulting token.
 *
 * Chunks may arrive in any order; duplicates are discarded.  The function
 * performs the same validation as cwist_jwt_verify() on the reassembled token.
 *
 * @param chunks Array of chunk objects received from the peer.
 * @param count Number of chunks.
 * @param secret HMAC secret key.
 * @return Claims object on success, NULL when reassembly or verification fails.
 *         Caller must free with cwist_jwt_claims_destroy().
 */
cwist_jwt_claims *cwist_jwt_verify_chunks(const cwist_jwt_chunk_t *chunks,
                                          size_t count,
                                          const char *secret);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SECURITY_JWT_H__ */
