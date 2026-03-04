#include <cwist/security/db_crypt/db_crypt.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static const unsigned char MAGIC[4] = { 0x43, 0x57, 0x44, 0x42 }; /* "CWDB" */
static const unsigned char VERSION  = 0x01;

/**
 * AES-256-CBC encryption.
 * Returns cipher-text length (padded to 16), or -1 on error.
 * out must be at least (in_len + 16) bytes.
 */
static int aes256_encrypt(const unsigned char *key,
                           const unsigned char *iv,
                           const unsigned char *in,  size_t in_len,
                           unsigned char       *out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 1;
    int len1 = 0, len2 = 0;

    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv);
    ok &= EVP_EncryptUpdate(ctx, out, &len1, in, (int)in_len);
    ok &= EVP_EncryptFinal_ex(ctx, out + len1, &len2);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;
    return len1 + len2;
}

/**
 * AES-256-CBC decryption.
 * Returns plaintext length, or -1 on error.
 * out must be at least in_len bytes.
 */
static int aes256_decrypt(const unsigned char *key,
                           const unsigned char *iv,
                           const unsigned char *in,  size_t in_len,
                           unsigned char       *out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 1;
    int len1 = 0, len2 = 0;

    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv);
    ok &= EVP_DecryptUpdate(ctx, out, &len1, in, (int)in_len);
    ok &= EVP_DecryptFinal_ex(ctx, out + len1, &len2);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;
    return len1 + len2;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

unsigned char *cwist_db_crypt_seal(const cwist_db_crypt_ctx_t *ctx,
                                   const unsigned char *sqlite_bytes,
                                   size_t sqlite_len,
                                   size_t *out_len) {
    if (!ctx || !sqlite_bytes || !out_len) return NULL;

    /* 1. Generate random DEK and IVs. */
    unsigned char dek[CWIST_DB_CRYPT_KEY_LEN];
    unsigned char kek_iv[CWIST_DB_CRYPT_IV_LEN];
    unsigned char dek_iv[CWIST_DB_CRYPT_IV_LEN];

    if (RAND_bytes(dek,    sizeof(dek))    != 1 ||
        RAND_bytes(kek_iv, sizeof(kek_iv)) != 1 ||
        RAND_bytes(dek_iv, sizeof(dek_iv)) != 1) {
        return NULL;
    }

    /* 2. Wrap the DEK: encrypt ( dek_iv || dek ) with KEK.
     *    Plaintext: 16 + 32 = 48 bytes → cipher: 48 bytes (no padding needed,
     *    but EVP adds a full block when input is block-aligned, so we use 48
     *    and disable padding via a temp context).
     *    To keep it simple we just encrypt 48 bytes of dek_iv||dek normally;
     *    EVP pads to 64. We'll store all 64 for safe round-trip. */
    unsigned char dek_plain[CWIST_DB_CRYPT_IV_LEN + CWIST_DB_CRYPT_KEY_LEN];
    memcpy(dek_plain,                         dek_iv, CWIST_DB_CRYPT_IV_LEN);
    memcpy(dek_plain + CWIST_DB_CRYPT_IV_LEN, dek,    CWIST_DB_CRYPT_KEY_LEN);

    /* Encrypted DEK blob: 48 plaintext bytes → 64 ciphertext bytes. */
    unsigned char enc_dek[CWIST_DB_CRYPT_IV_LEN + CWIST_DB_CRYPT_KEY_LEN + 16];
    int enc_dek_len = aes256_encrypt(ctx->kek, kek_iv,
                                     dek_plain, sizeof(dek_plain),
                                     enc_dek);
    if (enc_dek_len <= 0) return NULL;

    /* 3. Encrypt SQLite bytes with DEK. */
    size_t ct_max = sqlite_len + 16 + 1; /* extra block for padding */
    unsigned char *ct = (unsigned char *)malloc(ct_max);
    if (!ct) return NULL;

    int ct_len = aes256_encrypt(dek, dek_iv, sqlite_bytes, sqlite_len, ct);
    if (ct_len <= 0) {
        free(ct);
        return NULL;
    }

    /* 4. Assemble blob:
     *    magic(4) + ver(1) + kek_iv(16) + enc_dek(enc_dek_len)
     *    + dek_iv(16) + ptlen(8) + ct(ct_len)
     */
    size_t hdr_fixed = 4 + 1 + CWIST_DB_CRYPT_IV_LEN + (size_t)enc_dek_len
                       + CWIST_DB_CRYPT_IV_LEN + 8;
    size_t total = hdr_fixed + (size_t)ct_len;

    unsigned char *blob = (unsigned char *)malloc(total);
    if (!blob) {
        free(ct);
        return NULL;
    }

    size_t off = 0;
    memcpy(blob + off, MAGIC,   4);        off += 4;
    blob[off] = VERSION;                   off += 1;
    memcpy(blob + off, kek_iv,  CWIST_DB_CRYPT_IV_LEN); off += CWIST_DB_CRYPT_IV_LEN;
    memcpy(blob + off, enc_dek, (size_t)enc_dek_len);   off += (size_t)enc_dek_len;
    memcpy(blob + off, dek_iv,  CWIST_DB_CRYPT_IV_LEN); off += CWIST_DB_CRYPT_IV_LEN;

    /* plaintext length as little-endian uint64 */
    uint64_t ptlen_le = (uint64_t)sqlite_len;
    unsigned char ptlen_buf[8];
    for (int i = 0; i < 8; i++)
        ptlen_buf[i] = (unsigned char)(ptlen_le >> (8 * i));
    memcpy(blob + off, ptlen_buf, 8);     off += 8;

    memcpy(blob + off, ct, (size_t)ct_len);

    free(ct);

    *out_len = total;
    return blob;
}

unsigned char *cwist_db_crypt_open(const cwist_db_crypt_ctx_t *ctx,
                                   const unsigned char *blob,
                                   size_t blob_len,
                                   size_t *out_len) {
    if (!ctx || !blob || !out_len) return NULL;

    /* Minimum header: 4+1+16+64+16+8 = 109 bytes. */
    if (blob_len < CWIST_DB_CRYPT_HDR_LEN) return NULL;

    size_t off = 0;

    /* 1. Verify magic and version. */
    if (memcmp(blob + off, MAGIC, 4) != 0) return NULL;
    off += 4;
    if (blob[off] != VERSION) return NULL;
    off += 1;

    /* 2. Read KEK IV. */
    const unsigned char *kek_iv = blob + off;
    off += CWIST_DB_CRYPT_IV_LEN;

    /* 3. Decrypt DEK blob (48 bytes padded to 64 during seal).
     *    The encrypted DEK blob occupies:
     *    enc_dek_len = blob_len - fixed_parts_after_enc_dek
     *    fixed_parts_after: dek_iv(16) + ptlen(8) + at_least_one_block(16)
     *    We know enc_dek_len = (blob_len - off - 16 - 8 - ct_len)
     *    but we stored enc_dek_len as 48+16=64 bytes always.
     *    Recalculate: plain was 48 bytes → padded = 64. */
    const size_t enc_dek_len = 64;
    if (blob_len < off + enc_dek_len + CWIST_DB_CRYPT_IV_LEN + 8) return NULL;

    unsigned char dek_plain[CWIST_DB_CRYPT_IV_LEN + CWIST_DB_CRYPT_KEY_LEN + 16];
    int dek_plain_len = aes256_decrypt(ctx->kek, kek_iv,
                                       blob + off, enc_dek_len,
                                       dek_plain);
    if (dek_plain_len < (int)(CWIST_DB_CRYPT_IV_LEN + CWIST_DB_CRYPT_KEY_LEN))
        return NULL;
    off += enc_dek_len;

    const unsigned char *dek_iv = dek_plain;
    const unsigned char *dek    = dek_plain + CWIST_DB_CRYPT_IV_LEN;

    /* 4. Read stored DEK IV (should match what was unwrapped). */
    off += CWIST_DB_CRYPT_IV_LEN; /* skip dek_iv in header (redundant; we use the unwrapped one) */

    /* 5. Read plaintext length (little-endian uint64). */
    uint64_t ptlen_le = 0;
    for (int i = 0; i < 8; i++)
        ptlen_le |= ((uint64_t)blob[off + i]) << (8 * i);
    off += 8;
    size_t expected_pt_len = (size_t)ptlen_le;

    /* 6. Decrypt SQLite bytes. */
    size_t ct_len = blob_len - off;
    if (ct_len == 0) return NULL;

    unsigned char *pt = (unsigned char *)malloc(ct_len + 1); /* extra for safety */
    if (!pt) return NULL;

    int pt_len = aes256_decrypt(dek, dek_iv, blob + off, ct_len, pt);
    if (pt_len < 0 || (size_t)pt_len < expected_pt_len) {
        free(pt);
        return NULL;
    }

    pt[expected_pt_len] = '\0'; /* null-terminate for safety (sqlite doesn't need it) */
    *out_len = expected_pt_len;
    return pt;
}
