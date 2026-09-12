#include <cwist/app.h>
#include <cwist/security/db_crypt/db_crypt.h>
#include <cwist/core/mem/alloc.h>

int main(void) {
    cwist_db_crypt_ctx_t ctx;
    memset(&ctx, 0x42, sizeof(ctx));

    const char *plaintext = "SQLite format 3\0Sensitive DB Content";
    size_t sealed_len = 0;
    /* CWIST_DEFER_FREE: sealed/opened never leave main(), so each is
     * released automatically at the end of the block it was declared in
     * (opened at the inner if's close, sealed at the outer one) instead of
     * an explicit free() to keep matched with every return path. */
    unsigned char *sealed CWIST_DEFER_FREE = cwist_db_crypt_seal(&ctx, (const unsigned char *)plaintext, strlen(plaintext), &sealed_len);
    if (sealed) {
        printf("Sealed DB blob created (%zu bytes)\n", sealed_len);

        size_t opened_len = 0;
        unsigned char *opened CWIST_DEFER_FREE = cwist_db_crypt_open(&ctx, sealed, sealed_len, &opened_len);
        if (opened) {
            printf("Opened DB blob successfully (%zu bytes)\n", opened_len);
        }
    }
    return 0;
}
