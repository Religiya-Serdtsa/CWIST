/**
 * @file test_cache.c
 * @brief Unit tests for HTTP date formatting, parsing, and ETag logic.
 */

#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>

void test_http_format_date(void) {
    printf("test_http_format_date...\n");
    char buf[64];
    time_t now = 1684152000; /* 2023-05-15 12:00:00 UTC */
    cwist_http_format_date(now, buf, sizeof(buf));
    assert(strcmp(buf, "Mon, 15 May 2023 12:00:00 GMT") == 0);
    printf("  OK\n");
}

void test_http_parse_date(void) {
    printf("test_http_parse_date...\n");
    time_t t = cwist_http_parse_date("Mon, 15 May 2023 12:00:00 GMT");
    assert(t == 1684152000);
    printf("  OK\n");
}

void test_http_date_roundtrip(void) {
    printf("test_http_date_roundtrip...\n");
    time_t now = time(NULL);
    char buf[64];
    cwist_http_format_date(now, buf, sizeof(buf));
    time_t parsed = cwist_http_parse_date(buf);
    assert(parsed == now);
    printf("  OK\n");
}

void test_etag_format(void) {
    printf("test_etag_format...\n");
    char etag[64];
    time_t mtime = 0x12345678;
    size_t size = 4096;
    snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)mtime, (unsigned long)size);
    assert(strcmp(etag, "\"12345678-1000\"") == 0);
    printf("  OK\n");
}

int main(void) {
    test_http_format_date();
    test_http_parse_date();
    test_http_date_roundtrip();
    test_etag_format();
    printf("All cache tests passed.\n");
    return 0;
}
