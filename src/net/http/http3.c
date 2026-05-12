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
#include <ctype.h>

#if CWIST_HAVE_OPENSSL_QUIC

static const struct {
    const char *name;
    const char *value;
} qpack_static_table[] = {
    {":authority", ""}, {":path", "/"}, {"age", "0"},
    {"content-disposition", ""}, {"content-length", "0"}, {"cookie", ""},
    {"date", ""}, {"etag", ""}, {"if-modified-since", ""},
    {"if-none-match", ""}, {"last-modified", ""}, {"link", ""},
    {"location", ""}, {"referer", ""}, {"set-cookie", ""},
    {":method", "CONNECT"}, {":method", "DELETE"}, {":method", "GET"},
    {":method", "HEAD"}, {":method", "OPTIONS"}, {":method", "POST"},
    {":method", "PUT"}, {":scheme", "http"}, {":scheme", "https"},
    {":status", "103"}, {":status", "200"}, {":status", "304"},
    {":status", "404"}, {":status", "503"}, {"accept", "*/*"},
    {"accept", "application/dns-message"}, {"accept-encoding", "gzip, deflate, br"},
    {"accept-ranges", "bytes"}, {"access-control-allow-headers", "cache-control"},
    {"access-control-allow-headers", "content-type"}, {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"}, {"cache-control", "max-age=2592000"},
    {"cache-control", "max-age=604800"}, {"cache-control", "no-cache"},
    {"cache-control", "no-store"}, {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"}, {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"}, {"content-type", "application/javascript"},
    {"content-type", "application/json"}, {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"}, {"content-type", "image/jpeg"},
    {"content-type", "image/png"}, {"content-type", "text/css"},
    {"content-type", "text/html; charset=utf-8"}, {"content-type", "text/plain"},
    {"content-type", "text/plain;charset=utf-8"}, {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"}, {"vary", "origin"},
    {"x-content-type-options", "nosniff"}, {"x-xss-protection", "1; mode=block"},
    {":status", "100"}, {":status", "204"}, {":status", "206"},
    {":status", "302"}, {":status", "400"}, {":status", "403"},
    {":status", "421"}, {":status", "425"}, {":status", "500"},
    {"accept-language", ""}, {"access-control-allow-credentials", "FALSE"},
    {"access-control-allow-credentials", "TRUE"}, {"access-control-allow-headers", "*"},
    {"access-control-allow-methods", "get"}, {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"}, {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"}, {"access-control-request-method", "get"},
    {"access-control-request-method", "post"}, {"alt-svc", "clear"},
    {"authorization", ""}, {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"}, {"expect-ct", ""}, {"forwarded", ""},
    {"if-range", ""}, {"origin", ""}, {"purpose", "prefetch"},
    {"server", ""}, {"timing-allow-origin", "*"}, {"upgrade-insecure-requests", "1"},
    {"user-agent", ""}, {"x-forwarded-for", ""}, {"x-frame-options", "deny"},
    {"x-frame-options", "sameorigin"}
};

#define QPACK_STATIC_TABLE_COUNT (sizeof(qpack_static_table)/sizeof(qpack_static_table[0]))

static size_t qpack_encode_integer(unsigned char *dst, size_t dst_cap, uint32_t value, uint8_t prefix_bits) {
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    unsigned char first = dst[0] & ~mask;
    if (value < mask) {
        dst[0] = first | (uint8_t)value;
        return 1;
    }
    dst[0] = first | mask;
    value -= mask;
    size_t i = 1;
    while (value >= 128) {
        if (i >= dst_cap) return 0;
        dst[i++] = (unsigned char)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (i >= dst_cap) return 0;
    dst[i++] = (unsigned char)value;
    return i;
}

static size_t qpack_encode_string(unsigned char *dst, size_t dst_cap, const char *str) {
    size_t len = strlen(str);
    dst[0] = 0x00; /* literal, no huffman */
    size_t n = qpack_encode_integer(dst, dst_cap, (uint32_t)len, 7);
    if (n == 0 || n + len > dst_cap) return 0;
    memcpy(dst + n, str, len);
    return n + len;
}

static int qpack_static_table_find_name(const char *name) {
    for (size_t i = 0; i < QPACK_STATIC_TABLE_COUNT; ++i) {
        if (qpack_static_table[i].name && strcasecmp(qpack_static_table[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int qpack_static_status_index(int status_code) {
    switch (status_code) {
        case 100: return 63;
        case 103: return 24;
        case 200: return 25;
        case 204: return 64;
        case 206: return 65;
        case 302: return 66;
        case 304: return 26;
        case 400: return 67;
        case 403: return 68;
        case 404: return 27;
        case 421: return 69;
        case 425: return 70;
        case 500: return 71;
        case 503: return 28;
        default: return -1;
    }
}

static size_t qpack_encode_response_headers(cwist_http_response *res,
                                             unsigned char *dst, size_t dst_cap) {
    size_t pos = 0;
    /* Encoded Field Section Prefix: Required Insert Count = 0, Base = 0 */
    if (pos + 2 > dst_cap) return 0;
    dst[pos++] = 0x00;
    dst[pos++] = 0x00;

    /* :status */
    int status_idx = qpack_static_status_index(res->status_code);
    if (status_idx >= 0 && status_idx < 64) {
        if (pos + 1 > dst_cap) return 0;
        dst[pos++] = (unsigned char)(0xC0 | status_idx); /* Indexed Field Line, static */
    } else {
        char status_str[16];
        int status_len = snprintf(status_str, sizeof(status_str), "%d", res->status_code);
        if (pos + 1 > dst_cap) return 0;
        dst[pos] = 0x20; /* Literal Field Line with Literal Name, H=0 */
        size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, 7, 4); /* ":status" len */
        if (n == 0) return 0;
        pos += n;
        if (pos + 7 > dst_cap) return 0;
        memcpy(dst + pos, ":status", 7);
        pos += 7;
        n = qpack_encode_string(dst + pos, dst_cap - pos, status_str);
        if (n == 0) return 0;
        pos += n;
    }

    /* Auto content-length */
    size_t body_len = 0;
    if (res->use_file_stream) body_len = res->file_stream_len;
    else if (res->is_ptr_body) body_len = res->ptr_body_len;
    else if (res->body) body_len = res->body->size;

    if (!headers_have_content_length(res->headers)) {
        char cl_str[32];
        int cl_len = snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
        int name_idx = qpack_static_table_find_name("content-length");
        if (name_idx >= 0 && name_idx < 16) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x50; /* Literal with Name Reference, static, indexed name */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_idx, 4);
            if (n == 0) return 0;
            pos += n;
        } else {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x20; /* Literal with Literal Name */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, 14, 4);
            if (n == 0) return 0;
            pos += n;
            if (pos + 14 > dst_cap) return 0;
            memcpy(dst + pos, "content-length", 14);
            pos += 14;
        }
        size_t n = qpack_encode_string(dst + pos, dst_cap - pos, cl_str);
        if (n == 0) return 0;
        pos += n;
    }

    /* User headers */
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (!curr->key || !curr->key->data || !curr->value || !curr->value->data) {
            curr = curr->next;
            continue;
        }
        if (strcasecmp(curr->key->data, "connection") == 0 ||
            strcasecmp(curr->key->data, "keep-alive") == 0 ||
            strcasecmp(curr->key->data, "transfer-encoding") == 0 ||
            strcasecmp(curr->key->data, "upgrade") == 0) {
            curr = curr->next;
            continue;
        }

        char lower_name[256];
        size_t name_len = strlen(curr->key->data);
        if (name_len >= sizeof(lower_name)) name_len = sizeof(lower_name) - 1;
        for (size_t i = 0; i < name_len; ++i) {
            lower_name[i] = (char)tolower((unsigned char)curr->key->data[i]);
        }
        lower_name[name_len] = '\0';

        int name_idx = qpack_static_table_find_name(lower_name);
        if (name_idx >= 0 && name_idx < 16) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x50; /* Literal with Name Reference, static */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_idx, 4);
            if (n == 0) return 0;
            pos += n;
        } else {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x20; /* Literal with Literal Name */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_len, 4);
            if (n == 0) return 0;
            pos += n;
            if (pos + name_len > dst_cap) return 0;
            memcpy(dst + pos, lower_name, name_len);
            pos += name_len;
        }
        size_t n = qpack_encode_string(dst + pos, dst_cap - pos, curr->value->data);
        if (n == 0) return 0;
        pos += n;

        curr = curr->next;
    }

    return pos;
}

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
    unsigned char header_block[8192];
    size_t block_len = qpack_encode_response_headers(res, header_block, sizeof(header_block));
    if (block_len == 0) return -1;

    if (h3_write_frame(stream, CWIST_HTTP3_FRAME_HEADERS, header_block, block_len) != 0) {
        return -1;
    }

    size_t body_len = 0;
    const unsigned char *body_data = NULL;
    if (res->use_file_stream) {
        body_len = res->file_stream_len;
    } else if (res->is_ptr_body) {
        body_len = res->ptr_body_len;
        body_data = (const unsigned char *)res->ptr_body;
    } else if (res->body) {
        body_len = res->body->size;
        body_data = (const unsigned char *)res->body->data;
    }

    if (res->use_file_stream && res->file_stream_fd >= 0) {
        off_t offset = res->file_stream_offset;
        size_t remaining = res->file_stream_len;
        while (remaining > 0) {
            size_t chunk = remaining > 16384 ? 16384 : remaining;
            unsigned char *chunk_buf = (unsigned char *)malloc(chunk);
            if (!chunk_buf) return -1;
            ssize_t r = pread(res->file_stream_fd, chunk_buf, chunk, offset);
            if (r <= 0) { free(chunk_buf); return -1; }
            if (h3_write_frame(stream, CWIST_HTTP3_FRAME_DATA, chunk_buf, (size_t)r) != 0) {
                free(chunk_buf); return -1;
            }
            free(chunk_buf);
            offset += r;
            remaining -= (size_t)r;
        }
    } else {
        if (body_len > 0) {
            if (h3_write_frame(stream, CWIST_HTTP3_FRAME_DATA, body_data, body_len) != 0) {
                return -1;
            }
        }
    }
    return 0;
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

#else

static cwist_error_t cwist_http3_quic_unavailable(void) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx,
                                       const char *cert_path,
                                       const char *key_path) {
    (void)cert_path;
    (void)key_path;
    if (ctx) *ctx = NULL;
    return cwist_http3_quic_unavailable();
}

cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx) {
    if (ctx) *ctx = NULL;
    return cwist_http3_quic_unavailable();
}

void cwist_http3_destroy_context(cwist_http3_context *ctx) {
    if (ctx) {
        if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
        cwist_free(ctx);
    }
}

cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler) {
    (void)conn;
    (void)user_ctx;
    (void)handler;
    return cwist_http3_quic_unavailable();
}

cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx) {
    (void)udp_fd;
    (void)ctx;
    (void)handler;
    (void)user_ctx;
    return cwist_http3_quic_unavailable();
}

#endif /* CWIST_HAVE_OPENSSL_QUIC */
