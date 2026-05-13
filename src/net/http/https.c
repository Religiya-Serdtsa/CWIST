#define _POSIX_C_SOURCE 200809L

#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>

#define CWIST_ALPN_HTTP11       ((const unsigned char *)"\x08http/1.1")
#define CWIST_ALPN_H2_HTTP11    ((const unsigned char *)"\x02h2\x08http/1.1")
#define CWIST_ALPN_H3_H2_HTTP11 ((const unsigned char *)"\x02h3\x02h2\x08http/1.1")
#define CWIST_ALPN_HTTP11_LEN       9
#define CWIST_ALPN_H2_HTTP11_LEN    12
#define CWIST_ALPN_H3_H2_HTTP11_LEN 15

/**
 * @file https.c
 * @brief OpenSSL-backed HTTPS accept, receive, send, and server-loop helpers.
 */

/**
 * @brief Build a JSON-rich cwist_error_t from the latest OpenSSL error state.
 * @param msg Human-readable message describing the failing HTTPS step.
 * @return Error object with module, message, and OpenSSL error string fields.
 */
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

/**
 * @brief Initialize OpenSSL and create a server TLS context from PEM files.
 * @param ctx Output pointer that receives the allocated HTTPS context.
 * @param cert_path Path to the PEM certificate chain.
 * @param key_path Path to the PEM private key.
 * @return Tagged CWIST error describing success or failure.
 */
static void cwist_https_apply_base_tls_defaults(SSL_CTX *ssl_ctx) {
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_RENEGOTIATION);
#endif
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
}

static cwist_error_t cwist_https_apply_http2_tls_profile(SSL_CTX *ssl_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (SSL_CTX_set_cipher_list(ssl_ctx,
                                "ECDHE-ECDSA-AES128-GCM-SHA256:"
                                "ECDHE-RSA-AES128-GCM-SHA256:"
                                "ECDHE-ECDSA-AES256-GCM-SHA384:"
                                "ECDHE-RSA-AES256-GCM-SHA384:"
                                "ECDHE-ECDSA-CHACHA20-POLY1305:"
                                "ECDHE-RSA-CHACHA20-POLY1305") != 1) {
        return make_ssl_error("Unable to apply HTTP/2-compatible TLS 1.2 cipher profile");
    }
#ifdef SSL_CTX_set_ciphersuites
    if (SSL_CTX_set_ciphersuites(ssl_ctx,
                                 "TLS_AES_128_GCM_SHA256:"
                                 "TLS_AES_256_GCM_SHA384:"
                                 "TLS_CHACHA20_POLY1305_SHA256") != 1) {
        return make_ssl_error("Unable to apply HTTP/2-compatible TLS 1.3 cipher suites");
    }
#endif
    SSL_CTX_set_options(ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    err.error.err_i16 = 0;
    return err;
}

static int cwist_https_alpn_select_cb(SSL *ssl,
                                      const unsigned char **out,
                                      unsigned char *outlen,
                                      const unsigned char *in,
                                      unsigned int inlen,
                                      void *arg) {
    (void)ssl;
    const cwist_https_options *opts = (const cwist_https_options *)arg;
    bool enable_http3 = opts && opts->enable_http3;
    bool enable_http2 = opts && (opts->enable_http2 || enable_http3);

    const unsigned char *supported;
    unsigned int supported_len;

    if (enable_http3) {
        supported = CWIST_ALPN_H3_H2_HTTP11;
        supported_len = CWIST_ALPN_H3_H2_HTTP11_LEN;
    } else if (enable_http2) {
        supported = CWIST_ALPN_H2_HTTP11;
        supported_len = CWIST_ALPN_H2_HTTP11_LEN;
    } else {
        supported = CWIST_ALPN_HTTP11;
        supported_len = CWIST_ALPN_HTTP11_LEN;
    }

    if (SSL_select_next_proto((unsigned char **)out,
                              outlen,
                              supported,
                              supported_len,
                              in,
                              inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }

    return SSL_TLSEXT_ERR_NOACK;
}

/* --- Context Management --- */

cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    return cwist_https_init_context_with_options(ctx, cert_path, key_path, NULL);
}

cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path,
                                                    const char *key_path,
                                                    const cwist_https_options *options) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    bool enable_http3 = options && options->enable_http3;
    bool enable_http2 = options && (options->enable_http2 || enable_http3);
    
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

    if (SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to enforce TLS minimum version");
    }
    cwist_https_apply_base_tls_defaults(ssl_ctx);

    if (enable_http2) {
        err = cwist_https_apply_http2_tls_profile(ssl_ctx);
        if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) {
            SSL_CTX_free(ssl_ctx);
            return err;
        }
    }

    SSL_CTX_set_alpn_select_cb(ssl_ctx,
                               cwist_https_alpn_select_cb,
                               (void *)options);

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

    *ctx = (cwist_https_context*)cwist_alloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;
    (*ctx)->http2_enabled = enable_http2;
    (*ctx)->http3_enabled = enable_http3;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Free an HTTPS context and release its OpenSSL resources.
 * @param ctx Context to destroy.
 */
void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        cwist_free(ctx);
        EVP_cleanup();
    }
}

/**
 * @brief Wrap an accepted TCP client socket in an OpenSSL connection object.
 * @param ctx HTTPS context holding the configured SSL_CTX.
 * @param client_fd Accepted TCP socket descriptor.
 * @param conn Output pointer that receives the allocated connection wrapper.
 * @return Tagged CWIST error describing success or failure.
 */
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

    *conn = (cwist_https_connection*)cwist_alloc(sizeof(cwist_https_connection));
    if (!*conn) {
        SSL_free(ssl);
        err.error.err_i16 = -1;
        return err;
    }

    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;
    (*conn)->read_buf = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!(*conn)->read_buf) {
        SSL_free(ssl);
        cwist_free(*conn);
        *conn = NULL;
        err.error.err_i16 = -1;
        return err;
    }
    (*conn)->buf_len = 0;
    (*conn)->read_buf[0] = '\0';
    (*conn)->negotiated_http2 = false;
    (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP11;
    (*conn)->http3_enabled = ctx->http3_enabled;

    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn && alpn_len == 2 && memcmp(alpn, "h2", 2) == 0) {
        (*conn)->negotiated_http2 = true;
        (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2;
    } else if (alpn && alpn_len == 2 && memcmp(alpn, "h3", 2) == 0) {
        (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP3;
    }

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Gracefully close an HTTPS connection and free its buffers.
 * @param conn HTTPS connection wrapper to close.
 */
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn) {
    return conn && conn->negotiated_protocol == CWIST_HTTPS_PROTOCOL_HTTP2;
}

cwist_https_protocol cwist_https_connection_protocol(const cwist_https_connection *conn) {
    if (!conn) return CWIST_HTTPS_PROTOCOL_NONE;
    return conn->negotiated_protocol;
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
        cwist_free(conn->read_buf);
        cwist_free(conn);
    }
}

/**
 * @brief Read from the TLS stream until a full HTTP request has been assembled.
 * @param conn Active HTTPS connection wrapper.
 * @return Parsed HTTP request, or NULL on timeout, parse failure, or IO failure.
 */
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

        char *body = cwist_alloc(req->content_length + 1);
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
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }

            int bytes = SSL_read(conn->ssl, body + current_body_len, (int)(req->content_length - current_body_len));
            if (bytes <= 0) {
                int ssl_err = SSL_get_error(conn->ssl, bytes);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
        }
        body[req->content_length] = '\0';
        cwist_sstring_assign_len(req->body, body, req->content_length);
        cwist_free(body);

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

/**
 * @brief Serialize an HTTP response and send it over an active TLS connection.
 * @param conn Active HTTPS connection wrapper.
 * @param res Response object to serialize.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // Inject Alt-Svc when HTTP/3 is enabled so clients discover the QUIC endpoint
    if (conn->http3_enabled) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        int port = 443;
        if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
        }
        char alt_svc[64];
        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
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

/**
 * @brief Worker entry point that performs the TLS handshake before dispatching.
 * @param arg Thread payload containing the accepted socket and dispatch callback.
 * @return Always NULL for pthread compatibility.
 */
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

/**
 * @brief Accept HTTPS clients in a loop and dispatch each one to the supplied handler.
 * @param server_fd Bound listening socket descriptor.
 * @param ctx HTTPS context shared by all accepted connections.
 * @param handler Callback invoked for each successful TLS client wrapper.
 * @param user_ctx Opaque pointer forwarded to the handler.
 * @return Tagged CWIST error describing success or failure.
 */
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
