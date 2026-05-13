/**
 * @file test_compress.c
 * @brief Test compression middleware with zlib backend.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>

static void large_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Build a large repetitive body that compresses well */
    char buf[4096];
    for (size_t i = 0; i < sizeof(buf) - 1; ++i) {
        buf[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
    }
    buf[sizeof(buf) - 1] = '\0';
    cwist_sstring_assign(res->body, buf);
    res->status_code = CWIST_HTTP_OK;
}

int main(void) {
    printf("Testing compression middleware...\n");

    /* Register backends */
    cwist_compress_register_backend(cwist_compress_backend_gzip());

    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    cwist_app_use(app, cwist_mw_compress(64));
    cwist_app_get(app, "/", large_handler);

    /* Build request with Accept-Encoding */
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/");
    cwist_http_header_add(&req->headers, "Accept-Encoding", "gzip");

    cwist_app_dispatch(app, req, res);

    assert(res->status_code == CWIST_HTTP_OK);

    char *ce = cwist_http_header_get(res->headers, "Content-Encoding");
    assert(ce != NULL);
    assert(strcmp(ce, "gzip") == 0);

    /* Body should be smaller than original after compression */
    assert(res->body->size < 4096);

    /* Decompress and verify */
    z_stream zs = {0};
    assert(inflateInit2(&zs, 15 + 16) == Z_OK);

    unsigned char decompressed[8192];
    zs.next_in = (Bytef *)res->body->data;
    zs.avail_in = (uInt)res->body->size;
    zs.next_out = decompressed;
    zs.avail_out = sizeof(decompressed);

    int rc = inflate(&zs, Z_FINISH);
    assert(rc == Z_STREAM_END || rc == Z_OK);
    size_t decompressed_len = sizeof(decompressed) - zs.avail_out;
    inflateEnd(&zs);

    assert(decompressed_len == 4095);
    char expected[4095];
    for (size_t i = 0; i < sizeof(expected); ++i) {
        expected[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
    }
    assert(memcmp(decompressed, expected, sizeof(expected)) == 0);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_app_destroy(app);
    cwist_compress_unregister_all();

    printf("Passed compression middleware test.\n");
    return 0;
}
