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

static void *http3_server_thread(void *arg) {
    int udp_fd = *(int *)arg;
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);

    /* The loop will run until we close the socket from the main thread or it fails */
    cwist_http3_server_loop(udp_fd, ctx, http3_test_handler, NULL);

    cwist_http3_destroy_context(ctx);
    return NULL;
}

int main(void) {
    printf("Testing HTTP/3 infrastructure...\n");

    /* 1. Test Context Initialization */
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->ssl_ctx != NULL);
    cwist_http3_destroy_context(ctx);
    printf("HTTP/3 context initialization passed.\n");

    /* 2. Test Server Loop Skeleton */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0); // Random port

    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    pthread_t tid;
    pthread_create(&tid, NULL, http3_server_thread, &udp_fd);

    /* Give it a moment to start */
    usleep(100000);

    /* Send a dummy UDP packet to trigger the loop log */
    struct sockaddr_in server_addr;
    socklen_t server_addr_len = sizeof(server_addr);
    getsockname(udp_fd, (struct sockaddr *)&server_addr, &server_addr_len);

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sendto(client_fd, "hello h3", 8, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
    close(client_fd);

    usleep(100000);

    /* Cleanup: Closing the FD will eventually break the loop in this simple skeleton */
    close(udp_fd);
    
    /* In this skeleton, the thread might still be blocked on recvfrom. 
     * Since it's a test, we can just exit or use a more robust way to stop it.
     */
    
    printf("HTTP/3 server loop skeleton test finished.\n");
    printf("All HTTP/3 infrastructure tests passed!\n");

    return 0;
}
