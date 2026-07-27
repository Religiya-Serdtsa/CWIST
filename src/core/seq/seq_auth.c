#include <cwist/core/seq/seq_auth.h>
#include <cwist/core/mem/alloc.h>

#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>

struct cwist_seq_auth_context {
    uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE];
    uint8_t session_id[CWIST_SEQ_AUTH_SESSION_ID_SIZE];
    uint8_t *nonces;
    uint8_t *completed_ids;
    size_t capacity;
    size_t nonce_count;
    size_t completed_count;
    size_t nonce_next;
    size_t completed_next;
    pthread_mutex_t lock;
};

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool seq_auth_chunk_valid(const uint8_t *data, size_t len,
                                 cwist_seq_chunk_t *out) {
    if (!data || len < CWIST_SEQ_HEADER_SIZE || !out) return false;
    out->seq = read_u16(data);
    out->total = read_u16(data + 2);
    out->payload_len = read_u16(data + 4);
    out->chunk_size = read_u16(data + 6);
    if (!out->seq || !out->total || out->seq > out->total ||
        !out->payload_len || !out->chunk_size ||
        out->payload_len > out->chunk_size ||
        (out->seq != out->total && out->payload_len != out->chunk_size) ||
        len < (size_t)CWIST_SEQ_AUTH_HEADER_SIZE + out->payload_len) return false;
    out->payload = data + CWIST_SEQ_AUTH_HEADER_SIZE;
    return true;
}

static bool auth_tag(const cwist_seq_auth_context_t *ctx,
                     const uint8_t *header_without_tag, size_t header_len,
                     const uint8_t *payload, size_t payload_len,
                     uint8_t tag[EVP_MAX_MD_SIZE]) {
    HMAC_CTX *h = HMAC_CTX_new();
    unsigned int tag_len = 0;
    bool ok = h && HMAC_Init_ex(h, ctx->key, sizeof(ctx->key), EVP_sha256(), NULL) == 1 &&
              HMAC_Update(h, ctx->session_id, sizeof(ctx->session_id)) == 1 &&
              HMAC_Update(h, header_without_tag, header_len) == 1 &&
              HMAC_Update(h, payload, payload_len) == 1 &&
              HMAC_Final(h, tag, &tag_len) == 1 && tag_len >= CWIST_SEQ_AUTH_TAG_SIZE;
    HMAC_CTX_free(h);
    return ok;
}

static bool cache_contains(const uint8_t *cache, size_t count, size_t width,
                           const uint8_t *value) {
    for (size_t i = 0; i < count; ++i)
        if (CRYPTO_memcmp(cache + i * width, value, width) == 0) return true;
    return false;
}

static void cache_add(uint8_t *cache, size_t *count, size_t *next,
                      size_t capacity, size_t width, const uint8_t *value) {
    memcpy(cache + *next * width, value, width);
    *next = (*next + 1) % capacity;
    if (*count < capacity) (*count)++;
}

cwist_seq_auth_context_t *cwist_seq_auth_context_create(
    const uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE],
    const uint8_t session_id[CWIST_SEQ_AUTH_SESSION_ID_SIZE], size_t replay_window) {
    if (!key || !session_id || replay_window == 0 || replay_window > SIZE_MAX / 28) return NULL;
    cwist_seq_auth_context_t *ctx = cwist_alloc(sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        cwist_free(ctx);
        return NULL;
    }
    ctx->nonces = cwist_alloc_array(replay_window, CWIST_SEQ_AUTH_NONCE_SIZE);
    ctx->completed_ids = cwist_alloc_array(replay_window, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE);
    if (!ctx->nonces || !ctx->completed_ids) { cwist_seq_auth_context_destroy(ctx); return NULL; }
    memcpy(ctx->key, key, sizeof(ctx->key));
    memcpy(ctx->session_id, session_id, sizeof(ctx->session_id));
    ctx->capacity = replay_window;
    return ctx;
}

void cwist_seq_auth_context_destroy(cwist_seq_auth_context_t *ctx) {
    if (!ctx) return;
    OPENSSL_cleanse(ctx->key, sizeof(ctx->key));
    pthread_mutex_destroy(&ctx->lock);
    cwist_free(ctx->nonces);
    cwist_free(ctx->completed_ids);
    cwist_free(ctx);
}

bool cwist_seq_auth_random(uint8_t *out, size_t len) {
    return out && len > 0 && len <= INT_MAX && RAND_bytes(out, (int)len) == 1;
}

bool cwist_seq_auth_wrap(const cwist_seq_auth_context_t *ctx,
                         const uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE],
                         const uint8_t nonce[CWIST_SEQ_AUTH_NONCE_SIZE],
                         const uint8_t *chunk, size_t chunk_len,
                         uint8_t **out, size_t *out_len) {
    cwist_seq_chunk_t parsed;
    if (!ctx || !message_id || !nonce || !out || !out_len ||
        !cwist_seq_chunk_parse(chunk, chunk_len, &parsed) ||
        chunk_len != (size_t)CWIST_SEQ_HEADER_SIZE + parsed.payload_len) return false;
    uint8_t *wire = cwist_alloc(CWIST_SEQ_AUTH_HEADER_SIZE + parsed.payload_len);
    if (!wire) return false;
    memcpy(wire, chunk, CWIST_SEQ_HEADER_SIZE);
    memcpy(wire + CWIST_SEQ_HEADER_SIZE, message_id, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE);
    memcpy(wire + CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE,
           nonce, CWIST_SEQ_AUTH_NONCE_SIZE);
    memcpy(wire + CWIST_SEQ_AUTH_HEADER_SIZE, parsed.payload, parsed.payload_len);
    uint8_t tag[EVP_MAX_MD_SIZE];
    if (!auth_tag(ctx, wire, CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE +
                        CWIST_SEQ_AUTH_NONCE_SIZE, parsed.payload, parsed.payload_len, tag)) {
        cwist_free(wire); return false;
    }
    memcpy(wire + CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE +
           CWIST_SEQ_AUTH_NONCE_SIZE, tag, CWIST_SEQ_AUTH_TAG_SIZE);
    *out = wire;
    *out_len = CWIST_SEQ_AUTH_HEADER_SIZE + parsed.payload_len;
    return true;
}

bool cwist_seq_auth_unwrap(cwist_seq_auth_context_t *ctx, const uint8_t *data,
                           size_t len, cwist_seq_auth_metadata_t *metadata,
                           cwist_seq_chunk_t *chunk) {
    cwist_seq_chunk_t parsed;
    if (!ctx || !metadata || !chunk || !seq_auth_chunk_valid(data, len, &parsed) ||
        len != (size_t)CWIST_SEQ_AUTH_HEADER_SIZE + parsed.payload_len) return false;
    const uint8_t *message_id = data + CWIST_SEQ_HEADER_SIZE;
    const uint8_t *nonce = message_id + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE;
    const uint8_t *tag = nonce + CWIST_SEQ_AUTH_NONCE_SIZE;
    uint8_t expected[EVP_MAX_MD_SIZE];
    if (!auth_tag(ctx, data, CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE +
                        CWIST_SEQ_AUTH_NONCE_SIZE, parsed.payload, parsed.payload_len, expected) ||
        CRYPTO_memcmp(tag, expected, CWIST_SEQ_AUTH_TAG_SIZE) != 0) return false;
    pthread_mutex_lock(&ctx->lock);
    bool replayed = cache_contains(ctx->nonces, ctx->nonce_count,
                                   CWIST_SEQ_AUTH_NONCE_SIZE, nonce) ||
                    cache_contains(ctx->completed_ids, ctx->completed_count,
                                   CWIST_SEQ_AUTH_MESSAGE_ID_SIZE, message_id);
    if (replayed) {
        pthread_mutex_unlock(&ctx->lock);
        return false;
    }
    cache_add(ctx->nonces, &ctx->nonce_count, &ctx->nonce_next, ctx->capacity,
              CWIST_SEQ_AUTH_NONCE_SIZE, nonce);
    pthread_mutex_unlock(&ctx->lock);
    memcpy(metadata->message_id, message_id, CWIST_SEQ_AUTH_MESSAGE_ID_SIZE);
    memcpy(metadata->nonce, nonce, CWIST_SEQ_AUTH_NONCE_SIZE);
    *chunk = parsed;
    return true;
}

void cwist_seq_auth_mark_complete(cwist_seq_auth_context_t *ctx,
                                  const uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE]) {
    if (!ctx || !message_id) return;
    pthread_mutex_lock(&ctx->lock);
    if (cache_contains(ctx->completed_ids, ctx->completed_count,
                       CWIST_SEQ_AUTH_MESSAGE_ID_SIZE, message_id)) {
        pthread_mutex_unlock(&ctx->lock);
        return;
    }
    cache_add(ctx->completed_ids, &ctx->completed_count, &ctx->completed_next, ctx->capacity,
              CWIST_SEQ_AUTH_MESSAGE_ID_SIZE, message_id);
    pthread_mutex_unlock(&ctx->lock);
}
