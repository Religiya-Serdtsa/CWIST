#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080

// Actual request handler logic (keep-alive capable)
void handle_client(int client_fd, void *ctx) {
    (void)ctx;
    char *read_buf = malloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!read_buf) {
        close(client_fd);
        return;
    }
    size_t buf_len = 0;
    read_buf[0] = '\0';

    while (1) {
        cwist_http_request *req = cwist_http_receive_request(client_fd, read_buf, CWIST_HTTP_READ_BUFFER_SIZE, &buf_len);
        if (!req) {
            break;
        }

        req->client_fd = client_fd;
        printf("[%s] %s\n", cwist_http_method_to_string(req->method), req->path->data);

        cwist_http_response *res = cwist_http_response_create();
        if (!res) {
            cwist_http_request_destroy(req);
            break;
        }

        cwist_http_header_add(&res->headers, "Server", "Cwist-Simple/1.0");

        bool close_after = !req->keep_alive;
        res->keep_alive = !close_after;

        if (close_after) {
            cwist_http_header_add(&res->headers, "Connection", "close");
        } else {
            cwist_http_header_add(&res->headers, "Connection", "keep-alive");
        }

        if (strcmp(req->path->data, "/") == 0 && req->method == CWIST_HTTP_GET) {
            res->status_code = CWIST_HTTP_OK;
            cwist_sstring_assign(res->status_text, "OK");
            cwist_http_header_add(&res->headers, "Content-Type", "text/html");

            cwist_sstring_assign(res->body,
                "<html>"
                "<head><title>Cwist Server</title></head>"
                "<body>"
                "<h1>Hello from Cwist!</h1>"
                "<p>This is a robust, simple example server.</p>"
                "<a href='/health'>Check Health</a> | <a href='/json'>Get JSON</a>"
                "</body>"
                "</html>"
            );
        }
        else if (strcmp(req->path->data, "/health") == 0 && req->method == CWIST_HTTP_GET) {
            res->status_code = CWIST_HTTP_OK;
            cwist_sstring_assign(res->status_text, "OK");
            cwist_http_header_add(&res->headers, "Content-Type", "application/json");
            cwist_sstring_assign(res->body, "{\"status\": \"ok\", \"uptime\": \"forever\"}");
        }
        else if (strcmp(req->path->data, "/echo") == 0 && req->method == CWIST_HTTP_POST) {
            res->status_code = CWIST_HTTP_OK;
            cwist_sstring_assign(res->status_text, "OK");

            char *ct = cwist_http_header_get(req->headers, "Content-Type");
            if (ct) cwist_http_header_add(&res->headers, "Content-Type", ct);

            cwist_sstring_assign(res->body, req->body->data);
        }
        else {
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->status_text, "Not Found");
            cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
            cwist_sstring_assign(res->body, "404 - Not Found");
        }

        cwist_http_send_response(client_fd, res);

        cwist_http_response_destroy(res);
        cwist_http_request_destroy(req);

        if (close_after) {
            break;
        }
    }

    free(read_buf);
    close(client_fd);
}

int main() {
    struct sockaddr_in server_addr;

    // Create and bind socket
    // Backlog increased to 512 to handle high concurrency
    int server_fd = cwist_make_socket_ipv4(&server_addr, "0.0.0.0", PORT, 512);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to start server. Error code: %d\n", server_fd);
        return 1;
    }

    printf("Server listening on http://localhost:%d\n", PORT);
    printf("Ctrl+C to stop.\n");

    // Start server loop (threaded)
    cwist_server_config config;
    memset(&config, 0, sizeof(config));
    config.use_threading = true;

    cwist_http_server_loop(server_fd, &config, handle_client, NULL);
    return 0;
}
