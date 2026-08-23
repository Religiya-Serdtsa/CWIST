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
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

static cwist_reactor_t *g_reactor = NULL;

static bool app_use_https(const cwist_app *app) {
    return app && app->use_ssl && app->ssl_ctx && app->https_request_handler;
}

static void async_accept_cb(int fd, void *ctx) {
    /* ctx is the reactor slot's inline payload holding the app pointer. */
    cwist_app *app = *(cwist_app **)ctx;
    int client_fd;
#if defined(__linux__)
    while ((client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC)) >= 0) {
#else
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    while ((client_fd = accept(fd, (struct sockaddr*)&addr, &len)) >= 0) {
#endif
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#if defined(__linux__) && defined(TCP_QUICKACK)
        int quickack = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif

        if (app_use_https(app)) {
            cwist_https_dispatch(client_fd, app->ssl_ctx, app->https_request_handler, app);
        } else if (app && !app->use_ssl) {
            cwist_http_pool_submit_async(client_fd, cwist_app_http_handler_async, app);
        } else {
            fprintf(stderr, "[async] SSL request accepted but HTTPS not ready (use_ssl=%d ssl_ctx=%p handler=%p), closing fd=%d\n",
                    app ? app->use_ssl : -1,
                    app ? (void*)app->ssl_ctx : NULL,
                    app ? (void*)app->https_request_handler : NULL,
                    client_fd);
            close(client_fd);
        }
    }

    /* Re-arm the listening socket so we can accept the next batch.  A
     * transient submission failure (e.g. a momentarily full io_uring SQ
     * under a connect burst) must not silently stop accepting on this
     * worker forever — retry with a short backoff, then scream. */
    if (g_reactor) {
        if (atomic_load(&g_cwist_running)) {
            for (int attempt = 0; attempt < 1000; attempt++) {
                if (cwist_reactor_add(g_reactor, fd, async_accept_cb, &app, sizeof(app))) {
                    return;
                }
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            fprintf(stderr, "[async] FATAL: listen socket re-arm failed 1000x; this worker stopped accepting\n");
        } else {
            cwist_reactor_stop(g_reactor);
        }
    }
}

cwist_error_t cwist_async_server_loop(int server_fd, cwist_app *app) {
    cwist_error_t err;
    memset(&err, 0, sizeof(err));
    err.errtype = CWIST_ERR_INT16;
    err.error.err_i16 = -1;

    bool use_https = app_use_https(app);
    if (use_https) {
        if (https_pool_init() != 0) {
            fprintf(stderr, "[async] Failed to init HTTPS thread pool\n");
            return err;
        }
    } else {
        if (cwist_http_pool_init() != 0) {
            fprintf(stderr, "[async] Failed to init HTTP thread pool\n");
            return err;
        }
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("[async] Failed to set server socket non-blocking");
        if (use_https) https_pool_destroy();
        else cwist_http_pool_destroy();
        return err;
    }

#if defined(SO_REUSEPORT)
    int reuseport = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport));
#endif

    g_cwist_listen_fd = server_fd;

    g_reactor = cwist_reactor_create();
    if (!g_reactor) {
        fprintf(stderr, "[async] Failed to create reactor\n");
        if (use_https) https_pool_destroy();
        else cwist_http_pool_destroy();
        return err;
    }

    cwist_reactor_add(g_reactor, server_fd, async_accept_cb, &app, sizeof(app));
    printf("[io_uring/kqueue/epoll] Reactor started for C1M scale.\n");

    cwist_reactor_run(g_reactor);

    cwist_reactor_destroy(g_reactor);
    g_reactor = NULL;

    if (use_https) {
        https_pool_destroy();
    } else {
        cwist_http_pool_destroy();
    }

    err.error.err_i16 = 0;
    return err;
}
