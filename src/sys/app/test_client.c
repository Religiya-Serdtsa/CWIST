/** @file test_client.c
 * @brief test_client.c interface.
 */
#include <cwist/sys/app/test_client.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static char *clone_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)cwist_alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void free_cookie(cwist_test_client_cookie *c) {
    if (!c) return;
    cwist_free(c->name);
    cwist_free(c->value);
    cwist_free(c->path);
    cwist_free(c);
}

static cwist_test_client_cookie *find_cookie(cwist_test_client_cookie *head, const char *name) {
    for (; head; head = head->next) {
        if (strcmp(head->name, name) == 0) return head;
    }
    return NULL;
}

static void cookie_jar_apply(cwist_test_client *client, cwist_http_request *req,
                              const cwist_test_client_kv *adhoc, size_t adhoc_count) {
    if (!client) return;
    size_t jar_count = 0;
    for (cwist_test_client_cookie *c = client->cookies; c; c = c->next) jar_count++;
    if (jar_count == 0 && adhoc_count == 0) return;

    size_t buf_len = 0;
    for (cwist_test_client_cookie *c = client->cookies; c; c = c->next)
        buf_len += strlen(c->name) + 1 + strlen(c->value) + 2;
    for (size_t i = 0; i < adhoc_count; i++)
        buf_len += strlen(adhoc[i].key) + 1 + strlen(adhoc[i].value) + 2;

    char *cookie_header = (char *)cwist_alloc(buf_len + 1);
    if (!cookie_header) return;
    cookie_header[0] = '\0';

    size_t pos = 0;
    for (cwist_test_client_cookie *c = client->cookies; c; c = c->next) {
        if (pos > 0) cookie_header[pos++] = ';';
        if (pos > 0) cookie_header[pos++] = ' ';
        pos += (size_t)snprintf(cookie_header + pos, buf_len + 1 - pos, "%s=%s", c->name, c->value);
    }
    for (size_t i = 0; i < adhoc_count; i++) {
        if (pos > 0) cookie_header[pos++] = ';';
        if (pos > 0) cookie_header[pos++] = ' ';
        pos += (size_t)snprintf(cookie_header + pos, buf_len + 1 - pos, "%s=%s",
                                 adhoc[i].key, adhoc[i].value);
    }
    cwist_http_header_add(&req->headers, "Cookie", cookie_header);
    cwist_free(cookie_header);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void cookie_jar_update(cwist_test_client *client, cwist_http_response *res) {
    if (!client || !res) return;
    for (cwist_http_header_node *h = res->headers; h; h = h->next) {
        if (!h->key || !h->value || strcasecmp(h->key->data, "Set-Cookie") != 0) continue;
        char *buf = clone_str(h->value->data);
        if (!buf) continue;
        char *name_val = strtok(buf, ";");
        if (!name_val) { cwist_free(buf); continue; }
        name_val = trim(name_val);
        char *eq = strchr(name_val, '=');
        if (!eq) { cwist_free(buf); continue; }
        *eq = '\0';
        char *name = clone_str(trim(name_val));
        char *value = clone_str(trim(eq + 1));
        char *path = NULL;
        char *rest = strtok(NULL, ";");
        while (rest) {
            rest = trim(rest);
            if (strncasecmp(rest, "Path=", 5) == 0) {
                path = clone_str(trim(rest + 5));
                break;
            }
            rest = strtok(NULL, ";");
        }
        cwist_test_client_cookie *c = find_cookie(client->cookies, name);
        if (c) {
            cwist_free(c->value);
            c->value = value;
            if (path) { cwist_free(c->path); c->path = path; }
            else { cwist_free(path); }
            cwist_free(name);
        } else {
            c = (cwist_test_client_cookie *)cwist_alloc(sizeof(*c));
            if (c) {
                c->name = name;
                c->value = value;
                c->path = path;
                c->next = client->cookies;
                client->cookies = c;
            } else {
                cwist_free(name);
                cwist_free(value);
                cwist_free(path);
            }
        }
        cwist_free(buf);
    }
}

cwist_test_client *cwist_test_client_create(cwist_app *app) {
    if (!app) return NULL;
    cwist_test_client *client = (cwist_test_client *)cwist_alloc(sizeof(cwist_test_client));
    if (!client) return NULL;
    client->app = app;
    client->cookies = NULL;
    return client;
}

void cwist_test_client_destroy(cwist_test_client *client) {
    if (!client) return;
    cwist_test_client_clear_cookies(client);
    cwist_free(client);
}

static cwist_http_response *do_request(cwist_test_client *client, cwist_http_method_t method,
                                        const char *path, const cwist_test_client_request_options *opts) {
    if (!client || !client->app || !path) return NULL;
    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    req->method = method;

    const char *query = opts && opts->query_string ? opts->query_string : NULL;
    const char *hash = strchr(path, '?');
    if (hash) {
        size_t path_len = (size_t)(hash - path);
        char *path_only = (char *)cwist_alloc(path_len + 1);
        if (!path_only) { cwist_http_request_destroy(req); return NULL; }
        memcpy(path_only, path, path_len);
        path_only[path_len] = '\0';
        cwist_sstring_assign(req->path, path_only);
        cwist_free(path_only);
        query = hash + 1;
    } else {
        cwist_sstring_assign(req->path, (char *)path);
    }
    if (query) {
        cwist_sstring_assign(req->query, (char *)query);
        cwist_query_map_clear(req->query_params);
        cwist_query_map_parse(req->query_params, query);
    }

    const char *body = opts ? opts->body : NULL;
    const char *content_type = opts ? opts->content_type : NULL;

    if (body) cwist_sstring_assign(req->body, (char *)body);
    if (content_type) cwist_http_header_add(&req->headers, "Content-Type", content_type);

    if (opts && opts->headers) {
        for (size_t i = 0; i < opts->header_count; i++) {
            if (opts->headers[i].key && opts->headers[i].value)
                cwist_http_header_add(&req->headers, opts->headers[i].key, opts->headers[i].value);
        }
    }

    cookie_jar_apply(client, req, opts ? opts->cookies : NULL, opts ? opts->cookie_count : 0);

    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    cwist_app_dispatch(client->app, req, res);
    cookie_jar_update(client, res);
    cwist_http_request_destroy(req);
    return res;
}

cwist_http_response *cwist_test_client_request_ex(cwist_test_client *client,
                                                   cwist_http_method_t method,
                                                   const char *path,
                                                   const cwist_test_client_request_options *opts) {
    return do_request(client, method, path, opts);
}

cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path) {
    return do_request(client, CWIST_HTTP_GET, path, NULL);
}

cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path, const char *body) {
    cwist_test_client_request_options opts = {0};
    opts.body = body;
    return do_request(client, CWIST_HTTP_POST, path, &opts);
}

cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path, const char *json_body) {
    cwist_test_client_request_options opts = {0};
    opts.body = json_body;
    opts.content_type = "application/json";
    return do_request(client, CWIST_HTTP_POST, path, &opts);
}

cwist_http_response *cwist_test_client_put(cwist_test_client *client, const char *path, const char *body) {
    cwist_test_client_request_options opts = {0};
    opts.body = body;
    return do_request(client, CWIST_HTTP_PUT, path, &opts);
}

cwist_http_response *cwist_test_client_delete(cwist_test_client *client, const char *path) {
    return do_request(client, CWIST_HTTP_DELETE, path, NULL);
}

cwist_http_response *cwist_test_client_patch(cwist_test_client *client, const char *path, const char *body) {
    cwist_test_client_request_options opts = {0};
    opts.body = body;
    return do_request(client, CWIST_HTTP_PATCH, path, &opts);
}

cwist_http_response *cwist_test_client_post_multipart(cwist_test_client *client,
                                                       const char *path,
                                                       const char *field_name,
                                                       const char *file_name,
                                                       const char *content_type,
                                                       const char *data,
                                                       size_t data_len) {
    if (!client || !client->app || !path || !field_name || !file_name || !content_type || !data) return NULL;
    const char *boundary = "CWISTTestClientBoundary7MA4YWxkTrZu0gW";
    size_t boundary_len = strlen(boundary);

    size_t body_len = 2 + boundary_len + 2 +
                      38 + strlen(field_name) + 25 + strlen(file_name) + 16 + strlen(content_type) + 4 +
                      data_len +
                      2 + boundary_len + 2 + 2;
    char *body = (char *)cwist_alloc(body_len + 1);
    if (!body) return NULL;

    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, body_len + 1 - pos, "--%s\r\n", boundary);
    pos += (size_t)snprintf(body + pos, body_len + 1 - pos,
                            "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n",
                            field_name, file_name);
    pos += (size_t)snprintf(body + pos, body_len + 1 - pos, "Content-Type: %s\r\n\r\n", content_type);
    memcpy(body + pos, data, data_len);
    pos += data_len;
    pos += (size_t)snprintf(body + pos, body_len + 1 - pos, "\r\n--%s--\r\n", boundary);
    body[pos] = '\0';

    char ct[256];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", boundary);

    cwist_test_client_request_options opts = {0};
    opts.body = body;
    opts.content_type = ct;
    cwist_http_response *res = do_request(client, CWIST_HTTP_POST, path, &opts);
    cwist_free(body);
    return res;
}

void cwist_test_client_set_cookie(cwist_test_client *client,
                                   const char *name,
                                   const char *value,
                                   const char *path) {
    if (!client || !name) return;
    cwist_test_client_cookie *c = find_cookie(client->cookies, name);
    if (c) {
        cwist_free(c->value);
        c->value = value ? clone_str(value) : NULL;
        if (path) { cwist_free(c->path); c->path = clone_str(path); }
    } else {
        c = (cwist_test_client_cookie *)cwist_alloc(sizeof(*c));
        if (!c) return;
        c->name = clone_str(name);
        c->value = value ? clone_str(value) : NULL;
        c->path = path ? clone_str(path) : NULL;
        c->next = client->cookies;
        client->cookies = c;
    }
}

const char *cwist_test_client_get_cookie(cwist_test_client *client, const char *name) {
    if (!client || !name) return NULL;
    cwist_test_client_cookie *c = find_cookie(client->cookies, name);
    return c ? c->value : NULL;
}

void cwist_test_client_clear_cookies(cwist_test_client *client) {
    if (!client) return;
    cwist_test_client_cookie *c = client->cookies;
    while (c) {
        cwist_test_client_cookie *next = c->next;
        free_cookie(c);
        c = next;
    }
    client->cookies = NULL;
}
