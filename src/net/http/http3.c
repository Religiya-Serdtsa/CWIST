/**
 * @file http3.c
 * @brief Implementation of HTTP/3 protocol handler for CWIST.
 *
 * This file implements an HTTP/3 server using OpenSSL's QUIC support.
 * It manages the QUIC connection, accepts multiplexed incoming QUIC streams,
 * parses basic HTTP/3 frames, and dispatches requests.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/http3.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

/**
 * @brief Initialize HTTP/3 context for a UDP socket.
 *
 * @param ctx Output pointer for the created context.
 * @param cert_path Path to the TLS certificate.
 * @param key_path Path to the TLS private key.
 * @return cwist_error_t Success or error status.
 */
cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx,
                                       const char *cert_path,
                                       const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    /* 1. Create OpenSSL QUIC server context */
    const SSL_METHOD *method = OSSL_QUIC_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        err.error.err_i16 = -1;
        return err;
    }

    /* 2. Load certificates */
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert_path, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }

    /* 3. Set ALPN for HTTP/3 */
    static const unsigned char h3_alpn[] = "\x02h3";
    SSL_CTX_set_alpn_protos(ssl_ctx, h3_alpn, sizeof(h3_alpn) - 1);

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }

    (*ctx)->ssl_ctx = ssl_ctx;
    (*ctx)->udp_fd = -1;

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Initialize an HTTP/3 context without manual certificates.
 *
 * Generates an ephemeral self-signed RSA certificate in memory.
 * Ideal for local testing or zero-config QUIC setups.
 *
 * @param ctx Output pointer for the created context.
 * @return cwist_error_t Success or error status.
 */
cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!ctx) {
        err.error.err_i16 = -1;
        return err;
    }

    const SSL_METHOD *method = OSSL_QUIC_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        err.error.err_i16 = -1;
        return err;
    }

    EVP_PKEY *pkey = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    if (!pkey) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }

    X509 *x509 = X509_new();
    if (!x509) {
        EVP_PKEY_free(pkey);
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year
    X509_set_pubkey(x509, pkey);
    X509_sign(x509, pkey, EVP_sha256());

    SSL_CTX_use_certificate(ssl_ctx, x509);
    SSL_CTX_use_PrivateKey(ssl_ctx, pkey);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    static const unsigned char h3_alpn[] = "\x02h3";
    SSL_CTX_set_alpn_protos(ssl_ctx, h3_alpn, sizeof(h3_alpn) - 1);

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }

    (*ctx)->ssl_ctx = ssl_ctx;
    (*ctx)->udp_fd = -1;

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Destroy HTTP/3 context.
 *
 * @param ctx Context to be destroyed.
 */
void cwist_http3_destroy_context(cwist_http3_context *ctx) {
    if (ctx) {
        if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
        cwist_free(ctx);
    }
}


#define CWIST_HTTP3_FRAME_DATA 0x00
#define CWIST_HTTP3_FRAME_HEADERS 0x01

static size_t h3_encode_varint(uint64_t value, unsigned char out[8]) {
    if (value <= 0x3f) {
        out[0] = (unsigned char)value;
        return 1;
    }
    if (value <= 0x3fff) {
        out[0] = (unsigned char)(0x40 | ((value >> 8) & 0x3f));
        out[1] = (unsigned char)(value & 0xff);
        return 2;
    }
    if (value <= 0x3fffffff) {
        out[0] = (unsigned char)(0x80 | ((value >> 24) & 0x3f));
        out[1] = (unsigned char)((value >> 16) & 0xff);
        out[2] = (unsigned char)((value >> 8) & 0xff);
        out[3] = (unsigned char)(value & 0xff);
        return 4;
    }
    out[0] = (unsigned char)(0xc0 | ((value >> 56) & 0x3f));
    out[1] = (unsigned char)((value >> 48) & 0xff);
    out[2] = (unsigned char)((value >> 40) & 0xff);
    out[3] = (unsigned char)((value >> 32) & 0xff);
    out[4] = (unsigned char)((value >> 24) & 0xff);
    out[5] = (unsigned char)((value >> 16) & 0xff);
    out[6] = (unsigned char)((value >> 8) & 0xff);
    out[7] = (unsigned char)(value & 0xff);
    return 8;
}

static int h3_decode_varint(const unsigned char *buf, size_t len, size_t *pos, uint64_t *value) {
    if (*pos >= len) return -1;
    unsigned char first = buf[*pos];
    size_t width = (size_t)1u << (first >> 6);
    if (*pos + width > len) return -1;

    uint64_t v = (uint64_t)(first & 0x3f);
    for (size_t i = 1; i < width; i++) {
        v = (v << 8) | buf[*pos + i];
    }
    *pos += width;
    *value = v;
    return 0;
}

static int h3_ssl_write_all(SSL *stream, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        int n = SSL_write(stream, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int h3_write_frame(SSL *stream, uint64_t type, const unsigned char *payload, size_t payload_len) {
    unsigned char header[16];
    unsigned char encoded[8];
    size_t header_len = 0;

    size_t n = h3_encode_varint(type, encoded);
    memcpy(header + header_len, encoded, n);
    header_len += n;
    n = h3_encode_varint(payload_len, encoded);
    memcpy(header + header_len, encoded, n);
    header_len += n;

    if (h3_ssl_write_all(stream, header, header_len) != 0) return -1;
    if (payload_len > 0 && payload) return h3_ssl_write_all(stream, payload, payload_len);
    return 0;
}

static void h3_apply_minimal_request_headers(cwist_http_request *req,
                                             const unsigned char *buf,
                                             size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint64_t type = 0;
        uint64_t frame_len = 0;
        if (h3_decode_varint(buf, len, &pos, &type) != 0 ||
            h3_decode_varint(buf, len, &pos, &frame_len) != 0 ||
            pos + frame_len > len) {
            break;
        }

        if (type == CWIST_HTTP3_FRAME_HEADERS && frame_len >= 3) {
            const unsigned char *block = buf + pos;
            /* Minimal QPACK awareness: skip required-insert-count and delta-base.
             * Full request decoding is intentionally left to a future QPACK table implementation. */
            if (block[0] == 0x00 && block[1] == 0x00) {
                req->method = CWIST_HTTP_GET;
            }
        }
        pos += frame_len;
    }
}

static int h3_send_response(SSL *stream, cwist_http_response *res) {
    /* QPACK header block: required insert count = 0, delta base = 0,
     * indexed static field line :status 200. */
    static const unsigned char headers_200[] = { 0x00, 0x00, 0xd9 };
    size_t body_len = res->body ? res->body->size : 0;
    const char *body_data = res->body ? res->body->data : "";

    if (h3_write_frame(stream, CWIST_HTTP3_FRAME_HEADERS, headers_200, sizeof(headers_200)) != 0) {
        return -1;
    }
    return h3_write_frame(stream, CWIST_HTTP3_FRAME_DATA,
                          (const unsigned char *)body_data, body_len);
}

/**
 * @brief Handle an individual QUIC stream.
 *
 * This function processes HTTP/3 frames (HEADERS, DATA) from a single QUIC stream,
 * invokes the application handler, and sends the response back over the same stream.
 *
 * @param stream SSL object representing the accepted QUIC stream.
 * @param handler Application request handler.
 * @param user_ctx Opaque pointer.
 */
static void cwist_http3_handle_stream(SSL *stream, cwist_http3_request_handler_func handler, void *user_ctx) {
    unsigned char buf[4096];
    size_t buffered = 0;

    while (buffered < sizeof(buf)) {
        int n = SSL_read(stream, buf + buffered, sizeof(buf) - buffered);
        if (n > 0) {
            buffered += (size_t)n;
            break;
        }

        int ssl_err = SSL_get_error(stream, n);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            usleep(1000);
            continue;
        }
        if (ssl_err == SSL_ERROR_ZERO_RETURN) {
            break;
        }
        break;
    }

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    if (!req || !res) {
        cwist_http_request_destroy(req);
        cwist_http_response_destroy(res);
        SSL_free(stream);
        return;
    }

    cwist_sstring_assign(req->version, "HTTP/3");
    cwist_sstring_assign(req->path, "/");
    h3_apply_minimal_request_headers(req, buf, buffered);

    if (handler) {
        handler(user_ctx, req, res);
    }

    h3_send_response(stream, res);
    SSL_stream_conclude(stream, 0);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    SSL_free(stream);
}

/**
 * @brief Handles an HTTP/3 connection over QUIC.
 *
 * This represents the QUIC connection lifecycle. It accepts multiplexed
 * QUIC streams created by the client and dispatches them.
 *
 * @param conn Connection tracking object.
 * @param user_ctx Opaque context for user state.
 * @param handler Application callback for handling HTTP requests.
 * @return cwist_error_t Execution status.
 */
cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    if (!conn || !conn->quic_ssl || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    /* Wait for and accept incoming multiplexed streams from the client */
    while (1) {
        /* SSL_ACCEPT_STREAM_NO_BLOCK is standard for async, but we'll use blocking/poll here */
        SSL *stream = SSL_accept_stream(conn->quic_ssl, 0);
        if (!stream) {
            int ssl_err = SSL_get_error(conn->quic_ssl, 0);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                /* Yield or poll underlying FD */
                struct pollfd pfd = { .fd = conn->udp_fd, .events = POLLIN };
                poll(&pfd, 1, 100);
                continue;
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                break; /* Connection closed */
            }
            break; /* Other error */
        }

        /* Process the stream. In a production server, this would be dispatched to a thread pool
         * or handled via an epoll event loop attached to the stream's readiness. */
        cwist_http3_handle_stream(stream, handler, user_ctx);
    }

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Simple HTTP/3 server loop (UDP-based).
 *
 * Binds OpenSSL's QUIC connection manager to a UDP socket, accepts
 * incoming QUIC connections, and processes their streams.
 *
 * @param udp_fd Active socket for UDP communications.
 * @param ctx Valid HTTP/3 context instance.
 * @param handler Route handler callback.
 * @param user_ctx Custom application state pointer.
 * @return cwist_error_t Runtime error status.
 */
cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (udp_fd < 0 || !ctx || !ctx->ssl_ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    printf("[HTTP/3] Listening on UDP socket %d\n", udp_fd);

    /* For an OpenSSL QUIC Server, we accept full QUIC connections over the UDP FD.
     * OpenSSL 3.2+ QUIC Server requires a BIO or setting the fd directly.
     */
    
    SSL *quic_conn = SSL_new(ctx->ssl_ctx);
    if (!quic_conn) {
        err.error.err_i16 = -1;
        return err;
    }

    SSL_set_fd(quic_conn, udp_fd);
    
    /* SSL_accept on a QUIC context handles the QUIC handshake and establishes the connection */
    if (SSL_accept(quic_conn) <= 0) {
        SSL_free(quic_conn);
        err.error.err_i16 = -1;
        return err;
    }

    cwist_http3_connection conn = {
        .quic_ssl = quic_conn,
        .udp_fd = udp_fd,
        .peer_addr_len = sizeof(struct sockaddr_storage)
    };

    /* Serve the connection (multiplexed streams) */
    cwist_http3_serve_connection(&conn, user_ctx, handler);

    SSL_shutdown(quic_conn);
    SSL_free(quic_conn);

    err.error.err_i16 = 0;
    return err;
}
