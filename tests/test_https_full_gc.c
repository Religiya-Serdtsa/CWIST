#define _POSIX_C_SOURCE 200809L
/* Covers the connection-registry wiring in https.c: cwist_https_accept()
 * tracks the new connection, cwist_https_close_connection() untracks it.
 * With full-GC on, an explicit close leaves nothing pending; a connection
 * whose owning thread exits without closing it gets closed by the
 * registry's thread-exit sweep instead of leaking the fd.
 */
#include <cwist/net/http/https.h>
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";

typedef struct {
    int fd;
    cwist_https_context *ctx;
    cwist_https_connection *conn; /* filled in; caller decides whether to close it */
} server_arg_t;

static void run_tls_client(int fd) {
    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, fd) == 1);
    assert(SSL_connect(client) == 1);
    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(fd);
}

static void *run_tls_client_thunk(void *arg) {
    int fd = *(int *)arg;
    run_tls_client(fd);
    return NULL;
}

static int accept_one(cwist_https_context *ctx, cwist_https_connection **out_conn) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pthread_t client;
    int client_fd = sv[1];
    assert(pthread_create(&client, NULL, run_tls_client_thunk, &client_fd) == 0);
    cwist_error_t err = cwist_https_accept(ctx, sv[0], out_conn);
    pthread_join(client, NULL);
    assert(err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0);
    return 0;
}

/* --- explicit close leaves nothing pending --- */
static void test_explicit_close(void) {
    cwist_https_context *ctx = NULL;
    assert(cwist_https_init_context(&ctx, TEST_CERT, TEST_KEY).error.err_i16 == 0);

    cwist_https_connection *conn = NULL;
    accept_one(ctx, &conn);
    assert(conn != NULL);
    assert(cwist_conn_registry_pending_count() == 1);

    cwist_https_close_connection(conn);
    assert(cwist_conn_registry_pending_count() == 0);

    cwist_https_destroy_context(ctx);
    printf("test_https_full_gc: explicit close ok\n");
}

/* --- a forgotten connection is closed by the thread-exit sweep --- */
typedef struct {
    cwist_https_context *ctx;
    int conn_fd; /* written back so the joiner can check it got close()'d */
} forget_arg_t;

static void *thread_forgets_connection(void *arg) {
    forget_arg_t *fa = (forget_arg_t *)arg;
    cwist_https_connection *conn = NULL;
    accept_one(fa->ctx, &conn);
    fa->conn_fd = conn->fd;
    /* no cwist_https_close_connection() call -- forgotten on purpose */
    return NULL;
}

static void test_thread_exit_sweep(void) {
    cwist_https_context *ctx = NULL;
    assert(cwist_https_init_context(&ctx, TEST_CERT, TEST_KEY).error.err_i16 == 0);

    forget_arg_t fa = {.ctx = ctx, .conn_fd = -1};
    pthread_t t;
    assert(pthread_create(&t, NULL, thread_forgets_connection, &fa) == 0);
    assert(pthread_join(t, NULL) == 0);

    assert(fa.conn_fd >= 0);
    /* The connection's fd must now be closed: fcntl on a closed fd fails
     * with EBADF. This is the actual observable effect of
     * cwist_https_close_connection() having run via the sweep, not just
     * "some close_fn or other" -- fd + SSL + read_buf are all released by
     * that one function. */
    errno = 0;
    int rc = fcntl(fa.conn_fd, F_GETFD);
    assert(rc == -1 && errno == EBADF);

    cwist_https_destroy_context(ctx);
    printf("test_https_full_gc: thread-exit sweep closed the forgotten connection\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN); /* the client side of the test closes its fd
                                * right after SSL_shutdown(); the server's
                                * own SSL_shutdown() write can then race a
                                * SIGPIPE, same as the real app -- see
                                * src/sys/app/app.c's own SIG_IGN. */
    cwist_full_gc(true);
    test_explicit_close();
    test_thread_exit_sweep();
    printf("test_https_full_gc: ok\n");
    return 0;
}
