/**
 * @file http2.c
 * @brief Implementation of HTTP/2 protocol handler for CWIST.
 * @author Lee Yunjin
 * @date 2026-04-27
 */

#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>

#include <cwist/net/http/http2.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* --- Internal Macros --- */
#define CWIST_HTTP2_FRAME_HEADER_SIZE 9
#define CWIST_HTTP2_CONNECTION_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define CWIST_HTTP2_CONNECTION_PREFACE_LEN 24
#define CWIST_HTTP2_MAX_FRAME_SIZE 16384
#define CWIST_HTTP2_MAX_CONCURRENT_STREAMS 32

#define CWIST_HTTP2_FLAG_END_STREAM 0x01
#define CWIST_HTTP2_FLAG_END_HEADERS 0x04
#define CWIST_HTTP2_FLAG_PADDED 0x08
#define CWIST_HTTP2_FLAG_PRIORITY 0x20
#define CWIST_HTTP2_FLAG_ACK 0x01
#define CWIST_HTTP2_FRAME_DATA 0x00
#define CWIST_HTTP2_FRAME_HEADERS 0x01
#define CWIST_HTTP2_FRAME_SETTINGS 0x04

/**
 * @brief Internal state for an HTTP/2 connection.
 *
 * Maintains the SSL context, connection preface status,
 * and stream management information.
 */
typedef struct {
    SSL *ssl;                 /**< Pointer to the underlying OpenSSL SSL object */
    bool preface_received;    /**< Flag indicating if the HTTP/2 connection preface was received */
    uint32_t last_stream_id;  /**< The highest stream ID seen on this connection */
} cwist_http2_conn_internal;

/* --- Private Function Prototypes --- */

typedef struct {
    const char *name;
    const char *value;
} cwist_http2_static_header;

static const cwist_http2_static_header cwist_http2_static_table[] = {
    { NULL, NULL },
    { ":authority", "" },
    { ":method", "GET" },
    { ":method", "POST" },
    { ":path", "/" },
    { ":path", "/index.html" },
    { ":scheme", "http" },
    { ":scheme", "https" },
    { ":status", "200" },
    { ":status", "204" },
    { ":status", "206" },
    { ":status", "304" },
    { ":status", "400" },
    { ":status", "404" },
    { ":status", "500" },
    { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" },
    { "accept-language", "" },
    { "accept-ranges", "" },
    { "accept", "" },
    { "access-control-allow-origin", "" },
    { "age", "" },
    { "allow", "" },
    { "authorization", "" },
    { "cache-control", "" },
    { "content-disposition", "" },
    { "content-encoding", "" },
    { "content-language", "" },
    { "content-length", "" },
    { "content-location", "" },
    { "content-range", "" },
    { "content-type", "" },
    { "cookie", "" },
    { "date", "" },
    { "etag", "" },
    { "expect", "" },
    { "expires", "" },
    { "from", "" },
    { "host", "" },
};

static int h2_read(cwist_https_connection *conn, void *buf, int len) {
    if (conn->ssl) return SSL_read(conn->ssl, buf, len);
    return read(conn->fd, buf, len);
}

static int h2_write(cwist_https_connection *conn, const void *buf, int len) {
    if (conn->ssl) return SSL_write(conn->ssl, buf, len);
    return write(conn->fd, buf, len);
}

static int h2_write_all(cwist_https_connection *conn, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        int n = h2_write(conn, p, (int)len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void h2_make_frame_header(unsigned char out[CWIST_HTTP2_FRAME_HEADER_SIZE],
                                 uint32_t len,
                                 uint8_t type,
                                 uint8_t flags,
                                 uint32_t stream_id) {
    out[0] = (unsigned char)((len >> 16) & 0xff);
    out[1] = (unsigned char)((len >> 8) & 0xff);
    out[2] = (unsigned char)(len & 0xff);
    out[3] = type;
    out[4] = flags;
    out[5] = (unsigned char)((stream_id >> 24) & 0x7f);
    out[6] = (unsigned char)((stream_id >> 16) & 0xff);
    out[7] = (unsigned char)((stream_id >> 8) & 0xff);
    out[8] = (unsigned char)(stream_id & 0xff);
}

static int h2_write_frame(cwist_https_connection *conn,
                          uint8_t type,
                          uint8_t flags,
                          uint32_t stream_id,
                          const unsigned char *payload,
                          uint32_t len) {
    unsigned char frame[CWIST_HTTP2_FRAME_HEADER_SIZE];
    h2_make_frame_header(frame, len, type, flags, stream_id);
    if (h2_write_all(conn, frame, sizeof(frame)) != 0) return -1;
    if (len > 0 && payload) return h2_write_all(conn, payload, len);
    return 0;
}

static int h2_decode_integer(const unsigned char *buf,
                             size_t len,
                             size_t *pos,
                             uint8_t prefix_bits,
                             uint32_t *value) {
    if (*pos >= len || prefix_bits == 0 || prefix_bits > 8) return -1;
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    uint32_t n = buf[*pos] & mask;
    (*pos)++;
    if (n < mask) {
        *value = n;
        return 0;
    }

    uint32_t m = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        if (m > 28) return -1;
        n += (uint32_t)(b & 0x7f) << m;
        if ((b & 0x80) == 0) {
            *value = n;
            return 0;
        }
        m += 7;
    }
    return -1;
}

static char *h2_decode_string(const unsigned char *buf, size_t len, size_t *pos) {
    if (*pos >= len) return NULL;
    bool huffman = (buf[*pos] & 0x80) != 0;
    uint32_t str_len = 0;
    if (h2_decode_integer(buf, len, pos, 7, &str_len) != 0) return NULL;
    if (huffman || *pos + str_len > len) return NULL;

    char *out = (char *)malloc((size_t)str_len + 1);
    if (!out) return NULL;
    memcpy(out, buf + *pos, str_len);
    out[str_len] = '\0';
    *pos += str_len;
    return out;
}

static const cwist_http2_static_header *h2_static_header(uint32_t index) {
    size_t count = sizeof(cwist_http2_static_table) / sizeof(cwist_http2_static_table[0]);
    if (index == 0 || index >= count) return NULL;
    return &cwist_http2_static_table[index];
}

static void h2_apply_header(cwist_http_request *req, const char *name, const char *value) {
    if (!req || !name || !value) return;

    if (strcmp(name, ":method") == 0) {
        req->method = cwist_http_string_to_method(value);
    } else if (strcmp(name, ":path") == 0) {
        cwist_sstring_assign(req->path, (char *)value);
    } else if (strcmp(name, ":authority") == 0 || strcmp(name, "host") == 0) {
        cwist_http_header_add(&req->headers, "Host", value);
    } else if (strcmp(name, ":scheme") != 0 && name[0] != ':') {
        cwist_http_header_add(&req->headers, name, value);
    }
}

static void h2_decode_header_block(cwist_http_request *req, const unsigned char *payload, size_t len) {
    size_t pos = 0;

    while (pos < len) {
        uint8_t b = payload[pos];
        uint32_t name_index = 0;
        char *name = NULL;
        char *value = NULL;

        if (b & 0x80) {
            uint32_t index = 0;
            if (h2_decode_integer(payload, len, &pos, 7, &index) != 0) break;
            const cwist_http2_static_header *entry = h2_static_header(index);
            if (entry && entry->name) h2_apply_header(req, entry->name, entry->value);
            continue;
        }

        if ((b & 0x40) == 0x40) {
            if (h2_decode_integer(payload, len, &pos, 6, &name_index) != 0) break;
        } else if ((b & 0xf0) == 0x00) {
            if (h2_decode_integer(payload, len, &pos, 4, &name_index) != 0) break;
        } else if ((b & 0xf0) == 0x10) {
            if (h2_decode_integer(payload, len, &pos, 4, &name_index) != 0) break;
        } else {
            break;
        }

        if (name_index > 0) {
            const cwist_http2_static_header *entry = h2_static_header(name_index);
            if (!entry || !entry->name) break;
            name = strdup(entry->name);
        } else {
            name = h2_decode_string(payload, len, &pos);
        }
        value = h2_decode_string(payload, len, &pos);
        if (!name || !value) {
            free(name);
            free(value);
            break;
        }
        h2_apply_header(req, name, value);
        free(name);
        free(value);
    }
}

static int h2_send_response(cwist_https_connection *conn, uint32_t stream_id, cwist_http_response *res) {
    unsigned char status_200[] = { 0x88 };
    size_t body_len = res->body ? res->body->size : 0;
    const char *body_data = res->body ? res->body->data : "";
    uint8_t header_flags = CWIST_HTTP2_FLAG_END_HEADERS;
    if (body_len == 0) header_flags |= CWIST_HTTP2_FLAG_END_STREAM;

    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_HEADERS, header_flags, stream_id,
                       status_200, sizeof(status_200)) != 0) {
        return -1;
    }

    size_t sent = 0;
    while (sent < body_len) {
        size_t remaining = body_len - sent;
        uint32_t chunk = (uint32_t)(remaining > CWIST_HTTP2_MAX_FRAME_SIZE ?
                                   CWIST_HTTP2_MAX_FRAME_SIZE : remaining);
        uint8_t flags = (sent + chunk == body_len) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
        if (h2_write_frame(conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                           (const unsigned char *)body_data + sent, chunk) != 0) {
            return -1;
        }
        sent += chunk;
    }
    return 0;
}

/**
 * @brief Verifies the mandatory HTTP/2 connection preface.
 *
 * @param conn Pointer to the connection to read from.
 * @return 0 on success (preface matches), -1 on failure.
 */
static int cwist_http2_verify_preface(cwist_https_connection *conn) {
    char buffer[CWIST_HTTP2_CONNECTION_PREFACE_LEN];
    int offset = 0;
    while (offset < CWIST_HTTP2_CONNECTION_PREFACE_LEN) {
        int n = h2_read(conn, buffer + offset, CWIST_HTTP2_CONNECTION_PREFACE_LEN - offset);
        if (n <= 0) return -1;
        offset += n;
    }

    if (memcmp(buffer, CWIST_HTTP2_CONNECTION_PREFACE, CWIST_HTTP2_CONNECTION_PREFACE_LEN) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief Serves an HTTP/2 connection.
 *
 * Processes HTTP/2 frames, decodes HPACK headers (minimal support for testing),
 * triggers the appropriate user-defined request handler, and multiplexes
 * the response back to the client. Works with both TLS (h2) and cleartext (h2c).
 *
 * @param conn Pointer to the HTTPS connection structure (ssl can be NULL for h2c).
 * @param user_ctx Opaque user context to pass to the handler.
 * @param handler Function pointer to the user's HTTP request handler.
 * @return cwist_error_t Structure indicating success or failure.
 */
cwist_error_t cwist_http2_serve_connection(
    cwist_https_connection *conn,
    void *user_ctx,
    cwist_http2_request_handler_func handler
) {
    cwist_error_t result;

    /* 1. Validation */
    if (!conn || !handler) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    /* 2. Verify HTTP/2 Connection Preface */
    if (cwist_http2_verify_preface(conn) != 0) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_SETTINGS, 0, 0, NULL, 0) != 0) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    /* 3. Main Event Loop */
    bool connected = true;
    while (connected) {
        unsigned char hdr[9];
        int n = 0;
        int offset = 0;
        // Read 9-byte frame header
        while (offset < 9) {
            n = h2_read(conn, hdr + offset, 9 - offset);
            if (n <= 0) { connected = false; break; }
            offset += n;
        }
        if (!connected) break;

        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 8) | hdr[8];

        if (len > CWIST_HTTP2_MAX_FRAME_SIZE) {
            connected = false;
            break;
        }

        unsigned char *payload = NULL;
        if (len > 0) {
            payload = malloc(len);
            int off = 0;
            while (off < (int)len) {
                int r = h2_read(conn, payload + off, len - off);
                if (r <= 0) { connected = false; break; }
                off += r;
            }
        }
        if (!connected) { free(payload); break; }

        if (type == CWIST_HTTP2_FRAME_SETTINGS) {
            if ((hdr[4] & CWIST_HTTP2_FLAG_ACK) == 0) {
                h2_write_frame(conn, CWIST_HTTP2_FRAME_SETTINGS, CWIST_HTTP2_FLAG_ACK, 0, NULL, 0);
            }
        } else if (type == CWIST_HTTP2_FRAME_HEADERS && stream_id != 0) {
            cwist_http_request *req = cwist_http_request_create();
            if (!req) { free(payload); break; }
            cwist_sstring_assign(req->version, "HTTP/2");

            size_t block_offset = 0;
            size_t block_len = len;
            if ((hdr[4] & CWIST_HTTP2_FLAG_PADDED) != 0 && block_len > 0) {
                uint8_t pad_len = payload[block_offset++];
                block_len--;
                if (pad_len <= block_len) block_len -= pad_len;
            }
            if ((hdr[4] & CWIST_HTTP2_FLAG_PRIORITY) != 0 && block_len >= 5) {
                block_offset += 5;
                block_len -= 5;
            }
            if (payload && block_offset <= len) {
                h2_decode_header_block(req, payload + block_offset, block_len);
            }

            cwist_http_response *res = cwist_http_response_create();
            if (res) {
                handler(user_ctx, req, res);
                if (h2_send_response(conn, stream_id, res) != 0) connected = false;
                cwist_http_response_destroy(res);
            }

            cwist_http_request_destroy(req);
        }

        free(payload);
    }

    result = make_error(CWIST_ERR_INT16);
    result.error.err_i16 = 0;
    return result;
}
