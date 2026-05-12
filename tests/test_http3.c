#include <cwist/net/http/http3.h>
#include <cwist/sys/err/cwist_err.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY "example/othello-web/server.key"

static void http3_test_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    (void)req;
    (void)res;
}

#if CWIST_HAVE_OPENSSL_QUIC
static void *http3_server_thread(void *arg) {
    int udp_fd = *(int *)arg;
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);

    cwist_http3_server_loop(udp_fd, ctx, http3_test_handler, NULL);

    cwist_http3_destroy_context(ctx);
    return NULL;
}
#endif

int main(void) {
    printf("Testing HTTP/3 infrastructure...\n");

    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);

#if CWIST_HAVE_OPENSSL_QUIC
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->ssl_ctx != NULL);
    cwist_http3_destroy_context(ctx);
    printf("HTTP/3 context initialization passed.\n");

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    pthread_t tid;
    pthread_create(&tid, NULL, http3_server_thread, &udp_fd);

    usleep(100000);

    struct sockaddr_in server_addr;
    socklen_t server_addr_len = sizeof(server_addr);
    getsockname(udp_fd, (struct sockaddr *)&server_addr, &server_addr_len);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sendto(client_fd, "hello h3", 8, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
    close(client_fd);

    usleep(100000);
    close(udp_fd);

    printf("HTTP/3 server loop skeleton test finished.\n");
#else
    assert(err.error.err_i16 == -1);
    assert(ctx == NULL);
    printf("HTTP/3 QUIC support unavailable in this OpenSSL build; TCP HTTP/1.1 fallback remains available.\n");
#endif

    printf("All HTTP/3 infrastructure tests passed!\n");
    return 0;
}
