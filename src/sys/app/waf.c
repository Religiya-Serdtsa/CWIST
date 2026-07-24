/** @file waf.c @brief Bounded, case-insensitive WAF-lite checks. */
#include <cwist/sys/app/waf.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>

static bool ascii_equal_ci(const char *input, size_t input_len, size_t pos, const char *needle) {
    for (size_t i = 0; needle[i]; ++i) {
        if (pos + i >= input_len) return false;
        unsigned char c = (unsigned char)input[pos + i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)needle[i]) return false;
    }
    return true;
}

bool cwist_waf_is_safe(const char *input, size_t length) {
    if (!input) return true;
    static const char *const signatures[] = {
        "<script", "</script", "javascript:", "vbscript:",
        "union select", "drop table", "insert into", "delete from",
        " or 1=1", " and 1=1", "--", "/*", "*/"
    };
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == 0 || (c < 0x20U && c != '\t' && c != '\n' && c != '\r')) return false;
        for (size_t rule = 0; rule < sizeof(signatures) / sizeof(signatures[0]); ++rule) {
            if (ascii_equal_ci(input, length, i, signatures[rule])) return false;
        }
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
