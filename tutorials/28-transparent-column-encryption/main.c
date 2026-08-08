#include <cwist/app.h>
#include <cwist/security/db_crypt/db_crypt.h>

int main(void) {
    cwist_db_crypt_ctx_t ctx;
    memset(&ctx, 0x42, sizeof(ctx));

    const char *plaintext = "SQLite format 3\0Sensitive DB Content";
    size_t sealed_len = 0;
    unsigned char *sealed = cwist_db_crypt_seal(&ctx, (const unsigned char *)plaintext, strlen(plaintext), &sealed_len);
    if (sealed) {
        printf("Sealed DB blob created (%zu bytes)\n", sealed_len);

        size_t opened_len = 0;
        unsigned char *opened = cwist_db_crypt_open(&ctx, sealed, sealed_len, &opened_len);
        if (opened) {
            printf("Opened DB blob successfully (%zu bytes)\n", opened_len);
            free(opened);
        }
        free(sealed);
    }
    return 0;
}
