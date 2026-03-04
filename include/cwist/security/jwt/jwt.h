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

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SECURITY_JWT_H__ */
