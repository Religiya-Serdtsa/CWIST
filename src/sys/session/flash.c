#include <cwist/sys/session/flash.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>

void cwist_flash_set(cwist_http_request *req, const char *key, const char *value) {
    if (!req || !key || !req->flash) return;
    cwist_query_map_set(req->flash, key, value ? value : "");
}

const char *cwist_flash_peek(cwist_http_request *req, const char *key) {
    if (!req || !key || !req->flash) return NULL;
    return cwist_query_map_get(req->flash, key);
}

const char *cwist_flash_get(cwist_http_request *req, const char *key) {
    if (!req || !key || !req->flash) return NULL;
    const char *val = cwist_query_map_get(req->flash, key);
    if (!val) return NULL;
    // Remove from map by clearing the bucket entry
    size_t idx = 0;
    // Re-hash to find bucket (djb2 hash used in query.c)
    size_t hash = 5381;
    int c;
    const char *k = key;
    while ((c = *k++)) hash = ((hash << 5) + hash) + c;
    idx = hash % req->flash->size;

    cwist_query_bucket **prev = &req->flash->buckets[idx];
    cwist_query_bucket *curr = *prev;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            *prev = curr->next;
            cwist_free(curr->key);
            cwist_free(curr->value);
            cwist_free(curr);
            return val;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    return val;
}

char *cwist_flash_pop_all_json(cwist_http_request *req) {
    if (!req || !req->flash) return NULL;
    int count = 0;
    for (size_t i = 0; i < req->flash->size; i++) {
        cwist_query_bucket *b = req->flash->buckets[i];
        while (b) {
            count++;
            b = b->next;
        }
    }
    if (count == 0) return NULL;

    size_t cap = 256;
    char *buf = (char *)cwist_alloc(cap);
    size_t len = 0;
    buf[len++] = '{';
    int first = 1;
    for (size_t i = 0; i < req->flash->size; i++) {
        cwist_query_bucket *b = req->flash->buckets[i];
        while (b) {
            if (!first) {
                if (len + 2 >= cap) { cap *= 2; buf = cwist_realloc(buf, cap); }
                buf[len++] = ',';
            }
            first = 0;
            size_t key_escaped_len = strlen(b->key) * 2 + 3;
            size_t val_escaped_len = strlen(b->value) * 2 + 3;
            size_t needed = key_escaped_len + val_escaped_len + 4;
            if (len + needed >= cap) {
                while (len + needed >= cap) cap *= 2;
                buf = cwist_realloc(buf, cap);
            }
            buf[len++] = '"';
            for (char *p = b->key; *p; p++) {
                if (*p == '"' || *p == '\\') buf[len++] = '\\';
                buf[len++] = *p;
            }
            buf[len++] = '"';
            buf[len++] = ':';
            buf[len++] = '"';
            for (char *p = b->value; *p; p++) {
                if (*p == '"' || *p == '\\') buf[len++] = '\\';
                buf[len++] = *p;
            }
            buf[len++] = '"';
            b = b->next;
        }
    }
    if (len + 2 >= cap) { cap *= 2; buf = cwist_realloc(buf, cap); }
    buf[len++] = '}';
    buf[len] = '\0';
    // Clear all flash entries
    for (size_t i = 0; i < req->flash->size; i++) {
        cwist_query_bucket *b = req->flash->buckets[i];
        while (b) {
            cwist_query_bucket *next = b->next;
            cwist_free(b->key);
            cwist_free(b->value);
            cwist_free(b);
            b = next;
        }
        req->flash->buckets[i] = NULL;
    }
    return buf;
}
