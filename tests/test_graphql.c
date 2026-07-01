#include <cwist/net/graphql/graphql.h>
#include <cwist/core/sstring/sstring.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *hello(const cJSON *variables, void *ctx) {
    (void)variables; (void)ctx;
    return cJSON_CreateString("world");
}

int main(void) {
    cwist_graphql_schema_t *schema = cwist_graphql_schema_create();
    assert(schema && cwist_graphql_add_query(schema, "hello", hello, NULL));
    cwist_sstring *out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ hello missing }\"}", &out).error.err_i16 == 0);
    assert(strstr(out->data, "\"hello\":\"world\""));
    assert(strstr(out->data, "unknown query field"));
    cwist_sstring_destroy(out);
    cwist_graphql_schema_destroy(schema);
    puts("All GraphQL tests passed.");
    return 0;
}
