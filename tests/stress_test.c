#include <cwist/sys/app/app.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

void api_handler(cwist_http_request *req, cwist_http_response *res) {
    // Check if body is received
    if (req->body && req->body->size > 0) {
        cwist_sstring_assign_len(res->body, req->body->data, req->body->size);
    } else {
        cwist_sstring_assign(res->body, "{\"status\":\"ok\"}");
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

void *run_server(void *arg) {
    cwist_app *app = cwist_app_create();
    cwist_app_post(app, "/api", api_handler);
    cwist_app_listen(app, 31744);
    cwist_app_destroy(app);
    return NULL;
}

int main() {
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, run_server, NULL) != 0) {
        perror("Failed to create server thread");
        return 1;
    }

    // Wait for server to start
    sleep(1);

    // Prepare payload
    FILE *f = fopen("payload.json", "w");
    fprintf(f, "{\"test\":\"data\", \"more\": [1,2,3]}");
    fclose(f);

    // Run ab command
    printf("Running ab stress test...\n");
    // ab -k -n 20000 -c 64 -p payload.json -T application/json http://127.0.0.1:31744/api
    int ret = system("ab -k -n 20000 -c 64 -p payload.json -T application/json http://127.0.0.1:31744/api");

    if (ret != 0) {
        fprintf(stderr, "ab test failed with return code %d\n", ret);
        return 1;
    }

    printf("ab stress test passed!\n");

    // Clean up
    unlink("payload.json");
    
    // In a real test we might want to kill the server thread, but since this is a one-off test script,
    // exiting main is fine if we are done.
    
    return 0;
}
