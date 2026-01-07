#define _POSIX_C_SOURCE 200809L

#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* --- Internal Error Helpers --- */

static cwist_error_t make_ssl_error(const char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    
    unsigned long ssl_err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(ssl_err, buf, sizeof(buf));
    
    cJSON_AddStringToObject(err.error.err_json, "module", "https");
    cJSON_AddStringToObject(err.error.err_json, "message", msg);
    cJSON_AddStringToObject(err.error.err_json, "openssl_error", buf);
    
    return err;
}

/* --- Context Management --- */

cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    // Initialize OpenSSL
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        return make_ssl_error("Unable to create SSL context");
    }

    // Load Cert and Key
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load certificate");
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load private key");
    }

    // Verify key matches cert
    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Private key does not match certificate");
    }

    *ctx = (cwist_https_context*)malloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;

    err.error.err_i16 = 0; // Success
    return err;
}

void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        free(ctx);
        EVP_cleanup();
    }
}

/* --- Connection Handling --- */

cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!ctx || !ctx->ctx || client_fd < 0) {
        err.error.err_i16 = -1;
        return err;
    }

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        return make_ssl_error("Failed to create SSL structure");
    }

    SSL_set_fd(ssl, client_fd);

    if (SSL_accept(ssl) <= 0) {
        // Handshake failed
        // We capture the error before freeing
        cwist_error_t ssl_err = make_ssl_error("SSL handshake failed");
        SSL_free(ssl);
        return ssl_err;
    }

    *conn = (cwist_https_connection*)malloc(sizeof(cwist_https_connection));
    if (!*conn) {
        SSL_free(ssl);
        err.error.err_i16 = -1;
        return err;
    }

    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;
    (*conn)->read_buf = malloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!(*conn)->read_buf) {
        SSL_free(ssl);
        free(*conn);
        *conn = NULL;
        err.error.err_i16 = -1;
        return err;
    }
    (*conn)->buf_len = 0;
    (*conn)->read_buf[0] = '\0';

    err.error.err_i16 = 0;
    return err;
}

void cwist_https_close_connection(cwist_https_connection *conn) {
    if (conn) {
        if (conn->ssl) {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
        }
        if (conn->fd >= 0) {
            close(conn->fd);
        }
        free(conn->read_buf);
        free(conn);
    }
}

/* --- I/O Operations --- */

cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    if (!conn || !conn->ssl || !conn->read_buf) return NULL;

    size_t total_received = conn->buf_len;
    char *header_end = NULL;

    while (!(header_end = strstr(conn->read_buf, "\r\n\r\n"))) {
        if (total_received >= CWIST_HTTP_READ_BUFFER_SIZE - 1) {
            return NULL;
        }

        struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
        int pret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
        if (pret <= 0) {
            return NULL;
        }

        int bytes = SSL_read(conn->ssl, conn->read_buf + total_received, (int)(CWIST_HTTP_READ_BUFFER_SIZE - 1 - total_received));
        if (bytes <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, bytes);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return NULL;
        }

        total_received += (size_t)bytes;
        conn->read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request(conn->read_buf);
    if (!req) return NULL;

    req->client_fd = conn->fd;

    size_t header_len = (header_end + 4) - conn->read_buf;
    size_t body_received = total_received - header_len;

    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        char *body = malloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        size_t to_copy = body_received < req->content_length ? body_received : req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        while (current_body_len < req->content_length) {
            struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
            int pret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
            if (pret <= 0) {
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }

            int bytes = SSL_read(conn->ssl, body + current_body_len, (int)(req->content_length - current_body_len));
            if (bytes <= 0) {
                int ssl_err = SSL_get_error(conn->ssl, bytes);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
        }
        body[req->content_length] = '\0';
        cwist_sstring_assign_len(req->body, body, req->content_length);
        free(body);

        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(conn->read_buf, header_end + 4 + req->content_length, leftover_len);
            conn->buf_len = leftover_len;
        } else {
            conn->buf_len = 0;
        }
    } else {
        if (body_received > 0) {
            memmove(conn->read_buf, header_end + 4, body_received);
            conn->buf_len = body_received;
        } else {
            conn->buf_len = 0;
        }
    }
    conn->read_buf[conn->buf_len] = '\0';

    return req;
}

cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // 1. Serialize using existing HTTP logic
    cwist_sstring *response_str = cwist_http_stringify_response(res);
    if (!response_str) {
        err.error.err_i16 = -1;
        return err;
    }

    // 2. Send over SSL
    const char *p = response_str->data;
    int left = (int)response_str->size;
    int total_sent = 0;

    err.error.err_i16 = 0; // Assume success initially

    while (left > 0) {
        int sent = SSL_write(conn->ssl, p, left);
        if (sent <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, sent);
            if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                continue; // Retry
            }
            err = make_ssl_error("SSL write failed");
            break;
        }
        p += sent;
        left -= sent;
        total_sent += sent;
    }

    cwist_sstring_destroy(response_str);
    return err;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
};

static void *https_thread_handler(void *arg) {
    struct https_thread_payload *payload = (struct https_thread_payload *)arg;
    cwist_https_connection *conn = NULL;
    cwist_error_t hs_err = cwist_https_accept(payload->ctx, payload->client_fd, &conn);
    
    if (hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0) {
        payload->handler(conn, payload->user_ctx);
        cwist_https_close_connection(conn);
    } else {
        if (hs_err.errtype == CWIST_ERR_JSON) {
            cJSON_Delete(hs_err.error.err_json);
        }
        close(payload->client_fd);
    }
    
    free(payload);
    return NULL;
}

cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (server_fd < 0 || !ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue; 
        }

        pthread_t thread;
        struct https_thread_payload *payload = malloc(sizeof(*payload));
        if (!payload) {
            close(client_fd);
            continue;
        }
        payload->client_fd = client_fd;
        payload->ctx = ctx;
        payload->handler = handler;
        payload->user_ctx = user_ctx;

        if (pthread_create(&thread, NULL, https_thread_handler, payload) == 0) {
            pthread_detach(thread);
        } else {
            free(payload);
            close(client_fd);
        }
    }
    
    return err;
}
