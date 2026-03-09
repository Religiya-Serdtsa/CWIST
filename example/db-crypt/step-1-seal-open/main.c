#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cwist/security/db_crypt/db_crypt.h>

int main() {
    printf("=== DB Crypt: Seal & Open ===\n");

    /* 1. Set up a 32-byte KEK (Key Encryption Key).
     *    In production, derive this from a strong password (e.g. PBKDF2 / Argon2). */
    cwist_db_crypt_ctx_t ctx;
    memset(ctx.kek, 0xAB, CWIST_DB_CRYPT_KEY_LEN);  /* demo key — use a real one! */

    /* 2. Mock "SQLite database bytes" — normally from sqlite3_serialize() or a file read */
    const char *fake_db = "SQLite format 3\x00 ... (fake database content for demo)";
    size_t db_len = strlen(fake_db) + 1;

    printf("\nPlaintext  (%zu bytes): \"%s\"\n", db_len, fake_db);

    /* 3. Seal (encrypt) */
    printf("\n[Seal]\n");
    size_t sealed_len = 0;
    unsigned char *sealed = cwist_db_crypt_seal(&ctx,
        (const unsigned char *)fake_db, db_len, &sealed_len);
    if (!sealed) {
        fprintf(stderr, "Sealing failed\n");
        return 1;
    }
    printf("Sealed blob: %zu bytes (header=%d + ciphertext)\n",
           sealed_len, CWIST_DB_CRYPT_HDR_LEN);

    /* Print first 16 bytes of sealed blob as hex */
    printf("First 16 bytes: ");
    for (size_t i = 0; i < 16 && i < sealed_len; i++)
        printf("%02x", sealed[i]);
    printf(" ...\n");

    /* 4. Open (decrypt) */
    printf("\n[Open]\n");
    size_t plain_len = 0;
    unsigned char *plain = cwist_db_crypt_open(&ctx, sealed, sealed_len, &plain_len);
    if (!plain) {
        fprintf(stderr, "Opening failed\n");
        free(sealed);
        return 1;
    }
    printf("Recovered  (%zu bytes): \"%s\"\n", plain_len, (char *)plain);
    printf("Match: %s\n", memcmp(plain, fake_db, db_len) == 0 ? "YES" : "NO");

    /* 5. Wrong key should fail */
    printf("\n[Wrong key]\n");
    cwist_db_crypt_ctx_t bad_ctx;
    memset(bad_ctx.kek, 0x00, CWIST_DB_CRYPT_KEY_LEN);
    size_t bad_len = 0;
    unsigned char *bad = cwist_db_crypt_open(&bad_ctx, sealed, sealed_len, &bad_len);
    printf("Decrypt with wrong key: %s\n", bad ? "succeeded (unexpected!)" : "failed (correct)");
    if (bad) free(bad);

    free(sealed);
    free(plain);
    printf("\n=== Done ===\n");
    return 0;
}
