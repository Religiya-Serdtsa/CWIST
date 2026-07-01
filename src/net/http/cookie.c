/**
 * @file cookie.c
 * @brief HTTP cookie parsing and construction helpers.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/cookie.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* URL-safe cookie characters: unreserved + !#$%&'()*+-./:<>?@[]^_`{|}~ */
static int needs_url_encode(char c) {
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
        return 0;
    }
    return 1;
}

char *cwist_cookie_encode(const char *value) {
    if (!value) return NULL;
    size_t len = strlen(value);
    size_t max = len * 3 + 1;
    char *out = cwist_alloc(max);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (needs_url_encode((char)c)) {
            snprintf(out + j, max - j, "%%%02X", c);
            j += 3;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}

int cwist_cookie_decode(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len == 0) return -1;
    size_t len = strlen(in);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (j >= out_len - 1) return -1;
        if (in[i] == '%' && i + 2 < len) {
            unsigned int hex;
            if (sscanf(in + i + 1, "%2x", &hex) != 1) {
                out[j++] = in[i];
                continue;
            }
            out[j++] = (char)hex;
            i += 2;
        } else if (in[i] == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return (int)j;
}

void cwist_cookie_parse(cwist_query_map *map, const char *header) {
    if (!map || !header || !*header) return;

    char *buf = cwist_strdup(header);
    if (!buf) return;

    char *save = NULL;
    char *pair = strtok_r(buf, ";", &save);
    while (pair) {
        /* trim leading whitespace */
        while (*pair == ' ' || *pair == '\t') pair++;
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            char *name = pair;
            char *value = eq + 1;
            /* trim trailing whitespace on name */
            char *end = name + strlen(name) - 1;
            while (end > name && (*end == ' ' || *end == '\t')) *end-- = '\0';

            char decoded[4096];
            if (cwist_cookie_decode(value, decoded, sizeof(decoded)) >= 0) {
                cwist_query_map_set(map, name, decoded);
            }
        }
        pair = strtok_r(NULL, ";", &save);
    }
    cwist_free(buf);
}

const char *cwist_cookie_get(cwist_query_map *map, const char *name) {
    if (!map || !name) return NULL;
    return cwist_query_map_get(map, name);
}

int cwist_cookie_set(cwist_http_response *res,
                     const char *name,
                     const char *value,
                     const cwist_cookie_options *opts) {
    if (!res || !name) return -1;

    char *encoded = value ? cwist_cookie_encode(value) : cwist_strdup("");
    if (!encoded) return -1;

    cwist_sstring *cookie = cwist_sstring_create();
    if (!cookie) {
        cwist_free(encoded);
        return -1;
    }

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s=%s", name, encoded);
    cwist_sstring_append(cookie, buf);
    cwist_free(encoded);

    if (opts) {
        if (opts->path) {
            snprintf(buf, sizeof(buf), "; Path=%s", opts->path);
            cwist_sstring_append(cookie, buf);
        }
        if (opts->domain) {
            snprintf(buf, sizeof(buf), "; Domain=%s", opts->domain);
            cwist_sstring_append(cookie, buf);
        }
        if (opts->max_age_seconds >= 0) {
            snprintf(buf, sizeof(buf), "; Max-Age=%d", opts->max_age_seconds);
            cwist_sstring_append(cookie, buf);
        }
        if (opts->http_only) {
            cwist_sstring_append(cookie, "; HttpOnly");
        }
        if (opts->secure) {
            cwist_sstring_append(cookie, "; Secure");
        }
        if (opts->same_site) {
            snprintf(buf, sizeof(buf), "; SameSite=%s", opts->same_site);
            cwist_sstring_append(cookie, buf);
        }
    }

    cwist_error_t err = cwist_http_header_add(&res->headers, "Set-Cookie", cookie->data);
    cwist_sstring_destroy(cookie);
    return err.error.err_i16 == 0 ? 0 : -1;
}

int cwist_cookie_delete(cwist_http_response *res, const char *name) {
    if (!res || !name) return -1;

    cwist_sstring *cookie = cwist_sstring_create();
    if (!cookie) return -1;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s=; Path=/; Max-Age=0", name);
    cwist_sstring_append(cookie, buf);
    cwist_error_t err = cwist_http_header_add(&res->headers, "Set-Cookie", cookie->data);
    cwist_sstring_destroy(cookie);
    return err.error.err_i16 == 0 ? 0 : -1;
}
