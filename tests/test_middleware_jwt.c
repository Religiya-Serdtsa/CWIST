/**
 * @file test_middleware_jwt.c
 * @brief Unit tests for JWT middleware secret deduplication and case-insensitive bearer parsing.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

/* Minimal stubs for unused handlers in middleware.c */
void *cwist_metrics_registry(void) { return NULL; }
void cwist_metric_inc(void *m) { (void)m; }
void cwist_metric_add(void *m, double v) { (void)m; (void)v; }
cwist_sstring *cwist_get_client_ip_from_fd(int fd) { (void)fd; return NULL; }
const char *cwist_http_method_to_string(cwist_http_method_t m) { (void)m; return "GET"; }

/* Minimal HTTP header / request / response helpers */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    cwist_http_header_node *node = cwist_alloc(sizeof(*node));
    node->key = cwist_sstring_create();
    cwist_sstring_assign(node->key, key);
    node->value = cwist_sstring_create();
    cwist_sstring_assign(node->value, value);
    node->next = *head;
    *head = node;
    cwist_error_t err;
    memset(&err, 0, sizeof(err));
    return err;
}

char *cwist_http_header_get(cwist_http_header_node *headers, const char *key) {
    for (cwist_http_header_node *curr = headers; curr; curr = curr->next) {
        if (curr->key && curr->key->data && strcasecmp(curr->key->data, key) == 0) {
            return curr->value ? curr->value->data : NULL;
        }
    }
    return NULL;
}

static void free_headers(cwist_http_header_node *head) {
    while (head) {
        cwist_http_header_node *next = head->next;
        if (head->key) cwist_sstring_destroy(head->key);
        if (head->value) cwist_sstring_destroy(head->value);
        cwist_free(head);
        head = next;
    }
}

static bool s_handler_reached = false;

static void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    s_handler_reached = true;
    const cwist_jwt_claims *claims = cwist_mw_jwt_get_claims(req);
    assert(claims != NULL);
    assert(strcmp(cwist_jwt_claims_get(claims, "sub"), "alice") == 0);
}

static void test_jwt_middleware_secret_dedup(void) {
    printf("test_jwt_middleware_secret_dedup...\n");

    char secret1[32];
    char secret2[32];
    strcpy(secret1, "secret_xyz");
    strcpy(secret2, "secret_xyz");
    assert(secret1 != secret2); /* Different pointers with identical content */

    cwist_middleware_func mw1 = cwist_mw_jwt_auth(secret1);
    cwist_middleware_func mw2 = cwist_mw_jwt_auth(secret2);

    assert(mw1 != NULL);
    assert(mw2 != NULL);
    /* Should be deduplicated to the exact same wrapper function */
    assert(mw1 == mw2);
}

static void test_jwt_middleware_bearer_case_and_whitespace(void) {
    printf("test_jwt_middleware_bearer_case_and_whitespace...\n");

    const char *secret = "jwt_test_secret";
    cwist_middleware_func mw = cwist_mw_jwt_auth(secret);
    assert(mw != NULL);

    char *token = cwist_jwt_sign("{\"sub\":\"alice\"}", secret, 3600);
    assert(token != NULL);

    /* Test 1: Standard "Bearer <token>" */
    cwist_http_request req1;
    cwist_http_response res1;
    memset(&req1, 0, sizeof(req1));
    memset(&res1, 0, sizeof(res1));
    res1.body = cwist_sstring_create();

    char auth_val[512];
    snprintf(auth_val, sizeof(auth_val), "Bearer %s", token);
    cwist_http_header_add(&req1.headers, "Authorization", auth_val);

    s_handler_reached = false;
    mw(&req1, &res1, dummy_next);
    assert(s_handler_reached == true);
    assert(res1.status_code == 0 || res1.status_code == CWIST_HTTP_OK);

    free_headers(req1.headers);
    free_headers(res1.headers);
    cwist_sstring_destroy(res1.body);

    /* Test 2: Lowercase "bearer   <token>" with extra spaces */
    cwist_http_request req2;
    cwist_http_response res2;
    memset(&req2, 0, sizeof(req2));
    memset(&res2, 0, sizeof(res2));
    res2.body = cwist_sstring_create();

    snprintf(auth_val, sizeof(auth_val), "bearer   %s", token);
    cwist_http_header_add(&req2.headers, "Authorization", auth_val);

    s_handler_reached = false;
    mw(&req2, &res2, dummy_next);
    assert(s_handler_reached == true);

    free_headers(req2.headers);
    free_headers(res2.headers);
    cwist_sstring_destroy(res2.body);

    /* Test 3: Invalid scheme "Basic <token>" */
    cwist_http_request req3;
    cwist_http_response res3;
    memset(&req3, 0, sizeof(req3));
    memset(&res3, 0, sizeof(res3));
    res3.body = cwist_sstring_create();

    snprintf(auth_val, sizeof(auth_val), "Basic %s", token);
    cwist_http_header_add(&req3.headers, "Authorization", auth_val);

    s_handler_reached = false;
    mw(&req3, &res3, dummy_next);
    assert(s_handler_reached == false);
    assert(res3.status_code == CWIST_HTTP_UNAUTHORIZED);

    free_headers(req3.headers);
    free_headers(res3.headers);
    cwist_sstring_destroy(res3.body);

    cwist_free(token);
}

int main(void) {
    test_jwt_middleware_secret_dedup();
    test_jwt_middleware_bearer_case_and_whitespace();
    printf("All test_middleware_jwt tests passed!\n");
    return 0;
}
