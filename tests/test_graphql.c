#include <cwist/net/graphql/graphql.h>
#include <cwist/core/sstring/sstring.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *hello_resolver(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)variables; (void)ctx;
    const char *name = "world";
    if (args) {
        cJSON *name_arg = cJSON_GetObjectItemCaseSensitive(args, "name");
        if (name_arg && cJSON_IsString(name_arg)) {
            name = name_arg->valuestring;
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "hello %s", name);
    return cJSON_CreateString(buf);
}

static cJSON *create_user_mutation(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)variables; (void)ctx;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "id", 42);
    if (args) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
        if (name && cJSON_IsString(name)) {
            cJSON_AddStringToObject(res, "name", name->valuestring);
        }
    }
    return res;
}

int main(void) {
    cwist_graphql_schema_t *schema = cwist_graphql_schema_create();
    assert(schema);

    assert(cwist_graphql_add_query(schema, "greet", hello_resolver, NULL));
    assert(cwist_graphql_add_mutation(schema, "createUser", create_user_mutation, NULL));

    /* Test 1: Query with field argument */
    cwist_sstring *out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ greet(name: \\\"alice\\\") }\"}", &out).error.err_i16 == 0);
    assert(strstr(out->data, "\"greet\":\"hello alice\""));
    cwist_sstring_destroy(out);

    /* Test 2: Query with Field Alias */
    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ myAlias: greet }\"}", &out).error.err_i16 == 0);
    assert(strstr(out->data, "\"myAlias\":\"hello world\""));
    cwist_sstring_destroy(out);

    /* Test 3: Mutation execution */
    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"mutation { createUser(name: \\\"bob\\\") }\"}", &out).error.err_i16 == 0);
    assert(strstr(out->data, "\"createUser\":{\"id\":42,\"name\":\"bob\"}"));
    cwist_sstring_destroy(out);

    /* Test 4: Variable replacement in arguments */
    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ greet(name: $userName) }\", \"variables\": {\"userName\": \"charlie\"}}", &out).error.err_i16 == 0);
    assert(strstr(out->data, "\"greet\":\"hello charlie\""));
    cwist_sstring_destroy(out);

    cwist_graphql_schema_destroy(schema);
    puts("All GraphQL advanced tests passed successfully.");
    return 0;
}
