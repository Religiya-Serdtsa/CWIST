/**
 * @file http_client.c
 * @brief libcurl-based HTTP/1.1 and HTTP/2 client for CWIST.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/http_client.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>
#include "curl_global.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Response accumulator                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t size;
    size_t cap;
} response_body_t;

typedef struct {
    cwist_http_header_node *headers;
    long status_code;
} response_headers_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    response_body_t *body = (response_body_t *)userp;
    size_t need = body->size + total;
    if (need > body->cap) {
        size_t new_cap = body->cap ? body->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        char *tmp = realloc(body->data, new_cap);
        if (!tmp) return 0;
        body->data = tmp;
        body->cap = new_cap;
    }
    memcpy(body->data + body->size, contents, total);
    body->size += total;
    return total;
}

static size_t header_callback(char *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    response_headers_t *rh = (response_headers_t *)userp;

    /* libcurl sends headers as "Name: value\r\n" or "HTTP/1.1 200 OK\r\n" */
    if (total > 5 && strncasecmp(ptr, "HTTP/", 5) == 0) {
        /* Status line: parse status code */
        char *p = ptr;
        while (*p && !isspace((unsigned char)*p)) p++;
        while (*p && isspace((unsigned char)*p)) p++;
        rh->status_code = strtol(p, NULL, 10);
        return total;
    }

    /* Find colon separator */
    char *colon = memchr(ptr, ':', total);
    if (!colon) return total;

    size_t name_len = (size_t)(colon - ptr);
    if (name_len == 0) return total;

    char *value = colon + 1;
    size_t value_len = total - name_len - 1;

    /* Trim leading spaces in value */
    while (value_len > 0 && isspace((unsigned char)*value)) {
        value++;
        value_len--;
    }
    /* Trim trailing whitespace (\r\n) */
    while (value_len > 0 && isspace((unsigned char)value[value_len - 1])) {
        value_len--;
    }

    if (name_len > 0 && value_len > 0) {
        char *name = cwist_alloc(name_len + 1);
        if (!name) return total;
        memcpy(name, ptr, name_len);
        name[name_len] = '\0';

        char *val = cwist_alloc(value_len + 1);
        if (!val) {
            cwist_free(name);
            return total;
        }
        memcpy(val, value, value_len);
        val[value_len] = '\0';

        cwist_http_header_node *node = cwist_alloc(sizeof(*node));
        if (!node) {
            cwist_free(name);
            cwist_free(val);
            return total;
        }
        node->key = cwist_sstring_create();
        node->value = cwist_sstring_create();
        if (!node->key || !node->value) {
            cwist_sstring_destroy(node->key);
            cwist_sstring_destroy(node->value);
            cwist_free(node);
            cwist_free(name);
            cwist_free(val);
            return total;
        }
        cwist_sstring_assign(node->key, name);
        cwist_sstring_assign(node->value, val);
        node->next = rh->headers;
        rh->headers = node;

        cwist_free(name);
        cwist_free(val);
    }

    return total;
}

/* ------------------------------------------------------------------ */
/* Client handle                                                      */
/* ------------------------------------------------------------------ */

struct cwist_http_client {
    CURL *curl;
    int follow_redirects;
    int timeout_ms;
    char *ca_bundle;
    int altsvc_enabled;
    char *altsvc_db;
};

cwist_http_client *cwist_http_client_create(void) {
    cwist_curl_global_acquire();

    cwist_http_client *client = cwist_alloc(sizeof(*client));
    if (!client) {
        cwist_curl_global_release();
        return NULL;
    }

    client->curl = curl_easy_init();
    if (!client->curl) {
        cwist_free(client);
        cwist_curl_global_release();
        return NULL;
    }

    client->follow_redirects = 1;
    client->timeout_ms = 30000;
    client->ca_bundle = NULL;
    client->altsvc_enabled = 0;
    client->altsvc_db = NULL;

    curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(client->curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(client->curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(client->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(client->curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(client->curl, CURLOPT_USERAGENT, "CWIST-HTTP-Client/1.0");

    return client;
}

void cwist_http_client_destroy(cwist_http_client *client) {
    if (!client) return;
    if (client->curl) {
        curl_easy_cleanup(client->curl);
    }
    cwist_free(client->ca_bundle);
    cwist_free(client->altsvc_db);
    cwist_free(client);
    cwist_curl_global_release();
}

void cwist_http_client_set_follow_redirects(cwist_http_client *client, int follow) {
    if (!client) return;
    client->follow_redirects = follow;
    curl_easy_setopt(client->curl, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
}

void cwist_http_client_set_timeout_ms(cwist_http_client *client, int timeout_ms) {
    if (!client) return;
    client->timeout_ms = timeout_ms;
    curl_easy_setopt(client->curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
}

void cwist_http_client_set_ca_bundle(cwist_http_client *client, const char *path) {
    if (!client) return;
    cwist_free(client->ca_bundle);
    client->ca_bundle = path ? strdup(path) : NULL;
    curl_easy_setopt(client->curl, CURLOPT_CAINFO, client->ca_bundle);
}

void cwist_http_client_enable_altsvc(cwist_http_client *client, int enabled) {
    if (!client) return;
    client->altsvc_enabled = enabled;
    curl_easy_setopt(client->curl, CURLOPT_ALTSVC_CTRL,
                      enabled ? (long)(CURLALTSVC_H1 | CURLALTSVC_H2 | CURLALTSVC_H3) : 0L);
}

void cwist_http_client_set_altsvc_db(cwist_http_client *client, const char *path) {
    if (!client) return;
    cwist_free(client->altsvc_db);
    client->altsvc_db = path ? strdup(path) : NULL;
    curl_easy_setopt(client->curl, CURLOPT_ALTSVC, client->altsvc_db);
}

/* ------------------------------------------------------------------ */
/* Request execution                                                  */
/* ------------------------------------------------------------------ */

cwist_error_t cwist_http_client_request(cwist_http_client *client,
                                        const char *url,
                                        cwist_http_method_t method,
                                        cwist_http_header_node *headers,
                                        const char *body,
                                        size_t body_len,
                                        cwist_http_response **out_response) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!client || !client->curl || !url || !out_response) {
        err.error.err_i16 = -1;
        return err;
    }

    *out_response = NULL;

    CURL *curl = client->curl;

    /* Reset handle for reuse */
    curl_easy_reset(curl);

    /* Re-apply persistent settings after reset */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, client->follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)client->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CWIST-HTTP-Client/1.0");
    if (client->ca_bundle) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, client->ca_bundle);
    }
    if (client->altsvc_enabled) {
        curl_easy_setopt(curl, CURLOPT_ALTSVC_CTRL,
                         (long)(CURLALTSVC_H1 | CURLALTSVC_H2 | CURLALTSVC_H3));
        curl_easy_setopt(curl, CURLOPT_ALTSVC, client->altsvc_db);
    }

    /* URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* Method */
    const char *method_str = cwist_http_method_to_string(method);
    if (strcmp(method_str, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method_str, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (strcmp(method_str, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    } else if (strcmp(method_str, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (strcmp(method_str, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    } else if (strcmp(method_str, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (strcmp(method_str, "OPTIONS") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "OPTIONS");
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method_str);
    }

    /* Request headers */
    struct curl_slist *curl_headers = NULL;
    cwist_http_header_node *node = headers;
    while (node) {
        if (node->key && node->key->data && node->value && node->value->data) {
            char header_line[4096];
            int n = snprintf(header_line, sizeof(header_line), "%s: %s",
                             node->key->data, node->value->data);
            if (n > 0 && (size_t)n < sizeof(header_line)) {
                curl_headers = curl_slist_append(curl_headers, header_line);
            }
        }
        node = node->next;
    }
    if (curl_headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    }

    /* Request body */
    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
    }

    /* Response accumulators */
    response_body_t resp_body = {0};
    response_headers_t resp_hdrs = {0};

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_hdrs);

    /* Prefer HTTP/2, allow downgrade to HTTP/1.1 */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

    /* Perform */
    CURLcode res = curl_easy_perform(curl);

    /* Clean up request headers */
    if (curl_headers) {
        curl_slist_free_all(curl_headers);
    }

    if (res != CURLE_OK) {
        free(resp_body.data);
        cwist_http_header_free_all(resp_hdrs.headers);
        err.error.err_i16 = -1;
        return err;
    }

    /* Build response object */
    cwist_http_response *response = cwist_http_response_create();
    if (!response) {
        free(resp_body.data);
        cwist_http_header_free_all(resp_hdrs.headers);
        err.error.err_i16 = -1;
        return err;
    }

    response->status_code = (cwist_http_status_t)resp_hdrs.status_code;
    if (resp_body.data && resp_body.size > 0) {
        cwist_sstring_assign_len(response->body, resp_body.data, resp_body.size);
    }

    /* Transfer collected headers */
    response->headers = resp_hdrs.headers;

    /* Populate status text based on code */
    switch (response->status_code) {
        case CWIST_HTTP_OK:                  cwist_sstring_assign(response->status_text, "OK"); break;
        case CWIST_HTTP_CREATED:             cwist_sstring_assign(response->status_text, "Created"); break;
        case CWIST_HTTP_NO_CONTENT:          cwist_sstring_assign(response->status_text, "No Content"); break;
        case CWIST_HTTP_BAD_REQUEST:         cwist_sstring_assign(response->status_text, "Bad Request"); break;
        case CWIST_HTTP_UNAUTHORIZED:        cwist_sstring_assign(response->status_text, "Unauthorized"); break;
        case CWIST_HTTP_FORBIDDEN:           cwist_sstring_assign(response->status_text, "Forbidden"); break;
        case CWIST_HTTP_NOT_FOUND:           cwist_sstring_assign(response->status_text, "Not Found"); break;
        case CWIST_HTTP_INTERNAL_ERROR:      cwist_sstring_assign(response->status_text, "Internal Server Error"); break;
        case CWIST_HTTP_NOT_IMPLEMENTED:     cwist_sstring_assign(response->status_text, "Not Implemented"); break;
        case CWIST_HTTP_SERVICE_UNAVAILABLE: cwist_sstring_assign(response->status_text, "Service Unavailable"); break;
        default:                             cwist_sstring_assign(response->status_text, ""); break;
    }

    free(resp_body.data);
    *out_response = response;
    err.error.err_i16 = 0;
    return err;
}
