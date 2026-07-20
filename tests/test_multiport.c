#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void root_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "root");
}

static void sub_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "sub");
}

static void root_only_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "root-only");
}

int main(void) {
    unsigned short additional[] = { 18081, 18082 };
    cwist_multiport_t ports = cwist_create_multiport(additional);
    assert(ports.valid);
    assert(ports.count == 2);
    assert(ports.ports[0] == 18081);
    assert(ports.ports[1] == 18082);

    cwist_multiport_t empty = cwist_create_multiport_from_array(NULL, 0);
    assert(empty.valid);
    assert(empty.count == 0);

    cwist_app *root = cwist_app_create();
    assert(root != NULL);
    root->port = 18080;
    cwist_app_get(root, "/who", root_handler);

    cwist_app *sub = cwist_multiport_get_app(&root, 18081);
    assert(sub != NULL);
    assert(sub != root);
    assert(sub->port == 18081);
    assert(cwist_multiport_get_app(&root, 18080) == NULL);
    assert(cwist_multiport_get_app(&root, 0) == NULL);
    assert(cwist_multiport_get_app(&root, 18081) == sub);

    cwist_app_get(sub, "/who", sub_handler);
    cwist_app_get(root, "/root-only", root_only_handler);

    cwist_test_client *root_client = cwist_test_client_create(root);
    cwist_test_client *sub_client = cwist_test_client_create(sub);
    assert(root_client != NULL);
    assert(sub_client != NULL);

    cwist_http_response *res = cwist_test_client_get(root_client, "/who");
    assert(res != NULL && res->body != NULL);
    assert(strcmp(res->body->data, "root") == 0);
    cwist_http_response_destroy(res);

    res = cwist_test_client_get(sub_client, "/who");
    assert(res != NULL && res->body != NULL);
    assert(strcmp(res->body->data, "sub") == 0);
    cwist_http_response_destroy(res);

    res = cwist_test_client_get(sub_client, "/root-only");
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_NOT_FOUND);
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(sub_client);
    cwist_test_client_destroy(root_client);
    cwist_app_destroy(root);

    printf("test_multiport: OK\n");
    return 0;
}
