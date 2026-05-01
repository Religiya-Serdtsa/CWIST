/**
 * @file multipart.c
 * @brief RFC 7578 multipart/form-data parser — wrapper around multipart-parser-c.
 */

#include <cwist/net/http/multipart.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "multipart_parser.h"

typedef struct {
    char header_field[256];
    size_t header_field_len;
    char header_value[1024];
    size_t header_value_len;

    char name[256];
    char filename[256];
    char content_type[256];

    char *data;
    size_t data_len;
    size_t data_cap;

    cwist_multipart_result *result;
} mp_parse_ctx;

static int mp_on_header_field(multipart_parser *p, const char *at, size_t len) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    if (ctx->header_field_len + len < sizeof(ctx->header_field)) {
        memcpy(ctx->header_field + ctx->header_field_len, at, len);
        ctx->header_field_len += len;
    }
    return 0;
}

static int mp_on_header_value(multipart_parser *p, const char *at, size_t len) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    if (ctx->header_value_len + len < sizeof(ctx->header_value)) {
        memcpy(ctx->header_value + ctx->header_value_len, at, len);
        ctx->header_value_len += len;
    }
    return 0;
}

static void mp_parse_headers(mp_parse_ctx *ctx) {
    if (ctx->header_field_len >= sizeof(ctx->header_field))
        ctx->header_field_len = sizeof(ctx->header_field) - 1;
    ctx->header_field[ctx->header_field_len] = '\0';

    if (ctx->header_value_len >= sizeof(ctx->header_value))
        ctx->header_value_len = sizeof(ctx->header_value) - 1;
    ctx->header_value[ctx->header_value_len] = '\0';

    if (strcasecmp(ctx->header_field, "Content-Disposition") == 0) {
        const char *name_tag = strstr(ctx->header_value, "name=\"");
        if (name_tag) {
            name_tag += 6;
            const char *name_end = strchr(name_tag, '"');
            if (name_end) {
                size_t nlen = (size_t)(name_end - name_tag);
                if (nlen < sizeof(ctx->name)) {
                    memcpy(ctx->name, name_tag, nlen);
                    ctx->name[nlen] = '\0';
                }
            }
        }
        const char *filename_tag = strstr(ctx->header_value, "filename=\"");
        if (filename_tag) {
            filename_tag += 10;
            const char *filename_end = strchr(filename_tag, '"');
            if (filename_end) {
                size_t flen = (size_t)(filename_end - filename_tag);
                if (flen < sizeof(ctx->filename)) {
                    memcpy(ctx->filename, filename_tag, flen);
                    ctx->filename[flen] = '\0';
                }
            }
        }
    } else if (strcasecmp(ctx->header_field, "Content-Type") == 0) {
        size_t vlen = strlen(ctx->header_value);
        if (vlen < sizeof(ctx->content_type))
            strcpy(ctx->content_type, ctx->header_value);
    }

    ctx->header_field_len = 0;
    ctx->header_value_len = 0;
}

static int mp_on_headers_complete(multipart_parser *p) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    mp_parse_headers(ctx);
    return 0;
}

static int mp_on_part_data_begin(multipart_parser *p) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    ctx->header_field_len = 0;
    ctx->header_value_len = 0;
    ctx->name[0] = '\0';
    ctx->filename[0] = '\0';
    ctx->content_type[0] = '\0';
    if (ctx->data) {
        cwist_free(ctx->data);
        ctx->data = NULL;
    }
    ctx->data_len = 0;
    ctx->data_cap = 0;
    return 0;
}

static int mp_on_part_data(multipart_parser *p, const char *at, size_t len) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    if (len == 0) return 0;
    if (ctx->data_len + len > ctx->data_cap) {
        size_t new_cap = (ctx->data_len + len) * 2;
        if (new_cap < 256) new_cap = 256;
        char *new_data = (char *)cwist_alloc(new_cap);
        if (!new_data) return 1;
        if (ctx->data) {
            memcpy(new_data, ctx->data, ctx->data_len);
            cwist_free(ctx->data);
        }
        ctx->data = new_data;
        ctx->data_cap = new_cap;
    }
    memcpy(ctx->data + ctx->data_len, at, len);
    ctx->data_len += len;
    return 0;
}

static int mp_on_part_data_end(multipart_parser *p) {
    mp_parse_ctx *ctx = (mp_parse_ctx *)multipart_parser_get_data(p);
    cwist_multipart_field *field = (cwist_multipart_field *)cwist_alloc(sizeof(cwist_multipart_field));
    if (!field) return 1;
    memset(field, 0, sizeof(*field));

    if (ctx->name[0]) {
        field->name = (char *)cwist_alloc(strlen(ctx->name) + 1);
        if (field->name) strcpy(field->name, ctx->name);
    }
    if (ctx->filename[0]) {
        field->filename = (char *)cwist_alloc(strlen(ctx->filename) + 1);
        if (field->filename) strcpy(field->filename, ctx->filename);
    }
    if (ctx->content_type[0]) {
        field->content_type = (char *)cwist_alloc(strlen(ctx->content_type) + 1);
        if (field->content_type) strcpy(field->content_type, ctx->content_type);
    }
    field->data = ctx->data;
    field->data_len = ctx->data_len;
    field->next = ctx->result->fields;
    ctx->result->fields = field;

    ctx->data = NULL;
    ctx->data_len = 0;
    ctx->data_cap = 0;
    return 0;
}

cwist_multipart_result *cwist_multipart_parse(const char *body, size_t body_len, const char *boundary) {
    if (!body || body_len == 0 || !boundary) return NULL;

    cwist_multipart_result *result = (cwist_multipart_result *)cwist_alloc(sizeof(cwist_multipart_result));
    if (!result) return NULL;
    result->fields = NULL;

    mp_parse_ctx ctx = {0};
    ctx.result = result;

    multipart_parser_settings settings = {
        .on_header_field    = mp_on_header_field,
        .on_header_value    = mp_on_header_value,
        .on_headers_complete= mp_on_headers_complete,
        .on_part_data_begin = mp_on_part_data_begin,
        .on_part_data       = mp_on_part_data,
        .on_part_data_end   = mp_on_part_data_end,
    };

    multipart_parser *parser = multipart_parser_init(boundary, &settings);
    if (!parser) {
        cwist_free(result);
        return NULL;
    }
    multipart_parser_set_data(parser, &ctx);
    multipart_parser_execute(parser, body, body_len);
    multipart_parser_free(parser);

    return result;
}

void cwist_multipart_result_destroy(cwist_multipart_result *result) {
    if (!result) return;
    cwist_multipart_field *curr = result->fields;
    while (curr) {
        cwist_multipart_field *next = curr->next;
        cwist_free(curr->name);
        cwist_free(curr->filename);
        cwist_free(curr->content_type);
        cwist_free(curr->data);
        cwist_free(curr);
        curr = next;
    }
    cwist_free(result);
}

char *cwist_multipart_extract_boundary(const char *content_type) {
    if (!content_type) return NULL;
    const char *p = content_type;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (strncasecmp(p, "boundary", 8) == 0) {
            p += 8;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '=') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                const char *start = p;
                if (*p == '"') {
                    start = ++p;
                    while (*p && *p != '"') p++;
                } else {
                    while (*p && *p != ';' && !isspace((unsigned char)*p)) p++;
                }
                size_t len = (size_t)(p - start);
                char *boundary = (char *)cwist_alloc(len + 1);
                if (boundary) {
                    memcpy(boundary, start, len);
                    boundary[len] = '\0';
                }
                return boundary;
            }
        }
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return NULL;
}
