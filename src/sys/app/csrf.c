/**
 * @file csrf.c
 * @brief CSRF protection via double-submit cookie pattern.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/csrf.h>
#include <cwist/net/http/cookie.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define CWIST_CSRF_COOKIE_NAME "csrf_token"
#define CWIST_CSRF_TOKEN_LEN 32

static int is_safe_method(cwist_http_method_t method) {
    return method == CWIST_HTTP_GET ||
           method == CWIST_HTTP_HEAD ||
           method == CWIST_HTTP_OPTIONS;
}

static char *generate_token(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return NULL;
    unsigned char buf[CWIST_CSRF_TOKEN_LEN];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != (ssize_t)sizeof(buf)) return NULL;

    static const char hex[] = "0123456789abcdef";
    char *token = cwist_alloc(sizeof(buf) * 2 + 1);
    if (!token) return NULL;
    for (size_t i = 0; i < sizeof(buf); i++) {
        token[i * 2]     = hex[buf[i] >> 4];
        token[i * 2 + 1] = hex[buf[i] & 0x0F];
    }
    token[sizeof(buf) * 2] = '\0';
    return token;
}

static const char *get_submitted_token(cwist_http_request *req) {
    const char *header = cwist_http_header_get(req->headers, "X-CSRF-Token");
    if (header) return header;

    if (req->query_params) {
        const char *q = cwist_query_map_get(req->query_params, "csrf_token");
        if (q) return q;
    }

    if (req->body && req->body->data && req->body->size > 0) {
        /* Simple form parsing: look for _csrf=<value> in body.
         * This is enough for application/x-www-form-urlencoded bodies. */
        const char *body = req->body->data;
        const char *key = "_csrf=";
        const char *p = strstr(body, key);
        if (p) {
            static char token[128];
            p += strlen(key);
            size_t i = 0;
            while (i < sizeof(token) - 1 && p[i] && p[i] != '&') {
                token[i] = p[i];
                i++;
            }
            token[i] = '\0';
            return token;
        }
    }
    return NULL;
}

static void issue_cookie(cwist_http_response *res, const char *token) {
    cwist_cookie_options opts = {0};
    opts.path = "/";
    opts.max_age_seconds = 86400 * 30;
    opts.http_only = false; /* must be readable by JS for double-submit */
    opts.same_site = "Strict";
    cwist_cookie_set(res, CWIST_CSRF_COOKIE_NAME, token, &opts);
}

static void csrf_handler(cwist_http_request *req,
                         cwist_http_response *res,
                         cwist_handler_func next) {
    cwist_query_map *cookies = cwist_query_map_create();
    const char *cookie_token = NULL;
    if (cookies) {
        const char *cookie_header = cwist_http_header_get(req->headers, "Cookie");
        cwist_cookie_parse(cookies, cookie_header);
        cookie_token = cwist_cookie_get(cookies, CWIST_CSRF_COOKIE_NAME);
    }

    if (!cookie_token) {
        char *new_token = generate_token();
        if (!new_token) {
            cwist_query_map_destroy(cookies);
            res->status_code = CWIST_HTTP_INTERNAL_ERROR;
            return;
        }
        issue_cookie(res, new_token);
        req->csrf_token = new_token;
        if (is_safe_method(req->method)) {
            cwist_query_map_destroy(cookies);
            next(req, res);
            return;
        }
        /* Unsafe request without pre-existing cookie: reject. */
        cwist_query_map_destroy(cookies);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "CSRF token missing");
        return;
    }

    /* Use existing cookie token as the request token. */
    req->csrf_token = cwist_strdup(cookie_token);

    if (is_safe_method(req->method)) {
        cwist_query_map_destroy(cookies);
        next(req, res);
        return;
    }

    const char *submitted = get_submitted_token(req);
    if (!submitted || strcmp(submitted, cookie_token) != 0) {
        cwist_query_map_destroy(cookies);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "CSRF token invalid");
        return;
    }

    cwist_query_map_destroy(cookies);
    next(req, res);
}

cwist_middleware_func cwist_mw_csrf(cwist_app *app) {
    CWIST_UNUSED(app);
    return csrf_handler;
}

const char *cwist_csrf_token(cwist_http_request *req) {
    if (!req) return NULL;
    return req->csrf_token;
}
