#include <cwist/sys/app/app.h>
#include <cwist/net/http/https.h>
#include <cwist/net/http/async.h>
#include <assert.h>
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";

typedef struct alpn_server_ctx {
    int fd;
    cwist_https_context *ctx;
    cwist_https_protocol protocol;
    cwist_error_t result;
} alpn_server_ctx;

typedef struct aia_server_ctx {
    int listen_fd;
    const char *cert_path;
    bool served;
} aia_server_ctx;

static void *alpn_server_thread(void *arg) {
    alpn_server_ctx *server = arg;
    cwist_https_connection *conn = NULL;
    server->result = cwist_https_accept(server->ctx, server->fd, &conn);
    if (server->result.errtype == CWIST_ERR_INT16 && server->result.error.err_i16 == 0) {
        server->protocol = cwist_https_connection_protocol(conn);
        cwist_https_close_connection(conn);
    } else {
        close(server->fd);
    }
    return NULL;
}

static void run_alpn_client(int fd, const unsigned char *protos, unsigned int protos_len) {
    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, fd) == 1);
    if (protos && protos_len > 0) {
        assert(SSL_set_alpn_protos(client, protos, protos_len) == 0);
    }
    assert(SSL_connect(client) == 1);
    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(fd);
}

static void test_https_defaults(void) {
    printf("Testing HTTPS defaults...\n");
    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->http2_enabled == false);
    assert(SSL_CTX_get_min_proto_version(ctx->ctx) == TLS1_2_VERSION);
    cwist_https_destroy_context(ctx);
    printf("Passed HTTPS defaults.\n");
}

static void test_https_alpn_negotiates_h2_and_http11_fallback(void) {
    printf("Testing HTTPS ALPN negotiation and HTTP/1.1 fallback...\n");
    cwist_https_options options = { .enable_http2 = true };
    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context_with_options(&ctx, TEST_CERT, TEST_KEY, &options, NULL);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    alpn_server_ctx h2_server = { .fd = sv[0], .ctx = ctx, .protocol = CWIST_HTTPS_PROTOCOL_NONE };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, alpn_server_thread, &h2_server) == 0);
    static const unsigned char h2_client_alpn[] = "\x02h2\x08http/1.1";
    run_alpn_client(sv[1], h2_client_alpn, sizeof(h2_client_alpn) - 1);
    pthread_join(tid, NULL);
    assert(h2_server.protocol == CWIST_HTTPS_PROTOCOL_HTTP2);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    alpn_server_ctx http11_server = { .fd = sv[0], .ctx = ctx, .protocol = CWIST_HTTPS_PROTOCOL_NONE };
    assert(pthread_create(&tid, NULL, alpn_server_thread, &http11_server) == 0);
    static const unsigned char http11_client_alpn[] = "\x08http/1.1";
    run_alpn_client(sv[1], http11_client_alpn, sizeof(http11_client_alpn) - 1);
    pthread_join(tid, NULL);
    assert(http11_server.protocol == CWIST_HTTPS_PROTOCOL_HTTP11);

    cwist_https_destroy_context(ctx);
    printf("Passed HTTPS ALPN negotiation and HTTP/1.1 fallback.\n");
}

static void test_app_http2_toggle_rebuilds_context(void) {
    printf("Testing HTTPS/2 toggle context rebuild...\n");
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(app->use_https2 == false);

    cwist_error_t err = cwist_app_use_https2(app, true);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->use_https2 == true);
    assert(app->ssl_ctx == NULL);

    err = cwist_app_use_https(app, TEST_CERT, TEST_KEY);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->ssl_ctx != NULL);
    assert(app->ssl_ctx->http2_enabled == true);

    err = cwist_app_use_https2(app, false);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->ssl_ctx != NULL);
    assert(app->ssl_ctx->http2_enabled == false);

    cwist_app_destroy(app);
    printf("Passed HTTPS/2 toggle context rebuild.\n");
}

static bool run_cmd(const char *cmd) {
    return system(cmd) == 0;
}

static bool join_path(char *dst, size_t dst_len, const char *dir, const char *name) {
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > dst_len) return false;
    memcpy(dst, dir, dir_len);
    dst[dir_len] = '/';
    memcpy(dst + dir_len + 1, name, name_len);
    dst[dir_len + 1 + name_len] = '\0';
    return true;
}

static bool write_text_file(const char *path, const char *data) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    bool ok = fputs(data, fp) >= 0;
    ok = fclose(fp) == 0 && ok;
    return ok;
}

static void remove_tree(const char *dir) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", dir);
    if (system(cmd) == -1) perror("system");
}

static bool create_aia_listener(int *out_fd, unsigned short *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return false;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(fd);
        return false;
    }

    *out_fd = fd;
    *out_port = ntohs(addr.sin_port);
    return true;
}

static void *aia_cert_server_thread(void *arg) {
    aia_server_ctx *server = arg;
    struct pollfd pfd = { .fd = server->listen_fd, .events = POLLIN };
    if (poll(&pfd, 1, 5000) <= 0) {
        close(server->listen_fd);
        return NULL;
    }

    int client_fd = accept(server->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        close(server->listen_fd);
        return NULL;
    }

    char request_buf[1024];
    ssize_t got = read(client_fd, request_buf, sizeof(request_buf));
    (void)got;

    FILE *fp = fopen(server->cert_path, "rb");
    if (!fp) {
        close(client_fd);
        close(server->listen_fd);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long cert_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (cert_len <= 0) {
        fclose(fp);
        close(client_fd);
        close(server->listen_fd);
        return NULL;
    }

    char *cert = malloc((size_t)cert_len);
    if (!cert) {
        fclose(fp);
        close(client_fd);
        close(server->listen_fd);
        return NULL;
    }
    size_t read_len = fread(cert, 1, (size_t)cert_len, fp);
    fclose(fp);
    if (read_len != (size_t)cert_len) {
        free(cert);
        close(client_fd);
        close(server->listen_fd);
        return NULL;
    }

    dprintf(client_fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/pem-certificate\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "\r\n",
            cert_len);
    size_t sent = 0;
    while (sent < (size_t)cert_len) {
        ssize_t n = write(client_fd, cert + sent, (size_t)cert_len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }

    server->served = sent == (size_t)cert_len;
    free(cert);
    close(client_fd);
    close(server->listen_fd);
    return NULL;
}

static bool generate_aia_chain_fixture(char *dir,
                                       size_t dir_len,
                                       const char *leaf_aia_url,
                                       char *leaf_cert,
                                       size_t leaf_cert_len,
                                       char *leaf_key,
                                       size_t leaf_key_len,
                                       char *intermediate_cert,
                                       size_t intermediate_cert_len) {
    char template[] = "/tmp/cwist_tls_chain_XXXXXX";
    char *tmp = mkdtemp(template);
    if (!tmp) return false;
    snprintf(dir, dir_len, "%s", tmp);

    char root_key[PATH_MAX], root_cert[PATH_MAX];
    char int_key[PATH_MAX], int_csr[PATH_MAX], int_ext[PATH_MAX];
    char leaf_csr[PATH_MAX], leaf_ext[PATH_MAX];
    if (!join_path(root_key, sizeof(root_key), dir, "root.key") ||
        !join_path(root_cert, sizeof(root_cert), dir, "root.crt") ||
        !join_path(int_key, sizeof(int_key), dir, "intermediate.key") ||
        !join_path(int_csr, sizeof(int_csr), dir, "intermediate.csr") ||
        !join_path(int_ext, sizeof(int_ext), dir, "intermediate.ext") ||
        !join_path(intermediate_cert, intermediate_cert_len, dir, "intermediate.crt") ||
        !join_path(leaf_key, leaf_key_len, dir, "leaf.key") ||
        !join_path(leaf_csr, sizeof(leaf_csr), dir, "leaf.csr") ||
        !join_path(leaf_cert, leaf_cert_len, dir, "leaf.crt") ||
        !join_path(leaf_ext, sizeof(leaf_ext), dir, "leaf.ext")) {
        return false;
    }

    const char *int_ext_data =
        "basicConstraints=critical,CA:TRUE,pathlen:0\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
        "authorityKeyIdentifier=keyid,issuer\n";
    if (!write_text_file(int_ext, int_ext_data)) return false;

    char leaf_ext_data[1024];
    snprintf(leaf_ext_data, sizeof(leaf_ext_data),
             "basicConstraints=critical,CA:FALSE\n"
             "keyUsage=critical,digitalSignature,keyEncipherment\n"
             "extendedKeyUsage=serverAuth\n"
             "subjectAltName=DNS:localhost,IP:127.0.0.1\n"
             "authorityInfoAccess=caIssuers;URI:%s\n",
             leaf_aia_url);
    if (!write_text_file(leaf_ext, leaf_ext_data)) return false;

    char cmd[32768];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -keyout %s -out %s "
             "-days 1 -sha256 -subj /CN=CWIST-Test-Root "
             "-addext basicConstraints=critical,CA:TRUE,pathlen:1 "
             "-addext keyUsage=critical,keyCertSign,cRLSign >/dev/null 2>&1",
             root_key, root_cert);
    if (!run_cmd(cmd)) return false;

    snprintf(cmd, sizeof(cmd),
             "openssl req -newkey rsa:2048 -nodes -keyout %s -out %s "
             "-subj /CN=CWIST-Test-Intermediate >/dev/null 2>&1",
             int_key, int_csr);
    if (!run_cmd(cmd)) return false;

    snprintf(cmd, sizeof(cmd),
             "openssl x509 -req -in %s -CA %s -CAkey %s -CAcreateserial "
             "-out %s -days 1 -sha256 -extfile %s >/dev/null 2>&1",
             int_csr, root_cert, root_key, intermediate_cert, int_ext);
    if (!run_cmd(cmd)) return false;

    snprintf(cmd, sizeof(cmd),
             "openssl req -newkey rsa:2048 -nodes -keyout %s -out %s "
             "-subj /CN=localhost >/dev/null 2>&1",
             leaf_key, leaf_csr);
    if (!run_cmd(cmd)) return false;

    snprintf(cmd, sizeof(cmd),
             "openssl x509 -req -in %s -CA %s -CAkey %s -CAcreateserial "
             "-out %s -days 1 -sha256 -extfile %s >/dev/null 2>&1",
             leaf_csr, intermediate_cert, int_key, leaf_cert, leaf_ext);
    if (!run_cmd(cmd)) return false;
    return true;
}

static void test_https_autoloads_aia_intermediate(void) {
    printf("Testing HTTPS AIA intermediate autoload...\n");
    if (!run_cmd("command -v openssl >/dev/null 2>&1")) {
        printf("Skipping HTTPS AIA intermediate autoload; openssl CLI not found.\n");
        return;
    }

    int listen_fd = -1;
    unsigned short port = 0;
    assert(create_aia_listener(&listen_fd, &port));

    char aia_url[128];
    snprintf(aia_url, sizeof(aia_url), "http://127.0.0.1:%u/intermediate.crt", port);

    char tmp_dir[PATH_MAX], leaf_cert[PATH_MAX], leaf_key[PATH_MAX], intermediate_cert[PATH_MAX];
    bool generated = generate_aia_chain_fixture(tmp_dir,
                                                sizeof(tmp_dir),
                                                aia_url,
                                                leaf_cert,
                                                sizeof(leaf_cert),
                                                leaf_key,
                                                sizeof(leaf_key),
                                                intermediate_cert,
                                                sizeof(intermediate_cert));
    assert(generated);

    aia_server_ctx server = {
        .listen_fd = listen_fd,
        .cert_path = intermediate_cert,
        .served = false
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, aia_cert_server_thread, &server) == 0);

    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context(&ctx, leaf_cert, leaf_key);
    pthread_join(tid, NULL);

    assert(server.served);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);

    STACK_OF(X509) *chain = NULL;
    assert(SSL_CTX_get0_chain_certs(ctx->ctx, &chain) == 1);
    assert(chain != NULL);
    assert(sk_X509_num(chain) >= 1);
    assert(X509_check_issued(sk_X509_value(chain, 0), SSL_CTX_get0_certificate(ctx->ctx)) == X509_V_OK);

    cwist_https_destroy_context(ctx);
    remove_tree(tmp_dir);
    printf("Passed HTTPS AIA intermediate autoload.\n");
}

static void *async_https_worker_fn(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    usleep(20000);
    cwist_async_respond(a, CWIST_HTTP_OK, "text/plain", "async-https-ok", 14);
    return NULL;
}

static void async_https_route(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    assert(a != NULL);
    pthread_t t;
    assert(pthread_create(&t, NULL, async_https_worker_fn, a) == 0);
    pthread_detach(t);
}

static void normal_https_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "normal-https-ok");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

static int ssl_read_http_response(SSL *client, char *buf, size_t buf_size) {
    size_t total = 0;
    while (total < buf_size - 1) {
        int n = SSL_read(client, buf + total, (int)(buf_size - 1 - total));
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            char *cl = strstr(buf, "Content-Length: ");
            if (!cl) cl = strstr(buf, "content-length: ");
            size_t body_len = 0;
            if (cl) {
                body_len = (size_t)atol(cl + 16);
            }
            size_t received_body = total - ((hdr_end + 4) - buf);
            if (received_body >= body_len) break;
        }
    }
    return (int)total;
}

static void test_https_async_defer(void) {
    printf("Testing HTTPS async defer and keep-alive...\n");
    assert(https_pool_init() == 0);

    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_error_t err = cwist_app_use_https(app, TEST_CERT, TEST_KEY);
    assert(cwist_error_is_ok(&err));

    cwist_app_get(app, "/defer", async_https_route);
    cwist_app_get(app, "/normal", normal_https_route);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    https_pool_submit(sv[0], app->ssl_ctx, app->https_request_handler, app);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    // 1. Send deferred request
    const char *req1 = "GET /defer HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    assert(SSL_write(client, req1, (int)strlen(req1)) > 0);

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    int n = ssl_read_http_response(client, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "200 OK") != NULL);
    assert(strstr(buf, "async-https-ok") != NULL);

    // 2. Send follow-up request on same TLS connection (verifying keep-alive re-arm)
    const char *req2 = "GET /normal HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    assert(SSL_write(client, req2, (int)strlen(req2)) > 0);

    memset(buf, 0, sizeof(buf));
    n = ssl_read_http_response(client, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "200 OK") != NULL);
    assert(strstr(buf, "normal-https-ok") != NULL);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    cwist_app_destroy(app);
    printf("Passed HTTPS async defer and keep-alive.\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_https_defaults();
    test_https_alpn_negotiates_h2_and_http11_fallback();
    test_app_http2_toggle_rebuilds_context();
    test_https_autoloads_aia_intermediate();
    test_https_async_defer();
    printf("All HTTPS tests passed!\n");
    return 0;
}
