#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/net/http/session.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/cookie.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int header_contains(cwist_http_header_node *headers, const char *key, const char *value) {
    cwist_http_header_node *curr = headers;
    while (curr) {
        if (curr->key && curr->value &&
            strcasecmp(curr->key->data, key) == 0 &&
            strstr(curr->value->data, value) != NULL) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

static void test_session_create_and_commit(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "my-super-secret-key") == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    assert(req != NULL && res != NULL);

    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(session != NULL);
    assert(cwist_session_set(session, "user_id", "42") == 0);
    assert(cwist_session_commit(session, res) == 0);

    assert(header_contains(res->headers, "Set-Cookie", "cwist_session="));
    assert(header_contains(res->headers, "Set-Cookie", "HttpOnly"));

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[session] create and commit OK\n");
}

static void test_session_round_trip(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_use_session(app, "another-secret") == 0);
    cwist_app_set_session_name(app, "session2");

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(cwist_session_set(session, "role", "admin") == 0);
    assert(cwist_session_commit(session, res) == 0);

    const char *cookie_value = NULL;
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key && strcasecmp(curr->key->data, "Set-Cookie") == 0) {
            cookie_value = curr->value->data;
            break;
        }
        curr = curr->next;
    }
    assert(cookie_value != NULL);

    const char *eq = strchr(cookie_value, '=');
    assert(eq != NULL);
    const char *semi = strchr(eq, ';');
    char value[4096];
    size_t len = semi ? (size_t)(semi - (eq + 1)) : strlen(eq + 1);
    assert(len < sizeof(value));
    memcpy(value, eq + 1, len);
    value[len] = '\0';

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);

    cwist_http_request *req2 = cwist_http_request_create();
    cwist_http_response *res2 = cwist_http_response_create();
    char cookie_header[8192];
    snprintf(cookie_header, sizeof(cookie_header), "session2=%s", value);
    cwist_http_header_add(&req2->headers, "Cookie", cookie_header);

    cwist_session_t *session2 = cwist_session_start(app, req2, res2);
    assert(session2 != NULL);
    const char *role = cwist_session_get(session2, "role");
    assert(role != NULL && strcmp(role, "admin") == 0);

    cwist_http_response_destroy(res2);
    cwist_http_request_destroy(req2);
    cwist_app_destroy(app);
    printf("[session] round trip OK\n");
}

static void test_session_invalid_signature(void) {
    cwist_app *app = cwist_app_create();
    assert(cwist_app_use_session(app, "secret-a") == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_http_header_add(&req->headers, "Cookie",
                          "cwist_session=ZW1wdHk.invalidsig");

    cwist_session_t *session = cwist_session_start(app, req, res);
    assert(cwist_session_get(session, "x") == NULL);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[session] invalid signature rejected OK\n");
}

int main(void) {
    test_session_create_and_commit();
    test_session_round_trip();
    test_session_invalid_signature();
    printf("All session tests passed.\n");
    return 0;
}
