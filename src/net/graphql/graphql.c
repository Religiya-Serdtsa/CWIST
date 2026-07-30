#include <cwist/net/graphql/graphql.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

typedef struct graphql_field { char *name; cwist_graphql_resolver_fn fn; void *ctx; struct graphql_field *next; } graphql_field;
struct cwist_graphql_schema { graphql_field *queries; };

static cwist_error_t gql_error(int code) { return (cwist_error_t){ .errtype = CWIST_ERR_INT16, .error.err_i16 = code }; }
static bool gql_name(const char *s) { if (!s || !(*s == '_' || isalpha((unsigned char)*s))) return false; for (++s; *s; ++s) if (!(*s == '_' || isalnum((unsigned char)*s))) return false; return true; }
static void gql_set_error(cJSON *errors, const char *message) { cJSON *e = cJSON_CreateObject(); if (e) { cJSON_AddStringToObject(e, "message", message); cJSON_AddItemToArray(errors, e); } }

cwist_graphql_schema_t *cwist_graphql_schema_create(void) { cwist_graphql_schema_t *schema = cwist_alloc(sizeof(*schema)); if (schema) schema->queries = NULL; return schema; }
void cwist_graphql_schema_destroy(cwist_graphql_schema_t *schema) { if (!schema) return; graphql_field *f = schema->queries; while (f) { graphql_field *n = f->next; cwist_free(f->name); cwist_free(f); f = n; } cwist_free(schema); }
bool cwist_graphql_add_query(cwist_graphql_schema_t *schema, const char *field, cwist_graphql_resolver_fn resolver, void *ctx) {
    if (!schema || !gql_name(field) || !resolver) return false;
    for (graphql_field *f = schema->queries; f; f = f->next) if (!strcmp(f->name, field)) { f->fn = resolver; f->ctx = ctx; return true; }
    graphql_field *f = cwist_alloc(sizeof(*f)); if (!f) return false; f->name = cwist_strdup(field); if (!f->name) { cwist_free(f); return false; } f->fn = resolver; f->ctx = ctx; f->next = schema->queries; schema->queries = f; return true;
}
static graphql_field *gql_lookup(cwist_graphql_schema_t *schema, const char *name) { for (graphql_field *f = schema->queries; f; f = f->next) if (!strcmp(f->name, name)) return f; return NULL; }

cwist_error_t cwist_graphql_execute(cwist_graphql_schema_t *schema, const char *request_json, cwist_sstring **out_json) {
    if (!schema || !request_json || !out_json) return gql_error(-1);
    *out_json = NULL;
    cJSON *request = cJSON_Parse(request_json); cJSON *root = cJSON_CreateObject(); cJSON *data = cJSON_CreateObject(); cJSON *errors = cJSON_CreateArray();
    if (!request || !root || !data || !errors) { cJSON_Delete(request); cJSON_Delete(root); cJSON_Delete(data); cJSON_Delete(errors); return gql_error(-1); }
    cJSON_AddItemToObject(root, "data", data); cJSON_AddItemToObject(root, "errors", errors);
    cJSON *query = cJSON_GetObjectItemCaseSensitive(request, "query"); cJSON *variables = cJSON_GetObjectItemCaseSensitive(request, "variables");
    if (!cJSON_IsString(query) || !query->valuestring) gql_set_error(errors, "request must contain a string query");
    else if (strlen(query->valuestring) > CWIST_GRAPHQL_MAX_QUERY_SIZE) gql_set_error(errors, "query exceeds configured size limit");
    else {
        const char *p = strchr(query->valuestring, '{'); const char *end = p ? strrchr(p + 1, '}') : NULL;
        if (!p || !end || end <= p + 1) gql_set_error(errors, "only a non-empty top-level query selection is supported");
        else for (size_t fields = 0; ++p < end;) {
            while (p < end && (isspace((unsigned char)*p) || *p == ',')) ++p;
            if (p >= end) break;
            if (++fields > CWIST_GRAPHQL_MAX_TOP_LEVEL_FIELDS) { gql_set_error(errors, "too many top-level query fields"); break; }
            const char *start = p;
            if (!(*p == '_' || isalpha((unsigned char)*p))) { gql_set_error(errors, "invalid query field"); break; }
            while (p < end && (*p == '_' || isalnum((unsigned char)*p))) ++p;
            size_t len = (size_t)(p - start); if (len > 127) { gql_set_error(errors, "query field is too long"); break; }
            char name[128]; memcpy(name, start, len); name[len] = '\0'; graphql_field *f = gql_lookup(schema, name);
            if (!f) { gql_set_error(errors, "unknown query field"); continue; }
            cJSON *value = f->fn(cJSON_IsObject(variables) ? variables : NULL, f->ctx);
            if (!value) { gql_set_error(errors, "resolver failed"); continue; } cJSON_AddItemToObject(data, name, value);
        }
    }
    if (!cJSON_GetArraySize(errors)) cJSON_DeleteItemFromObjectCaseSensitive(root, "errors");
    char *printed = cJSON_PrintUnformatted(root); cJSON_Delete(request); cJSON_Delete(root);
    if (!printed) return gql_error(-1);
    *out_json = cwist_sstring_create();
    if (!*out_json) { free(printed); return gql_error(-1); }
    cwist_sstring_assign(*out_json, printed); free(printed); return gql_error(0);
}
void cwist_graphql_serve(cwist_graphql_schema_t *schema, cwist_http_request *req, cwist_http_response *res) {
    if (!schema || !req || !res || req->method != CWIST_HTTP_POST) { if (res) res->status_code = CWIST_HTTP_BAD_REQUEST; return; }
    cwist_sstring *json = NULL; if (cwist_graphql_execute(schema, req->body ? req->body->data : NULL, &json).error.err_i16) { res->status_code = CWIST_HTTP_BAD_REQUEST; cwist_sstring_assign(res->body, "{\"errors\":[{\"message\":\"invalid GraphQL request\"}]}"); }
    else { cwist_sstring_assign_len(res->body, json->data, json->size); cwist_sstring_destroy(json); }
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}
