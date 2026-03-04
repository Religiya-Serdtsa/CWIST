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

int main(void) {
    test_sign_and_verify();
    test_wrong_secret();
    test_tampered_payload();
    test_expired_token();
    test_no_exp();
    printf("All JWT tests passed!\n");
    return 0;
}
