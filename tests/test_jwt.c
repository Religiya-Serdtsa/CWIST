/**
 * @file test_jwt.c
 * @brief Unit tests for cwist JWT sign/verify API.
 */

#include <cwist/security/jwt/jwt.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

static void test_sign_and_verify(void) {
    printf("Testing JWT sign and verify...\n");

    const char *secret = "supersecret";
    const char *payload = "{\"sub\":\"user42\",\"role\":\"admin\"}";

    char *token = cwist_jwt_sign(payload, secret, 3600);
    assert(token != NULL);
    printf("  Token: %.40s...\n", token);

    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    assert(claims != NULL);

    const char *sub = cwist_jwt_claims_get(claims, "sub");
    assert(sub != NULL);
    assert(strcmp(sub, "user42") == 0);

    const char *role = cwist_jwt_claims_get(claims, "role");
    assert(role != NULL);
    assert(strcmp(role, "admin") == 0);

    cwist_jwt_claims_destroy(claims);
    cwist_free(token);
    printf("  Passed sign and verify.\n");
}

static void test_wrong_secret(void) {
    printf("Testing JWT reject with wrong secret...\n");

    char *token = cwist_jwt_sign("{\"sub\":\"1\"}", "correct-secret", 3600);
    assert(token != NULL);

    cwist_jwt_claims *claims = cwist_jwt_verify(token, "wrong-secret");
    assert(claims == NULL); /* must be rejected */

    cwist_free(token);
    printf("  Passed wrong secret rejection.\n");
}

static void test_tampered_payload(void) {
    printf("Testing JWT reject tampered payload...\n");

    char *token = cwist_jwt_sign("{\"sub\":\"1\",\"role\":\"user\"}", "secret", 3600);
    assert(token != NULL);

    /* Tamper: change one character in the middle of the token */
    /* Find the second dot (start of signature) */
    char *dot1 = strchr(token, '.');
    char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    assert(dot2 != NULL);
    /* Flip a byte in the payload section */
    if (dot2 - token > 2) {
        token[dot2 - token - 1] ^= 0x01;
    }

    cwist_jwt_claims *claims = cwist_jwt_verify(token, "secret");
    assert(claims == NULL); /* must be rejected */

    cwist_free(token);
    printf("  Passed tampered payload rejection.\n");
}

static void test_expired_token(void) {
    printf("Testing JWT reject expired token...\n");

    /* exp_seconds = 0 means we supply our own exp in the payload */
    /* Use a timestamp that is clearly in the past (year 2000 = Unix 946684800) */
    const char *payload = "{\"sub\":\"old\",\"exp\":946684800}";
    char *token = cwist_jwt_sign(payload, "secret", 0);
    assert(token != NULL);

    cwist_jwt_claims *claims = cwist_jwt_verify(token, "secret");
    assert(claims == NULL); /* must be rejected as expired */

    cwist_free(token);
    printf("  Passed expired token rejection.\n");
}

static void test_no_exp(void) {
    printf("Testing JWT without exp claim...\n");

    /* Tokens with no exp claim should verify successfully */
    const char *payload = "{\"sub\":\"neverexpires\"}";
    char *token = cwist_jwt_sign(payload, "secret", 0);
    assert(token != NULL);

    cwist_jwt_claims *claims = cwist_jwt_verify(token, "secret");
    assert(claims != NULL);

    const char *sub = cwist_jwt_claims_get(claims, "sub");
    assert(sub != NULL && strcmp(sub, "neverexpires") == 0);

    cwist_jwt_claims_destroy(claims);
    cwist_free(token);
    printf("  Passed no-exp token.\n");
}

static void test_sequenced_chunks(void) {
    printf("Testing JWT sequenced chunks...\n");

    const char *secret = "chunk-secret";
    const char *payload = "{\"sub\":\"user99\",\"role\":\"user\",\"data\":\"CWIST-sequenced-chunk-test\"}";
    char *token = cwist_jwt_sign(payload, secret, 3600);
    assert(token != NULL);

    size_t count = 0;
    cwist_jwt_chunk_t *chunks = cwist_jwt_split_chunks(token, 8, &count);
    assert(chunks != NULL);
    assert(count >= 3);

    /* Reassemble out of order. */
    cwist_jwt_chunk_t *shuffled = (cwist_jwt_chunk_t *)cwist_alloc_array(count, sizeof(cwist_jwt_chunk_t));
    assert(shuffled != NULL);
    for (size_t i = 0; i < count; i++) {
        shuffled[i].data = chunks[i].data;
        shuffled[i].len = chunks[i].len;
    }
    /* Swap first two chunks. */
    cwist_jwt_chunk_t tmp = shuffled[0];
    shuffled[0] = shuffled[1];
    shuffled[1] = tmp;

    char *rejoined = cwist_jwt_join_chunks(shuffled, count);
    assert(rejoined != NULL);
    assert(strcmp(rejoined, token) == 0);

    cwist_jwt_claims *claims = cwist_jwt_verify(rejoined, secret);
    assert(claims != NULL);
    const char *sub = cwist_jwt_claims_get(claims, "sub");
    assert(sub != NULL && strcmp(sub, "user99") == 0);

    cwist_jwt_claims_destroy(claims);
    cwist_free(rejoined);
    cwist_free(shuffled);
    cwist_jwt_chunks_free(chunks, count);
    cwist_free(token);
    printf("  Passed sequenced chunks.\n");
}

static void test_sign_verify_chunks(void) {
    printf("Testing JWT sign_chunks -> verify_chunks roundtrip...\n");

    const char *secret = "login-secret";
    const char *payload = "{\"sub\":\"login-user\",\"role\":\"admin\"}";

    size_t count = 0;
    cwist_jwt_chunk_t *chunks = cwist_jwt_sign_chunks(payload, secret, 3600, 12, &count);
    assert(chunks != NULL);
    assert(count >= 3);

    /* Shuffle chunks to simulate out-of-order network arrival. */
    cwist_jwt_chunk_t *shuffled = (cwist_jwt_chunk_t *)cwist_alloc_array(count, sizeof(cwist_jwt_chunk_t));
    assert(shuffled != NULL);
    for (size_t i = 0; i < count; i++) {
        shuffled[i].data = chunks[i].data;
        shuffled[i].len = chunks[i].len;
    }
    for (size_t i = 0; i < count; i++) {
        size_t j = (i + 1) % count;
        cwist_jwt_chunk_t tmp = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = tmp;
    }

    cwist_jwt_claims *claims = cwist_jwt_verify_chunks(shuffled, count, secret);
    assert(claims != NULL);

    const char *sub = cwist_jwt_claims_get(claims, "sub");
    assert(sub != NULL && strcmp(sub, "login-user") == 0);

    const char *role = cwist_jwt_claims_get(claims, "role");
    assert(role != NULL && strcmp(role, "admin") == 0);

    cwist_jwt_claims_destroy(claims);
    cwist_free(shuffled);
    cwist_jwt_chunks_free(chunks, count);
    printf("  Passed sign/verify chunks.\n");
}

int main(void) {
    test_sign_and_verify();
    test_wrong_secret();
    test_tampered_payload();
    test_expired_token();
    test_no_exp();
    test_sequenced_chunks();
    test_sign_verify_chunks();
    printf("All JWT tests passed!\n");
    return 0;
}
