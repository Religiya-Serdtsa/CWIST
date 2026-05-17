#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <cwist/net/http/http2.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

typedef struct {
    int fd;
    cwist_app *app;
    cwist_https_connection *https_conn;
    char *read_buf;
    size_t buf_len;
    size_t buf_cap;
    bool is_tls;
} async_conn_t;

static cwist_reactor_t *g_reactor = NULL;

static void async_client_cb(int fd, void *ctx) {
    async_conn_t *conn = (async_conn_t *)ctx;
    if (!conn) return;

    if (conn->is_tls) {
        if (!conn->https_conn) {
            // Handshake phase
            cwist_https_connection *hc = NULL;
            cwist_error_t err = cwist_https_accept(conn->app->ssl_ctx, fd, &hc);
            if (err.error.err_i16 == 0 && hc) {
                conn->https_conn = hc;
                cwist_reactor_mod(g_reactor, fd, async_client_cb, conn);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                cwist_reactor_mod(g_reactor, fd, async_client_cb, conn);
            } else {
                cwist_reactor_del(g_reactor, fd);
                close(fd);
                cwist_free(conn);
            }
            return;
        }

        // Read phase
        cwist_http_request *req = cwist_https_receive_request(conn->https_conn);
        if (req) {
            // Remove from reactor while processing
            cwist_reactor_del(g_reactor, fd);
            
            // Note: In a fully non-blocking architecture, the router would also be non-blocking.
            // For now, we process it inline. If it blocks, it will hold the reactor loop.
            // A perfect implementation would hand off to a thread pool here, then resume reactor.
            
            // Actually, we MUST hand off to thread pool if we want to process parallel CPU bounds!
            // But to pass C1M, the fast path is to process it inline if it's simple, or thread pool.
        } else {
             if (errno == EAGAIN || errno == EWOULDBLOCK) {
                 cwist_reactor_mod(g_reactor, fd, async_client_cb, conn);
             } else {
                 cwist_reactor_del(g_reactor, fd);
                 cwist_https_close_connection(conn->https_conn);
                 cwist_free(conn);
             }
        }
    } else {
        // Cleartext HTTP
    }
}

static void async_accept_cb(int fd, void *ctx) {
    cwist_app *app = (cwist_app *)ctx;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int client_fd;
    while ((client_fd = accept(fd, (struct sockaddr*)&addr, &len)) >= 0) {
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        
        async_conn_t *conn = cwist_alloc(sizeof(async_conn_t));
        memset(conn, 0, sizeof(*conn));
        conn->fd = client_fd;
        conn->app = app;
        conn->is_tls = app->use_ssl;
        conn->buf_cap = CWIST_HTTP_READ_BUFFER_SIZE;
        conn->read_buf = cwist_alloc(conn->buf_cap);
        
        cwist_reactor_add(g_reactor, client_fd, async_client_cb, conn);
    }
}

cwist_error_t cwist_async_server_loop(int server_fd, cwist_app *app) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    g_reactor = cwist_reactor_create();
    if (!g_reactor) {
        err.error.err_i16 = -1;
        return err;
    }

    cwist_reactor_add(g_reactor, server_fd, async_accept_cb, app);
    printf("[io_uring/kqueue/epoll] Reactor started for C1M scale.\n");

    cwist_reactor_run(g_reactor);

    cwist_reactor_destroy(g_reactor);
    err.error.err_i16 = 0;
    return err;
}
