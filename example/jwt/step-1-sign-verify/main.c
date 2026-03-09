#include <stdio.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/core/mem/alloc.h>

int main() {
    printf("=== JWT Sign & Verify ===\n");

    const char *secret = "my-super-secret-key";

    /* 1. Sign a token that expires in 3600 seconds */
    printf("\n[Sign]\n");
    const char *payload = "{\"sub\":\"42\",\"name\":\"Alice\",\"role\":\"admin\"}";
    char *token = cwist_jwt_sign(payload, secret, 3600);
    if (!token) {
        fprintf(stderr, "Signing failed\n");
        return 1;
    }
    printf("Token: %s\n", token);

    /* 2. Verify the token */
    printf("\n[Verify]\n");
    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    if (!claims) {
        fprintf(stderr, "Verification failed\n");
        cwist_free(token);
        return 1;
    }

    /* 3. Read individual claims */
    printf("sub  : %s\n", cwist_jwt_claims_get(claims, "sub")  ?: "(null)");
    printf("name : %s\n", cwist_jwt_claims_get(claims, "name") ?: "(null)");
    printf("role : %s\n", cwist_jwt_claims_get(claims, "role") ?: "(null)");
    printf("exp  : %s\n", cwist_jwt_claims_get(claims, "exp")  ?: "(null)");

    /* 4. Tampered token should fail */
    printf("\n[Tampered token]\n");
    token[10] ^= 0x01; /* flip one bit */
    cwist_jwt_claims *bad = cwist_jwt_verify(token, secret);
    printf("Tampered verify result: %s\n", bad ? "accepted (unexpected!)" : "rejected (correct)");

    cwist_jwt_claims_destroy(claims);
    if (bad) cwist_jwt_claims_destroy(bad);
    cwist_free(token);
    printf("\n=== Done ===\n");
    return 0;
}
