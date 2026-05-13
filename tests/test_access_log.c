/**
 * @file test_access_log.c
 * @brief Unit tests for access log middleware formats.
 */

#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "OK");
}

static char *capture_access_log(cwist_middleware_func mw) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->client_fd = -1;

    FILE *tmp = tmpfile();
    assert(tmp != NULL);
    int fd = fileno(tmp);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);

    mw(req, res, dummy_next);

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    rewind(tmp);
    static char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    return buf;
}

void test_access_log_common(void) {
    printf("test_access_log_common...\n");
    char *out = capture_access_log(cwist_mw_access_log(CWIST_LOG_COMMON));
    assert(strstr(out, "GET") != NULL);
    assert(strstr(out, "/") != NULL);
    assert(strstr(out, "200") != NULL);
    assert(strstr(out, "2") != NULL); /* at least 2 bytes body */
    printf("  OK\n");
}

void test_access_log_combined(void) {
    printf("test_access_log_combined...\n");
    char *out = capture_access_log(cwist_mw_access_log(CWIST_LOG_COMBINED));
    assert(strstr(out, "GET") != NULL);
    assert(strstr(out, "200") != NULL);
    assert(strstr(out, "\"") != NULL); /* quoted fields present */
    printf("  OK\n");
}

void test_access_log_json(void) {
    printf("test_access_log_json...\n");
    char *out = capture_access_log(cwist_mw_access_log(CWIST_LOG_JSON));
    assert(strstr(out, "\"time\"") != NULL);
    assert(strstr(out, "\"method\":\"GET\"") != NULL);
    assert(strstr(out, "\"status\":200") != NULL);
    assert(strstr(out, "\"duration_ms\"") != NULL);
    printf("  OK\n");
}

int main(void) {
    test_access_log_common();
    test_access_log_combined();
    test_access_log_json();
    printf("All access log tests passed.\n");
    return 0;
}
