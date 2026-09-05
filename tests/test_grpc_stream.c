#define _POSIX_C_SOURCE 200809L
/* Wire-level gRPC-over-HTTP/2 (h2c) tests: incremental DATA delivery,
 * real trailer frames, grpc-timeout enforcement, cancellation, gzip. */
#include <cwist/sys/app/app.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/net/http/http2.h>
#include <cwist/net/http/https.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

/* --- tiny HPACK literal encoder (without indexing, no huffman) --- */

static size_t te_int(uint8_t *dst, uint32_t value, uint8_t prefix_bits) {
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    if (value < mask) {
        dst[0] |= (uint8_t)value;
        return 1;
    }
    dst[0] |= mask;
    value -= mask;
    size_t i = 1;
    while (value >= 128) {
        dst[i++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    dst[i++] = (uint8_t)value;
    return i;
}

static size_t te_str(uint8_t *dst, const char *s) {
    size_t len = strlen(s);
    dst[0] = 0;
    size_t n = te_int(dst, (uint32_t)len, 7);
    memcpy(dst + n, s, len);
    return n + len;
}

/* Append "literal without indexing, new name". */
static size_t te_header(uint8_t *dst, const char *name, const char *value) {
    dst[0] = 0x00;
    size_t pos = te_int(dst, 0, 4);
    pos += te_str(dst + pos, name);
    pos += te_str(dst + pos, value);
    return pos;
}

/* --- socket helpers --- */

static void fd_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        assert(n > 0);
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static void fd_send_frame(int fd, uint8_t type, uint8_t flags,
                          uint32_t stream_id, const void *payload, uint32_t len) {
    uint8_t hdr[9];
    hdr[0] = (uint8_t)((len >> 16) & 0xff);
    hdr[1] = (uint8_t)((len >> 8) & 0xff);
    hdr[2] = (uint8_t)(len & 0xff);
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = (uint8_t)((stream_id >> 24) & 0x7f);
    hdr[6] = (uint8_t)((stream_id >> 16) & 0xff);
    hdr[7] = (uint8_t)((stream_id >> 8) & 0xff);
    hdr[8] = (uint8_t)(stream_id & 0xff);
    fd_write_all(fd, hdr, 9);
    if (len) fd_write_all(fd, payload, len);
}

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t len;
    uint8_t payload[65536];
} test_frame;

/* Read one frame; returns 0 on success, -1 on timeout/EOF. */
static int fd_read_frame(int fd, test_frame *f, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return -1;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
        ssize_t n = read(fd, hdr + got, 9 - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    f->len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
    f->type = hdr[3];
    f->flags = hdr[4];
    f->stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                   ((uint32_t)hdr[7] << 8) | hdr[8];
    assert(f->len <= sizeof(f->payload));
    got = 0;
    while (got < f->len) {
        ssize_t n = read(fd, f->payload + got, f->len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int frame_payload_contains(const test_frame *f, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || f->len < nlen) return 0;
    for (uint32_t i = 0; i + nlen <= f->len; i++)
        if (memcmp(f->payload + i, needle, nlen) == 0) return 1;
    return 0;
}

/* Read until a frame matching type+stream arrives (skipping connection
 * frames); NULL type filter reads anything. */
static int fd_read_stream_frame(int fd, test_frame *f, uint8_t want_type,
                                uint32_t stream_id, int timeout_ms) {
    for (;;) {
        if (fd_read_frame(fd, f, timeout_ms) != 0) return -1;
        if (f->type == 0x06 && !(f->flags & 0x01)) {
            /* answer server PINGs so the connection stays calm */
            fd_send_frame(fd, 0x06, 0x01, 0, f->payload, 8);
            continue;
        }
        if (f->type == want_type && f->stream_id == stream_id) return 0;
        if (f->type == 0x07) return -1; /* GOAWAY */
    }
}

/* --- test app & handlers --- */

typedef struct {
    atomic_int cancelled_seen;
    atomic_int messages_seen;
    int check_metadata;
} live_state;

static void wire_unary(cwist_http_request *req, cwist_http_response *res,
                       const cwist_grpc_message *message, void *user_ctx) {
    (void)req;
    (void)user_ctx;
    cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, message->data, message->len);
}

static void wire_live(cwist_grpc_stream *stream, void *user_ctx) {
    live_state *st = user_ctx;
    if (st->check_metadata) {
        const char *meta = cwist_grpc_metadata_get(stream->req, "x-meta-echo");
        assert(meta && strcmp(meta, "live-meta") == 0);
        uint8_t bin[8];
        size_t bin_len = 0;
        assert(cwist_grpc_metadata_get_binary(stream->req, "trace-bin",
                                              bin, sizeof(bin), &bin_len) == 0);
        assert(bin_len == 2 && bin[0] == 0xaa && bin[1] == 0xbb);
    }
    cwist_grpc_message msg;
    while (cwist_grpc_stream_recv(stream, &msg) == 1) {
        atomic_fetch_add(&st->messages_seen, 1);
        assert(cwist_grpc_stream_send(stream, msg.data, msg.len) == 0);
    }
    if (cwist_grpc_stream_cancelled(stream))
        atomic_store(&st->cancelled_seen, 1);
    cwist_grpc_stream_close(stream, stream->status, stream->status_message);
}

static void wire_block(cwist_grpc_stream *stream, void *user_ctx) {
    live_state *st = user_ctx;
    cwist_grpc_message msg;
    while (cwist_grpc_stream_recv(stream, &msg) == 1)
        ;
    if (cwist_grpc_stream_cancelled(stream))
        atomic_store(&st->cancelled_seen, 1);
    cwist_grpc_stream_close(stream, stream->status, stream->status_message);
}

static void h2_test_bridge(void *user_ctx, cwist_http_request *req,
                           cwist_http_response *res) {
    cwist_app *app = user_ctx;
    req->app = app;
    cwist_app_dispatch(app, req, res);
}

typedef struct {
    int fd;
    cwist_app *app;
    cwist_error_t result;
} server_ctx;

static void *h2_test_server(void *arg) {
    server_ctx *ctx = arg;
    cwist_https_connection conn = {
        .fd = ctx->fd,
        .ssl = NULL,
        .read_buf = NULL,
        .buf_len = 0,
        .negotiated_http2 = true,
        .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = false
    };
    ctx->result = cwist_http2_serve_connection_ex(&conn, ctx->app, h2_test_bridge,
                                                  cwist_grpc_http2_hooks());
    close(ctx->fd);
    return NULL;
}

/* Start an h2c server on a socketpair; returns the client fd. */
static int start_server(cwist_app *app, server_ctx *ctx, pthread_t *tid) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ctx->fd = sv[0];
    ctx->app = app;
    ctx->result = make_error(CWIST_ERR_INT16);
    assert(pthread_create(tid, NULL, h2_test_server, ctx) == 0);
    int cfd = sv[1];
    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    fd_write_all(cfd, preface, sizeof(preface) - 1);
    fd_send_frame(cfd, 0x04, 0, 0, NULL, 0); /* empty SETTINGS */
    /* drain server SETTINGS / WINDOW_UPDATE / PING up to our SETTINGS ACK */
    test_frame f;
    for (;;) {
        assert(fd_read_frame(cfd, &f, 5000) == 0);
        if (f.type == 0x06 && !(f.flags & 0x01))
            fd_send_frame(cfd, 0x06, 0x01, 0, f.payload, 8);
        if (f.type == 0x04 && (f.flags & 0x01)) break; /* SETTINGS ACK */
    }
    return cfd;
}

static void stop_server(int cfd, server_ctx *ctx, pthread_t tid) {
    close(cfd);
    pthread_join(tid, NULL);
    assert(ctx->result.errtype == CWIST_ERR_INT16);
    assert(ctx->result.error.err_i16 == 0);
}

/* Build a request header block; extra headers appended as literal pairs. */
static size_t build_request_block(uint8_t *dst, const char *path,
                                  const char *extra[][2], size_t extra_count) {
    size_t pos = 0;
    dst[pos++] = 0x83; /* :method POST */
    dst[pos++] = 0x86; /* :scheme http */
    pos += te_header(dst + pos, ":path", path);
    pos += te_header(dst + pos, ":authority", "localhost");
    pos += te_header(dst + pos, "content-type", "application/grpc");
    pos += te_header(dst + pos, "te", "trailers");
    for (size_t i = 0; i < extra_count; i++)
        pos += te_header(dst + pos, extra[i][0], extra[i][1]);
    return pos;
}

static void make_pb_frame(uint8_t *out, size_t *out_len, const char *text,
                          int compressed, const uint8_t *payload, size_t payload_len) {
    (void)text;
    out[0] = compressed ? 1 : 0;
    out[1] = (uint8_t)((payload_len >> 24) & 0xff);
    out[2] = (uint8_t)((payload_len >> 16) & 0xff);
    out[3] = (uint8_t)((payload_len >> 8) & 0xff);
    out[4] = (uint8_t)(payload_len & 0xff);
    memcpy(out + 5, payload, payload_len);
    *out_len = payload_len + 5;
}

/* Extract the single response DATA payload (a gRPC framed message). */
static void decode_echo_payload(const test_frame *data_frame, const uint8_t **msg,
                                uint32_t *msg_len) {
    assert(data_frame->len >= 5);
    uint32_t len = ((uint32_t)data_frame->payload[1] << 24) |
                   ((uint32_t)data_frame->payload[2] << 16) |
                   ((uint32_t)data_frame->payload[3] << 8) |
                   data_frame->payload[4];
    assert((uint32_t)(len + 5) <= data_frame->len);
    *msg = data_frame->payload + 5;
    *msg_len = len;
}

/* --- 1. unary call over the wire: real trailer HEADERS --- */
static void test_unary_trailers(cwist_app *app) {
    printf("  unary response trailers...\n");
    server_ctx ctx;
    pthread_t tid;
    int cfd = start_server(app, &ctx, &tid);

    uint8_t block[4096];
    size_t blen = build_request_block(block, "/cwist.test.Wire/Say", NULL, 0);
    fd_send_frame(cfd, 0x01, 0x04, 1, block, (uint32_t)blen); /* HEADERS */

    uint8_t frame[512];
    size_t frame_len;
    const char *payload = "\x0a\x04ping";
    make_pb_frame(frame, &frame_len, NULL, 0, (const uint8_t *)payload, strlen(payload));
    fd_send_frame(cfd, 0x00, 0x01, 1, frame, (uint32_t)frame_len); /* DATA END_STREAM */

    test_frame f;
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* response HEADERS */
    assert(!(f.flags & 0x01)); /* not END_STREAM */
    assert(frame_payload_contains(&f, "application/grpc"));
    assert(!frame_payload_contains(&f, "grpc-status")); /* status must be in trailers */

    assert(fd_read_stream_frame(cfd, &f, 0x00, 1, 5000) == 0); /* DATA */
    assert(!(f.flags & 0x01));
    const uint8_t *msg;
    uint32_t msg_len;
    decode_echo_payload(&f, &msg, &msg_len);
    assert(msg_len == strlen(payload));
    assert(memcmp(msg, payload, msg_len) == 0);

    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* trailer HEADERS */
    assert(f.flags & 0x01); /* END_STREAM */
    assert(frame_payload_contains(&f, "grpc-status"));
    assert(frame_payload_contains(&f, "0"));

    stop_server(cfd, &ctx, tid);
}

/* --- 2. streaming: split DATA frames delivered incrementally --- */
static void test_streaming_incremental(cwist_app *app, live_state *st) {
    printf("  incremental DATA delivery...\n");
    server_ctx ctx;
    pthread_t tid;
    int cfd = start_server(app, &ctx, &tid);

    const char *extra[][2] = {
        { "X-Meta-Echo", "live-meta" }, /* mixed case on purpose */
        { "trace-bin", "qrs=" },        /* base64 of 0xaa 0xbb */
    };
    uint8_t block[4096];
    size_t blen = build_request_block(block, "/cwist.test.Wire/Live", extra, 2);
    fd_send_frame(cfd, 0x01, 0x04, 1, block, (uint32_t)blen);

    test_frame f;
    /* Initial response HEADERS arrive immediately (before any message). */
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0);
    assert(!(f.flags & 0x01));
    assert(frame_payload_contains(&f, "application/grpc"));
    assert(!frame_payload_contains(&f, "grpc-status"));

    /* Message 1, split across two DATA frames, no END_STREAM yet. */
    const char *p1 = "\x0a\x03one";
    uint8_t frame1[64];
    size_t frame1_len;
    make_pb_frame(frame1, &frame1_len, NULL, 0, (const uint8_t *)p1, strlen(p1));
    fd_send_frame(cfd, 0x00, 0, 1, frame1, 3);               /* split mid-header */
    fd_send_frame(cfd, 0x00, 0, 1, frame1 + 3, (uint32_t)(frame1_len - 3));

    /* The echo must come back before the client half-closes: proof the
     * handler saw the message as it arrived, not from a buffered body. */
    assert(fd_read_stream_frame(cfd, &f, 0x00, 1, 5000) == 0);
    const uint8_t *msg;
    uint32_t msg_len;
    decode_echo_payload(&f, &msg, &msg_len);
    assert(msg_len == strlen(p1) && memcmp(msg, p1, msg_len) == 0);

    /* Message 2 in a single DATA frame. */
    const char *p2 = "\x0a\x03two";
    uint8_t frame2[64];
    size_t frame2_len;
    make_pb_frame(frame2, &frame2_len, NULL, 0, (const uint8_t *)p2, strlen(p2));
    fd_send_frame(cfd, 0x00, 0, 1, frame2, (uint32_t)frame2_len);
    assert(fd_read_stream_frame(cfd, &f, 0x00, 1, 5000) == 0);
    decode_echo_payload(&f, &msg, &msg_len);
    assert(msg_len == strlen(p2) && memcmp(msg, p2, msg_len) == 0);

    /* Half-close; expect trailer HEADERS with grpc-status 0. */
    fd_send_frame(cfd, 0x00, 0x01, 1, NULL, 0);
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0);
    assert(f.flags & 0x01);
    assert(frame_payload_contains(&f, "grpc-status"));
    assert(frame_payload_contains(&f, "0"));

    stop_server(cfd, &ctx, tid);
    assert(atomic_load(&st->messages_seen) == 2);
    assert(atomic_load(&st->cancelled_seen) == 0);
}

/* --- 3. grpc-timeout -> DEADLINE_EXCEEDED trailers --- */
static void test_deadline(cwist_app *app, live_state *st) {
    printf("  grpc-timeout enforcement...\n");
    server_ctx ctx;
    pthread_t tid;
    int cfd = start_server(app, &ctx, &tid);

    const char *extra[][2] = { { "grpc-timeout", "80m" } };
    uint8_t block[4096];
    size_t blen = build_request_block(block, "/cwist.test.Wire/Block", extra, 1);
    fd_send_frame(cfd, 0x01, 0x04, 1, block, (uint32_t)blen);

    test_frame f;
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* initial HEADERS */

    /* No messages sent; the deadline must fire on its own. */
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* trailers */
    assert(f.flags & 0x01);
    assert(frame_payload_contains(&f, "grpc-status"));
    assert(frame_payload_contains(&f, "4")); /* DEADLINE_EXCEEDED */

    stop_server(cfd, &ctx, tid);
    assert(atomic_load(&st->cancelled_seen) == 1);
}

/* --- 4. client RST_STREAM cancels the handler --- */
static void test_cancellation(cwist_app *app, live_state *st) {
    printf("  cancellation propagation...\n");
    server_ctx ctx;
    pthread_t tid;
    int cfd = start_server(app, &ctx, &tid);

    uint8_t block[4096];
    size_t blen = build_request_block(block, "/cwist.test.Wire/Block", NULL, 0);
    fd_send_frame(cfd, 0x01, 0x04, 1, block, (uint32_t)blen);

    test_frame f;
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* initial HEADERS */

    uint8_t rst[4] = { 0, 0, 0, 0x08 }; /* CANCEL */
    fd_send_frame(cfd, 0x03, 0, 1, rst, 4); /* RST_STREAM */

    /* The handler thread must observe cancellation; give it a moment. */
    for (int i = 0; i < 200 && !atomic_load(&st->cancelled_seen); i++) {
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
    }
    assert(atomic_load(&st->cancelled_seen) == 1);

    stop_server(cfd, &ctx, tid);
}

/* --- 5. gzip request messages + unsupported encoding --- */
static void test_gzip(cwist_app *app, live_state *st) {
    printf("  gzip compression negotiation...\n");
    server_ctx ctx;
    pthread_t tid;
    int cfd = start_server(app, &ctx, &tid);

    const char *extra[][2] = { { "grpc-encoding", "gzip" } };
    uint8_t block[4096];
    size_t blen = build_request_block(block, "/cwist.test.Wire/Live", extra, 1);
    fd_send_frame(cfd, 0x01, 0x04, 1, block, (uint32_t)blen);

    test_frame f;
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0);
    assert(frame_payload_contains(&f, "grpc-accept-encoding"));

    const char *p1 = "\x0a\x07zipped!";
    uint8_t zipped[256];
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    assert(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                        16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    zs.next_in = (Bytef *)p1;
    zs.avail_in = (uInt)strlen(p1);
    zs.next_out = zipped;
    zs.avail_out = sizeof(zipped);
    assert(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    size_t zipped_len = sizeof(zipped) - zs.avail_out;
    deflateEnd(&zs);

    /* Split the compressed frame across two DATA frames for good measure. */
    uint8_t zframe[300];
    size_t zframe_len;
    make_pb_frame(zframe, &zframe_len, NULL, 1, zipped, zipped_len);
    fd_send_frame(cfd, 0x00, 0, 1, zframe, 4);
    fd_send_frame(cfd, 0x00, 0x01, 1, zframe + 4, (uint32_t)(zframe_len - 4));

    /* The echo comes back decompressed. */
    assert(fd_read_stream_frame(cfd, &f, 0x00, 1, 5000) == 0);
    const uint8_t *msg;
    uint32_t msg_len;
    decode_echo_payload(&f, &msg, &msg_len);
    assert(msg_len == strlen(p1) && memcmp(msg, p1, msg_len) == 0);

    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* trailers */
    assert(frame_payload_contains(&f, "grpc-status"));
    assert(frame_payload_contains(&f, "0"));

    stop_server(cfd, &ctx, tid);
    assert(atomic_load(&st->messages_seen) == 1);
}

static void test_unsupported_encoding(cwist_app *app) {
    printf("  unsupported grpc-encoding rejection...\n");
    server_ctx ctx;
    pthread_t tid;
    int cfd = start_server(app, &ctx, &tid);

    const char *extra[][2] = { { "grpc-encoding", "snappy" } };
    uint8_t block[4096];
    size_t blen = build_request_block(block, "/cwist.test.Wire/Live", extra, 1);
    fd_send_frame(cfd, 0x01, 0x04, 1, block, (uint32_t)blen);

    const char *p1 = "\x0a\x03one";
    uint8_t frame1[64];
    size_t frame1_len;
    make_pb_frame(frame1, &frame1_len, NULL, 0, (const uint8_t *)p1, strlen(p1));
    fd_send_frame(cfd, 0x00, 0x01, 1, frame1, (uint32_t)frame1_len);

    test_frame f;
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* response HEADERS */
    assert(!frame_payload_contains(&f, "grpc-status"));
    assert(fd_read_stream_frame(cfd, &f, 0x01, 1, 5000) == 0); /* trailers */
    assert(f.flags & 0x01);
    assert(frame_payload_contains(&f, "grpc-status"));
    assert(frame_payload_contains(&f, "12")); /* UNIMPLEMENTED */

    stop_server(cfd, &ctx, tid);
}

int main(void) {
    printf("Testing gRPC HTTP/2 streaming...\n");

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    static live_state live_st, deadline_st, cancel_st, gzip_st;
    atomic_store(&live_st.cancelled_seen, 0);
    atomic_store(&live_st.messages_seen, 0);
    live_st.check_metadata = 1;
    atomic_store(&deadline_st.cancelled_seen, 0);
    atomic_store(&deadline_st.messages_seen, 0);
    deadline_st.check_metadata = 0;
    atomic_store(&cancel_st.cancelled_seen, 0);
    atomic_store(&cancel_st.messages_seen, 0);
    cancel_st.check_metadata = 0;
    atomic_store(&gzip_st.cancelled_seen, 0);
    atomic_store(&gzip_st.messages_seen, 0);
    gzip_st.check_metadata = 0;

    assert(cwist_app_grpc_unary(app, "cwist.test.Wire", "Say", wire_unary, NULL) == 0);
    assert(cwist_app_grpc_stream(app, "cwist.test.Wire", "Live", wire_live, &live_st) == 0);
    assert(cwist_app_grpc_stream(app, "cwist.test.Wire", "Block", wire_block, &deadline_st) == 0);

    test_unary_trailers(app);
    test_streaming_incremental(app, &live_st);
    test_deadline(app, &deadline_st);

    /* Cancellation uses the Block handler; swap in the cancel flag state. */
    assert(cwist_app_grpc_stream(app, "cwist.test.Wire", "Block", wire_block, &cancel_st) == 0);
    test_cancellation(app, &cancel_st);

    assert(cwist_app_grpc_stream(app, "cwist.test.Wire", "Live", wire_live, &gzip_st) == 0);
    gzip_st.check_metadata = 0;
    test_gzip(app, &gzip_st);
    test_unsupported_encoding(app);

    cwist_app_destroy(app);
    printf("test_grpc_stream: OK\n");
    return 0;
}
