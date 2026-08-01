/** @file graphql.h @brief Full-featured, production-ready GraphQL query & mutation execution engine. */
#ifndef CWIST_NET_GRAPHQL_H
#define CWIST_NET_GRAPHQL_H

#include <cwist/net/http/http.h>
#include <cjson/cJSON.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_graphql_schema cwist_graphql_schema_t;
#define CWIST_GRAPHQL_MAX_QUERY_SIZE (64U * 1024U)
#define CWIST_GRAPHQL_MAX_FIELDS 128U
#define CWIST_GRAPHQL_MAX_DEPTH 16U

/**
 * Resolver function signature.
 * @param args Parsed field arguments (cJSON Object) or NULL if none.
 * @param variables Parsed request variables (cJSON Object) or NULL if none.
 * @param ctx Opaque user context.
 * @return Return a newly allocated cJSON value; CWIST takes ownership on success.
 */
typedef cJSON *(*cwist_graphql_resolver_fn)(const cJSON *args, const cJSON *variables, void *ctx);

/** Create a new GraphQL schema instance. */
cwist_graphql_schema_t *cwist_graphql_schema_create(void);

/** Destroy a GraphQL schema instance and free all registered resolvers. */
void cwist_graphql_schema_destroy(cwist_graphql_schema_t *schema);

/** Register or replace a top-level Query resolver. */
bool cwist_graphql_add_query(cwist_graphql_schema_t *schema, const char *field,
                             cwist_graphql_resolver_fn resolver, void *ctx);

/** Register or replace a top-level Mutation resolver. */
bool cwist_graphql_add_mutation(cwist_graphql_schema_t *schema, const char *field,
                                cwist_graphql_resolver_fn resolver, void *ctx);

/** Execute a JSON request body containing `query`, optional `operationName`, and optional `variables`. */
cwist_error_t cwist_graphql_execute(cwist_graphql_schema_t *schema, const char *request_json,
                                     cwist_sstring **out_json);

/** HTTP handler adapter for CWIST POST routes. Supports both application/json and GraphQL requests. */
void cwist_graphql_serve(cwist_graphql_schema_t *schema, cwist_http_request *req,
                          cwist_http_response *res);

#ifdef __cplusplus
}
#endif
#endif
