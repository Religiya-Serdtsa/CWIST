#include <cwist/app.h>
#include <cwist/sys/app/test_client.h>

static void handle_api(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"tested\":true}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/api/test", handle_api);

    cwist_test_client *client = cwist_test_client_create(app);
    if (client) {
        cwist_http_response *res = cwist_test_client_get(client, "/api/test");
        CWIST_ASSERT_STATUS(res, 200);
        CWIST_ASSERT_BODY_CONTAINS(res, "\"status\":\"ok\"");
        cwist_http_response_destroy(res);
        cwist_test_client_destroy(client);
        printf("Test client assertion passed.\n");
    }

    cwist_app_destroy(app);
    return 0;
}
