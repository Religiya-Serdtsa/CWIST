/**
 * @file session.c
 * @brief Signed client-side session cookie implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/session.h>
#include <cwist/net/http/cookie.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define CWIST_SESSION_DEFAULT_NAME "cwist_session"
#define CWIST_SESSION_DEFAULT_MAX_AGE 86400

struct cwist_session {
    cwist_app *app;
    cwist_http_request *req;
    cwist_query_map *data;
    bool modified;
    bool invalidated;
    bool loaded;
};

/* --- Base64 helpers (RFC 4648) ------------------------------------------ */

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = cwist_alloc(out_len + 1);
    if (!out) return NULL;
    for (size_t i = 0, j = 0; i < len; i += 3, j += 4) {
        uint32_t v = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) v |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) v |= ((uint32_t)data[i + 2]);
        out[j]     = b64[(v >> 18) & 0x3F];
        out[j + 1] = b64[(v >> 12) & 0x3F];
        out[j + 2] = (i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
        out[j + 3] = (i + 2 < len) ? b64[v & 0x3F] : '=';
    }
    out[out_len] = '\0';
    return out;
}

static int base64_decode(const char *in, uint8_t *out, size_t out_len) {
    size_t in_len = strlen(in);
    if (in_len % 4 != 0) return -1;
    size_t pad = 0;
    if (in_len > 0 && in[in_len - 1] == '=') pad++;
    if (in_len > 1 && in[in_len - 2] == '=') pad++;
    size_t len = (in_len / 4) * 3 - pad;
    if (len > out_len) return -1;

    int decode[256];
    for (int i = 0; i < 256; i++) decode[i] = -1;
    for (int i = 0; i < 64; i++) decode[(unsigned char)b64[i]] = i;

    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int a = decode[(unsigned char)in[i]];
        int b = decode[(unsigned char)in[i + 1]];
        int c = decode[(unsigned char)in[i + 2]];
        int d = decode[(unsigned char)in[i + 3]];
        if (a < 0 || b < 0) return -1;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        if (c >= 0) v |= ((uint32_t)c << 6);
        if (d >= 0) v |= (uint32_t)d;
        out[j++] = (v >> 16) & 0xFF;
        if (c >= 0 && in[i + 2] != '=') out[j++] = (v >> 8) & 0xFF;
        if (d >= 0 && in[i + 3] != '=') out[j++] = v & 0xFF;
    }
    return (int)j;
}

/* --- HMAC-SHA256 -------------------------------------------------------- */

static bool hmac_sha256(const char *key, size_t key_len,
                        const char *msg, size_t msg_len,
                        uint8_t out[32]) {
    unsigned int len = 32;
    unsigned char *r = HMAC(EVP_sha256(),
                            key, (int)key_len,
                            (const unsigned char *)msg, msg_len,
                            out, &len);
    return r != NULL && len == 32;
}

/* --- Secret management -------------------------------------------------- */

static char *generate_secret(size_t len) {
    char *secret = cwist_alloc(len * 2 + 1);
    if (!secret) return NULL;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        cwist_free(secret);
        return NULL;
    }
    unsigned char *buf = cwist_alloc(len);
    if (!buf) {
        close(fd);
        cwist_free(secret);
        return NULL;
    }
    ssize_t n = read(fd, buf, len);
    close(fd);
    if (n != (ssize_t)len) {
        cwist_free(buf);
        cwist_free(secret);
        return NULL;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        secret[i * 2]     = hex[buf[i] >> 4];
        secret[i * 2 + 1] = hex[buf[i] & 0x0F];
    }
    secret[len * 2] = '\0';
    cwist_free(buf);
    return secret;
}

int cwist_app_use_session(cwist_app *app, const char *secret) {
    if (!app) return -1;
    if (app->session_secret) cwist_free(app->session_secret);
    if (secret && *secret) {
        app->session_secret = cwist_strdup(secret);
    } else {
        app->session_secret = generate_secret(32);
    }
    if (!app->session_secret) return -1;
    if (!app->session_name) app->session_name = cwist_strdup(CWIST_SESSION_DEFAULT_NAME);
    if (app->session_max_age <= 0) app->session_max_age = CWIST_SESSION_DEFAULT_MAX_AGE;
    return 0;
}

void cwist_app_set_session_name(cwist_app *app, const char *name) {
    if (!app || !name) return;
    if (app->session_name) cwist_free(app->session_name);
    app->session_name = cwist_strdup(name);
}

void cwist_app_set_session_max_age(cwist_app *app, int seconds) {
    if (!app || seconds <= 0) return;
    app->session_max_age = seconds;
}

/* --- JSON serialization helpers ----------------------------------------- */

typedef struct {
    cJSON *json;
} json_ctx_t;

static void add_to_json(const char *key, const char *value, void *ctx) {
    json_ctx_t *jctx = ctx;
    cJSON_AddStringToObject(jctx->json, key, value ? value : "");
}

/* --- Session load/save -------------------------------------------------- */

static bool verify_signature(cwist_app *app, const char *payload_b64, const char *sig_b64) {
    uint8_t sig[32];
    if (base64_decode(sig_b64, sig, sizeof(sig)) != 32) return false;
    uint8_t expected[32];
    if (!hmac_sha256(app->session_secret, strlen(app->session_secret),
                     payload_b64, strlen(payload_b64), expected)) {
        return false;
    }
    return memcmp(sig, expected, 32) == 0;
}

static cwist_query_map *parse_payload(const char *payload_b64) {
    uint8_t payload[8192];
    int len = base64_decode(payload_b64, payload, sizeof(payload) - 1);
    if (len < 0) return NULL;
    payload[len] = '\0';
    cJSON *json = cJSON_Parse((const char *)payload);
    if (!json) return NULL;
    cwist_query_map *map = cwist_query_map_create();
    if (!map) {
        cJSON_Delete(json);
        return NULL;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (cJSON_IsString(item) && item->string && item->valuestring) {
            cwist_query_map_set(map, item->string, item->valuestring);
        }
    }
    cJSON_Delete(json);
    return map;
}

cwist_session_t *cwist_session_start(cwist_app *app,
                                     cwist_http_request *req,
                                     cwist_http_response *res) {
    if (!app || !req || !res) return NULL;
    if (!app->session_secret && cwist_app_use_session(app, NULL) != 0) return NULL;

    cwist_session_t *session = cwist_alloc(sizeof(*session));
    if (!session) return NULL;
    session->app = app;
    session->req = req;
    session->data = cwist_query_map_create();
    session->modified = false;
    session->invalidated = false;
    session->loaded = false;

    cwist_query_map *cookies = cwist_query_map_create();
    if (cookies) {
        const char *cookie_header = cwist_http_header_get(req->headers, "Cookie");
        cwist_cookie_parse(cookies, cookie_header);
        const char *raw = cwist_cookie_get(cookies, app->session_name);
        if (raw) {
            char *copy = cwist_strdup(raw);
            char *dot = strchr(copy, '.');
            if (dot) {
                *dot = '\0';
                if (verify_signature(app, copy, dot + 1)) {
                    cwist_query_map_destroy(session->data);
                    session->data = parse_payload(copy);
                    if (!session->data) session->data = cwist_query_map_create();
                    session->loaded = true;
                }
            }
            cwist_free(copy);
        }
        cwist_query_map_destroy(cookies);
    }

    req->session = session;
    return session;
}

const char *cwist_session_get(cwist_session_t *session, const char *key) {
    if (!session || !session->data || !key) return NULL;
    return cwist_query_map_get(session->data, key);
}

int cwist_session_set(cwist_session_t *session, const char *key, const char *value) {
    if (!session || !key) return -1;
    if (!session->data) return -1;
    cwist_query_map_set(session->data, key, value ? value : "");
    session->modified = true;
    return 0;
}

void cwist_session_delete(cwist_session_t *session, const char *key) {
    if (!session || !session->data || !key) return;
    cwist_query_map_delete(session->data, key);
    session->modified = true;
}

void cwist_session_invalidate(cwist_session_t *session) {
    if (!session) return;
    session->invalidated = true;
    session->modified = true;
}

void cwist_session_destroy(cwist_session_t *session) {
    if (!session) return;
    cwist_query_map_destroy(session->data);
    cwist_free(session);
}

int cwist_session_commit(cwist_session_t *session, cwist_http_response *res) {
    if (!session || !res) return -1;
    if (!session->modified && session->loaded) return 0;

    if (session->invalidated) {
        cwist_cookie_delete(res, session->app->session_name);
        return 0;
    }

    json_ctx_t ctx;
    ctx.json = cJSON_CreateObject();
    if (!ctx.json) return -1;
    cwist_query_map_foreach(session->data, add_to_json, &ctx);
    char *raw = cJSON_PrintUnformatted(ctx.json);
    cJSON_Delete(ctx.json);
    if (!raw) return -1;

    char *payload_b64 = base64_encode((const uint8_t *)raw, strlen(raw));
    free(raw);
    if (!payload_b64) return -1;

    uint8_t sig[32];
    if (!hmac_sha256(session->app->session_secret, strlen(session->app->session_secret),
                     payload_b64, strlen(payload_b64), sig)) {
        cwist_free(payload_b64);
        return -1;
    }
    char *sig_b64 = base64_encode(sig, sizeof(sig));
    if (!sig_b64) {
        cwist_free(payload_b64);
        return -1;
    }

    char cookie_value[16384];
    snprintf(cookie_value, sizeof(cookie_value), "%s.%s", payload_b64, sig_b64);
    cwist_free(payload_b64);
    cwist_free(sig_b64);

    cwist_cookie_options opts = {0};
    opts.path = "/";
    opts.max_age_seconds = session->app->session_max_age;
    opts.http_only = true;
    opts.same_site = "Lax";

    int rc = cwist_cookie_set(res, session->app->session_name, cookie_value, &opts);
    return rc;
}

/* Middleware ------------------------------------------------------------- */

static void cwist_mw_session_handler(cwist_http_request *req,
                                     cwist_http_response *res,
                                     cwist_handler_func next) {
    cwist_app *app = req->app;
    if (!app) {
        next(req, res);
        return;
    }
    cwist_session_start(app, req, res);
    next(req, res);
    if (req->session) {
        cwist_session_commit(req->session, res);
    }
}

cwist_middleware_func cwist_mw_session(cwist_app *app) {
    if (app && !app->session_secret) {
        cwist_app_use_session(app, NULL);
    }
    CWIST_UNUSED(app);
    return cwist_mw_session_handler;
}
