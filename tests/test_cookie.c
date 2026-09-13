/**
 * @file test_cookie.c
 * @brief Unit tests for HTTP cookie parsing, encoding, decoding, and header generation.
 */

#include <cwist/net/http/cookie.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/query.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_cookie_encode(void) {
    printf("Testing cookie encode...\n");
    char *encoded = cwist_cookie_encode("hello world; test=1");
    assert(encoded != NULL);
    assert(strstr(encoded, "%20") != NULL);
    assert(strstr(encoded, "%3B") != NULL);
    assert(strstr(encoded, "%3D") != NULL);
    free(encoded);

    /* Clean alphanumeric should not change */
    char *clean = cwist_cookie_encode("abc-123_xyz.~");
    assert(clean != NULL);
    assert(strcmp(clean, "abc-123_xyz.~") == 0);
    free(clean);
    printf("  Passed encode.\n");
}

static void test_cookie_decode(void) {
    printf("Testing cookie decode...\n");
    char out[128];

    /* Basic hex decoding and + as space */
    int len = cwist_cookie_decode("hello+world%21%2A", out, sizeof(out));
    assert(len == 13);
    assert(strcmp(out, "hello world!*") == 0);

    /* Lowercase hex */
    len = cwist_cookie_decode("%2fpath%2fa", out, sizeof(out));
    assert(len == 7);
    assert(strcmp(out, "/path/a") == 0);

    /* Incomplete/malformed percent sequences treated safely as literal */
    len = cwist_cookie_decode("abc%1gdef%", out, sizeof(out));
    assert(len > 0);
    assert(strcmp(out, "abc%1gdef%") == 0);

    /* Small buffer overflow protection */
    char small[4];
    int rc = cwist_cookie_decode("toolongstring", small, sizeof(small));
    assert(rc == -1);

    printf("  Passed decode.\n");
}

static void test_cookie_parse_and_get(void) {
    printf("Testing cookie parse and get...\n");
    cwist_query_map *map = cwist_query_map_create();
    assert(map != NULL);

    const char *header = "session_id=s%20123; user=alice; theme=dark; ; =empty;   spaced   =value  ";
    cwist_cookie_parse(map, header);

    const char *session = cwist_cookie_get(map, "session_id");
    assert(session != NULL && strcmp(session, "s 123") == 0);

    const char *user = cwist_cookie_get(map, "user");
    assert(user != NULL && strcmp(user, "alice") == 0);

    const char *theme = cwist_cookie_get(map, "theme");
    assert(theme != NULL && strcmp(theme, "dark") == 0);

    const char *spaced = cwist_cookie_get(map, "spaced");
    assert(spaced != NULL && strcmp(spaced, "value  ") == 0);

    /* Empty name or missing cookies should return NULL */
    assert(cwist_cookie_get(map, "") == NULL);
    assert(cwist_cookie_get(map, "missing") == NULL);

    cwist_query_map_destroy(map);
    printf("  Passed parse and get.\n");
}

static void test_cookie_set_and_delete(void) {
    printf("Testing cookie set and delete...\n");
    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);

    cwist_cookie_options opts = {
        .path = "/api",
        .domain = "example.com",
        .max_age_seconds = 3600,
        .http_only = true,
        .secure = true,
        .same_site = "Lax",
    };

    int rc = cwist_cookie_set(res, "auth_token", "xyz 789", &opts);
    assert(rc == 0);

    const char *set_cookie = cwist_http_header_get(res->headers, "Set-Cookie");
    assert(set_cookie != NULL);
    assert(strstr(set_cookie, "auth_token=xyz%20789") != NULL);
    assert(strstr(set_cookie, "Path=/api") != NULL);
    assert(strstr(set_cookie, "Domain=example.com") != NULL);
    assert(strstr(set_cookie, "Max-Age=3600") != NULL);
    assert(strstr(set_cookie, "HttpOnly") != NULL);
    assert(strstr(set_cookie, "Secure") != NULL);
    assert(strstr(set_cookie, "SameSite=Lax") != NULL);

    /* Delete cookie */
    cwist_http_response *del_res = cwist_http_response_create();
    assert(del_res != NULL);
    rc = cwist_cookie_delete(del_res, "auth_token");
    assert(rc == 0);

    const char *del_cookie = cwist_http_header_get(del_res->headers, "Set-Cookie");
    assert(del_cookie != NULL);
    assert(strstr(del_cookie, "auth_token=;") != NULL);
    assert(strstr(del_cookie, "Max-Age=0") != NULL);

    cwist_http_response_destroy(res);
    cwist_http_response_destroy(del_res);
    printf("  Passed set and delete.\n");
}

int main(void) {
    test_cookie_encode();
    test_cookie_decode();
    test_cookie_parse_and_get();
    test_cookie_set_and_delete();
    printf("All cookie tests passed!\n");
    return 0;
}
