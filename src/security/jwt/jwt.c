#include <cwist/security/jwt/jwt.h>
#include <cwist/core/mem/alloc.h>

#include <cjson/cJSON.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------------------
 * Internal: Base64URL helpers
 * -------------------------------------------------------------------------- */

/** Return the Base64URL-encoded length for @p input_len raw bytes. */
static size_t b64url_encoded_len(size_t input_len) {
    return ((input_len + 2) / 3) * 4 + 1; /* +1 for '\0' */
}

/** Encode @p src_len bytes of @p src into @p dst (null-terminated). */
static void b64url_encode(const unsigned char *src, size_t src_len, char *dst) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0, j = 0;
    while (i + 2 < src_len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8) | src[i+2];
        dst[j++] = tbl[(v >> 18) & 0x3F];
        dst[j++] = tbl[(v >> 12) & 0x3F];
        dst[j++] = tbl[(v >>  6) & 0x3F];
        dst[j++] = tbl[ v        & 0x3F];
        i += 3;
    }
    if (i + 1 == src_len) {
        uint32_t v = (uint32_t)src[i] << 4;
        dst[j++] = tbl[(v >> 6) & 0x3F];
        dst[j++] = tbl[ v       & 0x3F];
    } else if (i + 2 == src_len) {
        uint32_t v = ((uint32_t)src[i] << 10) | ((uint32_t)src[i+1] << 2);
        dst[j++] = tbl[(v >> 12) & 0x3F];
        dst[j++] = tbl[(v >>  6) & 0x3F];
        dst[j++] = tbl[ v        & 0x3F];
    }
    dst[j] = '\0';
}

/**
 * Decode a Base64URL string.
 * Returns heap-allocated buffer and sets *out_len, or NULL on error.
 * Caller must free with cwist_free().
 */
static unsigned char *b64url_decode(const char *src, size_t src_len, size_t *out_len) {
    /* Accept either standard or url-safe alphabet; strip '=' padding. */
    static const signed char lut[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63, /* '+' -> 62, '-' -> 62, '/' & '_' -> 63 */
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1, /* '0'-'9', '=' -> 0 */
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* 'A'-'O' */
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63, /* 'P'-'Z', '_' -> 63 */
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* 'a'-'o' */
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, /* 'p'-'z' */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    /* Calculate how many padding chars to expect */
    size_t padded_len = src_len;
    while (padded_len > 0 && src[padded_len - 1] == '=') {
        padded_len--;
    }

    size_t decoded_len = (padded_len * 3) / 4;
    unsigned char *out = (unsigned char *)cwist_alloc(decoded_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i + 3 < padded_len) {
        signed char a = lut[(unsigned char)src[i]];
        signed char b = lut[(unsigned char)src[i+1]];
        signed char c = lut[(unsigned char)src[i+2]];
        signed char d = lut[(unsigned char)src[i+3]];
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            cwist_free(out);
            return NULL;
        }
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        out[j++] = (unsigned char)((b << 4) | (c >> 2));
        out[j++] = (unsigned char)((c << 6) |  d);
        i += 4;
    }

    size_t rem = padded_len - i;
    if (rem == 2) {
        signed char a = lut[(unsigned char)src[i]];
        signed char b = lut[(unsigned char)src[i+1]];
        if (a < 0 || b < 0) { cwist_free(out); return NULL; }
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
    } else if (rem == 3) {
        signed char a = lut[(unsigned char)src[i]];
        signed char b = lut[(unsigned char)src[i+1]];
        signed char c = lut[(unsigned char)src[i+2]];
        if (a < 0 || b < 0 || c < 0) { cwist_free(out); return NULL; }
        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        out[j++] = (unsigned char)((b << 4) | (c >> 2));
    }

    out[j] = '\0';
    *out_len = j;
    return out;
}

/* --------------------------------------------------------------------------
 * Internal: HMAC-SHA256
 * -------------------------------------------------------------------------- */

/** Compute HMAC-SHA256 of @p msg using @p key; writes to @p out (32 bytes). */
static bool hmac_sha256(const char *key, size_t key_len,
                        const char *msg, size_t msg_len,
                        unsigned char out[32]) {
    unsigned int out_len = 0;
    unsigned char *r = HMAC(EVP_sha256(),
                            key, (int)key_len,
                            (const unsigned char *)msg, msg_len,
                            out, &out_len);
    return r != NULL && out_len == 32;
}

/* --------------------------------------------------------------------------
 * JWT claims (opaque struct)
 * -------------------------------------------------------------------------- */

struct cwist_jwt_claims {
    cJSON *json; ///< Parsed payload JSON object.
};

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

char *cwist_jwt_sign(const char *payload_json, const char *secret, long exp_seconds) {
    if (!payload_json || !secret) return NULL;

    /* --- Build header JSON ------------------------------------------------ */
    static const char *HEADER_JSON = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

    /* --- Optionally inject exp/iat into payload --------------------------- */
    cJSON *payload = cJSON_Parse(payload_json);
    if (!payload) return NULL;

    if (exp_seconds > 0) {
        time_t now = time(NULL);
        /* Only add if not already present */
        if (!cJSON_GetObjectItemCaseSensitive(payload, "iat")) {
            cJSON_AddNumberToObject(payload, "iat", (double)now);
        }
        if (!cJSON_GetObjectItemCaseSensitive(payload, "exp")) {
            cJSON_AddNumberToObject(payload, "exp", (double)(now + exp_seconds));
        }
    }

    char *final_payload_json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!final_payload_json) return NULL;

    /* --- Base64URL encode header and payload ------------------------------ */
    size_t hdr_enc_len = b64url_encoded_len(strlen(HEADER_JSON));
    size_t pay_enc_len = b64url_encoded_len(strlen(final_payload_json));

    char *hdr_enc = (char *)cwist_alloc(hdr_enc_len);
    char *pay_enc = (char *)cwist_alloc(pay_enc_len);
    if (!hdr_enc || !pay_enc) {
        cwist_free(hdr_enc);
        cwist_free(pay_enc);
        free(final_payload_json);
        return NULL;
    }

    b64url_encode((const unsigned char *)HEADER_JSON, strlen(HEADER_JSON), hdr_enc);
    b64url_encode((const unsigned char *)final_payload_json, strlen(final_payload_json), pay_enc);
    free(final_payload_json);

    /* --- Build "header.payload" signing input ----------------------------- */
    size_t signing_input_len = strlen(hdr_enc) + 1 + strlen(pay_enc);
    char *signing_input = (char *)cwist_alloc(signing_input_len + 1);
    if (!signing_input) {
        cwist_free(hdr_enc);
        cwist_free(pay_enc);
        return NULL;
    }
    snprintf(signing_input, signing_input_len + 1, "%s.%s", hdr_enc, pay_enc);

    /* --- Compute HMAC-SHA256 signature ------------------------------------ */
    unsigned char sig_raw[32];
    if (!hmac_sha256(secret, strlen(secret), signing_input, signing_input_len, sig_raw)) {
        cwist_free(hdr_enc);
        cwist_free(pay_enc);
        cwist_free(signing_input);
        return NULL;
    }

    size_t sig_enc_len = b64url_encoded_len(32);
    char *sig_enc = (char *)cwist_alloc(sig_enc_len);
    if (!sig_enc) {
        cwist_free(hdr_enc);
        cwist_free(pay_enc);
        cwist_free(signing_input);
        return NULL;
    }
    b64url_encode(sig_raw, 32, sig_enc);

    /* --- Assemble final token --------------------------------------------- */
    size_t token_len = signing_input_len + 1 + strlen(sig_enc);
    char *token = (char *)cwist_alloc(token_len + 1);
    if (!token) {
        cwist_free(hdr_enc);
        cwist_free(pay_enc);
        cwist_free(signing_input);
        cwist_free(sig_enc);
        return NULL;
    }
    snprintf(token, token_len + 1, "%s.%s", signing_input, sig_enc);

    cwist_free(hdr_enc);
    cwist_free(pay_enc);
    cwist_free(signing_input);
    cwist_free(sig_enc);

    return token;
}

cwist_jwt_claims *cwist_jwt_verify(const char *token, const char *secret) {
    if (!token || !secret) return NULL;

    /* --- Split the token into three parts --------------------------------- */
    /* We need mutable copies to split on '.' */
    char *tok_copy = cwist_strdup(token);
    if (!tok_copy) return NULL;

    char *dot1 = strchr(tok_copy, '.');
    if (!dot1) { cwist_free(tok_copy); return NULL; }
    *dot1 = '\0';

    char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) { cwist_free(tok_copy); return NULL; }
    *dot2 = '\0';

    const char *pay_enc  = dot1 + 1;
    const char *sig_enc  = dot2 + 1;

    /* --- Re-build the signing input from the original token --------------- */
    /* signing_input = original "hdr_enc.pay_enc" (up to the second dot) */
    size_t first_two_len = (size_t)(dot2 - tok_copy);
    /* dot2 points inside tok_copy which is already modified; use original */
    char *signing_input = (char *)cwist_alloc(first_two_len + 1);
    if (!signing_input) { cwist_free(tok_copy); return NULL; }
    memcpy(signing_input, token, first_two_len);
    signing_input[first_two_len] = '\0';

    /* --- Recompute expected signature ------------------------------------- */
    unsigned char expected_sig[32];
    if (!hmac_sha256(secret, strlen(secret), signing_input, first_two_len, expected_sig)) {
        cwist_free(tok_copy);
        cwist_free(signing_input);
        return NULL;
    }
    cwist_free(signing_input);

    /* --- Decode the provided signature ------------------------------------ */
    size_t provided_sig_len = 0;
    unsigned char *provided_sig = b64url_decode(sig_enc, strlen(sig_enc), &provided_sig_len);
    if (!provided_sig || provided_sig_len != 32) {
        cwist_free(tok_copy);
        cwist_free(provided_sig);
        return NULL;
    }

    /* --- Constant-time comparison ---------------------------------------- */
    int cmp = 0;
    for (int i = 0; i < 32; i++) {
        cmp |= expected_sig[i] ^ provided_sig[i];
    }
    cwist_free(provided_sig);

    if (cmp != 0) {
        cwist_free(tok_copy);
        return NULL; /* signature mismatch */
    }

    /* --- Decode and parse the payload ------------------------------------- */
    size_t pay_json_len = 0;
    unsigned char *pay_json = b64url_decode(pay_enc, strlen(pay_enc), &pay_json_len);
    cwist_free(tok_copy);

    if (!pay_json) return NULL;

    cJSON *json = cJSON_ParseWithLength((const char *)pay_json, pay_json_len);
    cwist_free(pay_json);

    if (!json) return NULL;

    /* --- Validate "exp" claim if present ---------------------------------- */
    cJSON *exp_item = cJSON_GetObjectItemCaseSensitive(json, "exp");
    if (exp_item && cJSON_IsNumber(exp_item)) {
        time_t now = time(NULL);
        if ((time_t)exp_item->valuedouble < now) {
            cJSON_Delete(json);
            return NULL; /* token expired */
        }
    }

    /* --- Build and return claims object ----------------------------------- */
    cwist_jwt_claims *claims = (cwist_jwt_claims *)cwist_alloc(sizeof(cwist_jwt_claims));
    if (!claims) {
        cJSON_Delete(json);
        return NULL;
    }
    claims->json = json;
    return claims;
}

const char *cwist_jwt_claims_get(const cwist_jwt_claims *claims, const char *key) {
    if (!claims || !key) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(claims->json, key);
    if (!item) return NULL;
    if (cJSON_IsString(item)) return item->valuestring;
    /* For numeric claims return the raw JSON representation */
    return NULL;
}

void cwist_jwt_claims_destroy(cwist_jwt_claims *claims) {
    if (!claims) return;
    cJSON_Delete(claims->json);
    cwist_free(claims);
}
