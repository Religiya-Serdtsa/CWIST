/** @file graphql.h @brief Small, bounded GraphQL query execution API. */
#ifndef CWIST_NET_GRAPHQL_H
#define CWIST_NET_GRAPHQL_H

#include <cwist/net/http/http.h>
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_graphql_schema cwist_graphql_schema_t;
#define CWIST_GRAPHQL_MAX_QUERY_SIZE (16U * 1024U)
#define CWIST_GRAPHQL_MAX_TOP_LEVEL_FIELDS 128U
/** Return a newly allocated cJSON value; CWIST takes ownership on success. */
typedef cJSON *(*cwist_graphql_resolver_fn)(const cJSON *variables, void *ctx);

cwist_graphql_schema_t *cwist_graphql_schema_create(void);
void cwist_graphql_schema_destroy(cwist_graphql_schema_t *schema);
/** Register or replace a top-level Query field. Field names are ASCII GraphQL names. */
bool cwist_graphql_add_query(cwist_graphql_schema_t *schema, const char *field,
                             cwist_graphql_resolver_fn resolver, void *ctx);
/** Execute a JSON request body containing `query` and optional `variables`. */
cwist_error_t cwist_graphql_execute(cwist_graphql_schema_t *schema, const char *request_json,
                                    cwist_sstring **out_json);
/** HTTP handler adapter. Register it in a normal CWIST POST route. */
void cwist_graphql_serve(cwist_graphql_schema_t *schema, cwist_http_request *req,
                         cwist_http_response *res);

#ifdef __cplusplus
}
#endif
#endif
