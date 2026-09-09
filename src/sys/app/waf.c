/** @file waf.c @brief Bounded, case-insensitive WAF-lite checks. */
#include <cwist/sys/app/waf.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>

static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Signature scanning runs a single Aho-Corasick pass over the input instead
 * of re-probing every rule at every byte: the automaton is built once from
 * the pattern set (dense 128-entry transitions cover the lowered ASCII
 * alphabet; failure edges are folded in at build time, so the hot loop is
 * one table index per byte) and matching cost stays O(input length)
 * regardless of how many signatures are registered. */
typedef struct {
    const char *text;
    size_t length;
} waf_signature;

static const waf_signature waf_signatures[] = {
    { "<script", 7 }, { "</script", 8 }, { "javascript:", 11 }, { "vbscript:", 9 },
    { "union select", 12 }, { "drop table", 10 }, { "insert into", 11 }, { "delete from", 11 },
    { " or 1=1", 7 }, { " and 1=1", 8 }, { "--", 2 }, { "/*", 2 }, { "*/", 2 }
};

#define WAF_MAX_PATTERNS 64
#define WAF_MAX_TEXT 4096
#define WAF_ALPHABET 128

static uint8_t waf_goto[WAF_MAX_TEXT][WAF_ALPHABET];
static uint16_t waf_fail[WAF_MAX_TEXT];
static bool waf_out[WAF_MAX_TEXT];
static uint16_t waf_states;
static pthread_once_t waf_once = PTHREAD_ONCE_INIT;

static void waf_build(void) {
    uint16_t queue[WAF_MAX_TEXT];
    waf_states = 1; /* state 0: root */

    for (size_t rule = 0; rule < sizeof(waf_signatures) / sizeof(waf_signatures[0]); ++rule) {
        uint16_t s = 0;
        for (size_t i = 0; i < waf_signatures[rule].length; ++i) {
            unsigned char c = (unsigned char)waf_signatures[rule].text[i];
            if (waf_goto[s][c] == 0) {
                if (waf_states > 255) return; /* uint8_t state space exhausted: fail open */
                waf_goto[s][c] = (uint8_t)waf_states++;
            }
            s = waf_goto[s][c];
        }
        waf_out[s] = true;
    }

    /* BFS failure links, then fold them into the goto table so the scan
     * loop never walks the fail chain per byte. */
    size_t qh = 0, qt = 0;
    for (unsigned c = 0; c < WAF_ALPHABET; ++c) {
        if (waf_goto[0][c]) queue[qt++] = waf_goto[0][c];
    }
    while (qh < qt) {
        uint16_t r = queue[qh++];
        if (waf_out[waf_fail[r]]) waf_out[r] = true;
        for (unsigned c = 0; c < WAF_ALPHABET; ++c) {
            uint8_t s = waf_goto[r][c];
            if (s) {
                queue[qt++] = s;
                uint16_t f = waf_fail[r];
                while (f && waf_goto[f][c] == 0) f = waf_fail[f];
                waf_fail[s] = waf_goto[f][c];
            } else {
                waf_goto[r][c] = waf_goto[waf_fail[r]][c];
            }
        }
    }
}

bool cwist_waf_is_safe(const char *input, size_t length) {
    if (!input) return true;
    pthread_once(&waf_once, waf_build);
    uint16_t s = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == 0 || (c < 0x20U && c != '\t' && c != '\n' && c != '\r')) return false;
        /* Bytes outside ASCII cannot be part of any signature; resetting
         * keeps matching identical to the old per-position compare. */
        if (c >= WAF_ALPHABET) {
            s = 0;
            continue;
        }
        s = waf_goto[s][ascii_lower(c)];
        if (waf_out[s]) return false;
    }
    return true;
}

char *cwist_sanitize_html(const char *input) {
    if (!input) return NULL;
    size_t length = strlen(input), extra = 0;
    for (size_t i = 0; i < length; ++i) {
        switch (input[i]) { case '&': extra += 4; break; case '<': case '>': extra += 3; break; case '"': extra += 5; break; case '\'': extra += 4; break; default: break; }
    }
    if (length > SIZE_MAX - extra - 1) return NULL;
    char *output = cwist_alloc(length + extra + 1);
    if (!output) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < length; ++i) {
        const char *replacement = NULL;
        switch (input[i]) { case '&': replacement = "&amp;"; break; case '<': replacement = "&lt;"; break; case '>': replacement = "&gt;"; break; case '"': replacement = "&quot;"; break; case '\'': replacement = "&#39;"; break; default: break; }
        if (replacement) { size_t n = strlen(replacement); memcpy(output + out, replacement, n); out += n; }
        else output[out++] = input[i];
    }
    output[out] = '\0';
    return output;
}

static bool waf_headers_safe(const cwist_http_header_node *header) {
    for (; header; header = header->next) {
        if ((header->key && !cwist_waf_is_safe(header->key->data, header->key->size)) ||
            (header->value && !cwist_waf_is_safe(header->value->data, header->value->size))) return false;
    }
    return true;
}

static void waf_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    bool safe = req &&
        (!req->path || cwist_waf_is_safe(req->path->data, req->path->size)) &&
        (!req->query || cwist_waf_is_safe(req->query->data, req->query->size)) &&
        (!req->body || cwist_waf_is_safe(req->body->data, req->body->size)) &&
        waf_headers_safe(req ? req->headers : NULL);
    if (!safe) { res->status_code = CWIST_HTTP_BAD_REQUEST; cwist_sstring_assign(res->body, "Request rejected by WAF"); return; }
    next(req, res);
}

cwist_middleware_func cwist_mw_waf_lite(void) { return waf_handler; }
